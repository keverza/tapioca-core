#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ApiDispatcher.hpp"
#include "DeferredBindings.hpp"
#include "DispatcherError.hpp"
#include "DispatcherVerbs.hpp"
#include "MainThreadGate.hpp"
#include "PathUtils.hpp"
#include "Diagnostics/ApiError.hpp" // DescribeErr, CallScope, LogEnvelopeFailure
#include "AddOnCommands.hpp"
#include "NativeCommands/CommandSchemas.hpp"
#include "NativeCommands/SchemaValidator.hpp"
#include "PythonHost.hpp" // evp::ReportAlert
#include "RunCancel.hpp"  // E9 — the running command's cancel token
#include "WebUIDispatch.hpp"
#include "WebUIRunState.hpp"
#include "../Notify/ChangeTracker.hpp"   // E25 — Archicad does not report OUR OWN writes
#include "../Palette/ControlPalette.hpp" // evp.ui.status -> the palette's status line

#include "ObjectState.hpp"
#include "ObjectStateJSONConversion.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>

namespace {

std::atomic<bool> tracingEnabled { false };
std::atomic<uint64_t> nextCallId { 1 };

double MillisSince (const std::chrono::steady_clock::time_point& start)
{
    return std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - start).count ();
}

void Trace (const GS::UniString& line)
{
    if (!tracingEnabled.load ())
        return;
    const GS::UniString dataDir = evp::EvpDataDir ();
    if (!dataDir.IsEmpty ())
        evp::AppendTextLine (dataDir + GS::UniString ("\\logs\\api_trace.log"), line);
}

GS::UniString ToJson (const GS::ObjectState& os)
{
    GS::UniString json;
    if (JSON::CreateFromObjectState (os, json) != NoError) {
        // Hand-built so that even a serializer failure is a valid envelope —
        // the Python side must never receive a non-JSON body.
        return GS::UniString ("{\"ok\":false,\"data\":null,\"error\":{\"code\":\"SerializationFailed\","
                              "\"message\":\"Could not serialize the response.\",\"detail\":null},\"meta\":{}}");
    }
    return json;
}

// Splits "Tapioca.GetStatus" into ("Tapioca", "GetStatus").
// True for native commands that touch no ACAPI and can therefore run inline on
// the caller's thread — the E2 geometry queries. Anything unknown is treated as
// needing the main thread, which is the safe direction to be wrong in.
bool IsGateFreeCommand (const GS::UniString& name)
{
    bool needsMainThread = true;
    if (!geomsrv::CommandNeedsMainThread (GS::String (name.ToCStr ().Get ()), needsMainThread))
        return false;
    return !needsMainThread;
}

// E9 — verbs that still work AFTER the run has been cancelled.
//
// Once a run is cancelled every other bus call is refused with a "Cancelled"
// envelope (see DispatchApiCall), which is what makes an ordinary command
// unwind without having to opt in: the next thing it asks Archicad for raises
// evp.Cancelled. These few must stay open anyway, or the command could neither
// notice the cancel nor report it:
//   * PollCancel/PollSelectionPrompt — the checkpoints themselves;
//   * SetTracing — bus-local, touches nothing;
//   * SetStatus/ShowAlert — how a command says "cancelled" on its way out;
//   * HideSelectionPrompt — ui.request_selection()'s finally-block cleanup,
//     which would otherwise strand the Continue button on screen.
//   * GetErrorTrail — a command's except/finally asking what went wrong. If a
//     cancel refused this one, the diagnostic would be unavailable in precisely
//     the case you most want it: a run stopped because it was misbehaving.
//   * WatchModel — E25's `finally` cleanup, and the same case as
//     HideSelectionPrompt: it is how a command turns OFF the element observer
//     and stops any arming pass. Measured 2026-08-04 — the user pressed Stop and
//     the probe's `evp.changes.unwatch()` was refused with Cancelled, LEAVING THE
//     WATCH ARMED after the run had ended. A verb whose job is "stop the thing
//     that outlives me" must not be the one a cancel disables.
// ShowResults is deliberately NOT here: a cancelled run has no results to show.
bool IsCancelExemptVerb (const GS::UniString& backend, const GS::UniString& name)
{
    if (backend != "EvP")
        return false;
    if (const evp::DispatcherVerbRegistration* verb = evp::FindDispatcherVerb (name))
        return verb->cancelExempt;
    return geomsrv::IsCancelExemptNativeCommand (GS::String (name.ToCStr ().Get ()));
}

bool SplitCommand (const GS::UniString& command, GS::UniString& backend, GS::UniString& name)
{
    const UIndex dot = command.FindFirst ('.');
    if (dot == MaxUIndex || dot == 0 || dot + 1 >= command.GetLength ())
        return false;
    backend = command.GetSubstring (0, dot);
    name = command.GetSubstring (dot + 1, command.GetLength () - dot - 1);
    return true;
}

const char* NativeFailureCode (const geomsrv::NativeCommandResult& result)
{
    return result.failureKind == geomsrv::NativeCommandFailureKind::SchemaValidation ? "SchemaValidationFailed"
                                                                                     : "CommandFailed";
}

bool ValidateDispatcherSchema (const GS::UniString& name, const GS::ObjectState& value, bool input,
                               GS::UniString& error)
{
    const GS::String commandName (name.ToCStr ().Get ());
    return geomsrv::ValidateObjectStateSchema (
        value, input ? geomsrv::GetNativeInputSchema (commandName) : geomsrv::GetNativeResponseSchema (commandName),
        error);
}

// ---------------------------------------------------------------------------
// Transaction replay: the whole batch inside ONE ACAPI_CallUndoableCommand.
//
// Atomicity comes free from Archicad: if any step fails we return an error from
// the lambda, and Archicad rolls back everything the batch did. No compensating
// deletes, no residue, and one undo step for the user when it succeeds.
//
// MAIN THREAD ONLY — the undo scope only exists inside this lambda, which is
// precisely why writes are recorded in Zone B and replayed here.
struct ReplayOutcome {
    bool allOk = true;
    UIndex failedIndex = 0;
    GS::UniString failedCommand;
    GS::UniString failedError;
    GS::Array<GS::UniString> results; // per-step response JSON
};

void ReplayBatch (const GS::UniString& name, const GS::Array<GS::UniString>& steps, ReplayOutcome& outcome)
{
    // Kept as ObjectStates so a later step's binding can read an earlier step's
    // result without reparsing its JSON.
    GS::Array<GS::ObjectState> responses;

    const GSErrCode undoErr = ACAPI_CallUndoableCommand (name, [&] () -> GSErrCode {
        for (UIndex i = 0; i < steps.GetSize (); ++i) {
            GS::ObjectState step;
            if (JSON::ConvertToObjectState (steps[i], step) != NoError) {
                outcome.allOk = false;
                outcome.failedIndex = i;
                outcome.failedError = "step is not valid JSON";
                return APIERR_BADPARS; // -> rolls the whole batch back
            }

            GS::UniString stepCommand;
            step.Get ("command", stepCommand);
            GS::ObjectState stepParams;
            step.Get ("params", stepParams);

            // Deferred handles resolve mid-batch, at the only moment an earlier
            // step's result exists. The helper handles typed, nested paths and
            // rebuilds the ObjectState before ExecuteNativeCommand validates it.
            GS::Array<GS::UniString> bindingJsons;
            step.Get ("bindings", bindingJsons);

            // The step declares how many bindings it sent. If we read a different
            // number, ObjectState::Get silently rejected the wire format (it
            // returns false on a type mismatch and leaves the array empty) — and
            // the bindings would vanish, surfacing much later as the command
            // complaining about a parameter it never received. Fail HERE, naming
            // the real cause.
            GS::Int32 declaredBindings = 0;
            step.Get ("bindingCount", declaredBindings);
            if ((GS::Int32) bindingJsons.GetSize () != declaredBindings) {
                outcome.allOk = false;
                outcome.failedIndex = i;
                outcome.failedCommand = stepCommand;
                outcome.failedError = GS::UniString::Printf (
                    "wire format mismatch: the step declared %d binding(s) but %u could be read. "
                    "Bindings must cross as an array of JSON strings.",
                    (int) declaredBindings, (unsigned) bindingJsons.GetSize ());
                return APIERR_BADPARS;
            }

            for (const GS::UniString& bindingJson : bindingJsons) {
                GS::ObjectState binding;
                if (JSON::ConvertToObjectState (bindingJson, binding) != NoError) {
                    outcome.allOk = false;
                    outcome.failedIndex = i;
                    outcome.failedError = "binding is not valid JSON";
                    return APIERR_BADPARS;
                }

                GS::UniString path, key;
                GS::Int32 sourceStep = -1;
                binding.Get ("path", path);
                binding.Get ("key", key);
                binding.Get ("step", sourceStep);

                if (sourceStep < 0 || (UIndex) sourceStep >= responses.GetSize ()) {
                    outcome.allOk = false;
                    outcome.failedIndex = i;
                    outcome.failedCommand = stepCommand;
                    outcome.failedError =
                        GS::UniString::Printf ("handle refers to step %d, which has not run", (int) sourceStep);
                    return APIERR_BADPARS;
                }

                GS::UniString bindingError;
                if (!evp::ApplyDeferredBinding (responses[sourceStep], key, stepParams, path, bindingError)) {
                    outcome.allOk = false;
                    outcome.failedIndex = i;
                    outcome.failedCommand = stepCommand;
                    outcome.failedError =
                        GS::UniString::Printf ("could not bind step %d '%T' to '%T': %T", (int) sourceStep,
                                               key.ToPrintf (), path.ToPrintf (), bindingError.ToPrintf ());
                    return APIERR_BADPARS;
                }
            }

            GS::UniString backend;
            GS::UniString commandName;
            if (!SplitCommand (stepCommand, backend, commandName) || (backend != "Tapioca" && backend != "EvP")) {
                outcome.allOk = false;
                outcome.failedIndex = i;
                outcome.failedCommand = stepCommand;
                outcome.failedError = "only Tapioca.* commands can take part in a transaction";
                return APIERR_BADPARS;
            }

            // A STRUCTURAL command cannot take part in a transaction, and this is
            // the one place that can say so usefully. Those ACAPI calls
            // (NewDatabase / DeleteDatabase / CreateLayout / CreateSubSet) refuse
            // to run inside an undo scope and we are INSIDE one right now — so
            // letting the step through would surface as a bare APIERR_REFUSEDCMD
            // from a batch, with nothing naming the real cause. Refuse by name
            // instead, and say what to do about it. See StructuralCommand.
            bool isStructural = false;
            if (geomsrv::IsStructuralCommand (GS::String (commandName.ToCStr ().Get ()), isStructural) &&
                isStructural) {
                outcome.allOk = false;
                outcome.failedIndex = i;
                outcome.failedCommand = stepCommand;
                outcome.failedError = GS::UniString::Printf (
                    "%T is a STRUCTURAL command and cannot run inside evp.transaction: Archicad refuses "
                    "non-undoable data-structure modifiers (create/delete database, layout, subset) while an "
                    "undo scope is open. Call it directly with evp.api.call instead - and note it will NOT be "
                    "undoable, so there is nothing a transaction could roll back for it anyway.",
                    stepCommand.ToPrintf ());
                return APIERR_BADPARS; // -> rolls the rest of the batch back
            }

            // Stamp everything this step reports with WHICH step it was. Without
            // it a failure five steps into a batch is indistinguishable from one
            // in step zero, and the whole batch rolls back either way — so the
            // log is the only place that difference survives.
            const evp::CallScope stepScope (GS::UniString::Printf ("tx#%u", (unsigned) i), stepCommand, steps[i]);

            const geomsrv::NativeCommandResult result =
                geomsrv::ExecuteNativeCommand (GS::String (commandName.ToCStr ().Get ()), stepParams);
            if (!result.ok) {
                outcome.allOk = false;
                outcome.failedIndex = i;
                outcome.failedCommand = stepCommand;
                outcome.failedError = result.error;
                // THE load-bearing line: a non-NoError return makes Archicad
                // discard everything this undoable command did, including the
                // steps that already succeeded.
                return APIERR_GENERAL;
            }

            responses.Push (result.data); // so later steps can bind to it

            GS::UniString responseJson;
            JSON::CreateFromObjectState (result.data, responseJson);
            outcome.results.Push (responseJson);
        }
        return NoError;
    });

    // A rolled-back batch reports the ORIGINATING step failure, not the undo
    // machinery's error code — the script needs to know which step broke.
    if (undoErr != NoError && outcome.allOk) {
        outcome.allOk = false;
        outcome.failedError =
            EVP_ACAPI_FAIL ("ACAPI_CallUndoableCommand", undoErr,
                            GS::UniString::Printf ("transaction '%T', %u step(s), every step reported success "
                                                   "- so the undo scope itself refused the batch",
                                                   name.ToPrintf (), (unsigned) steps.GetSize ()));
    }
}

} // namespace

namespace evp {

void SetApiTracing (bool enabled)
{
    tracingEnabled.store (enabled);
}
bool IsApiTracingEnabled ()
{
    return tracingEnabled.load ();
}

GS::UniString DispatchApiCall (const GS::UniString& command, const GS::UniString& paramsJson, const GS::UniString& zone)
{
    const auto started = std::chrono::steady_clock::now ();
    const GS::UniString callId = GS::UniString::Printf ("c%llu", (unsigned long long) nextCallId.fetch_add (1));

    // Everything reported from here down — by this function, or by a native
    // command it reaches — is stamped with this call. Tracing is a toggle a
    // developer has to remember to switch on; the error trail is not optional,
    // because the run you needed it for has already happened by the time you
    // think to enable anything.
    const evp::CallScope callScope (callId, command, paramsJson);

    Trace (GS::UniString::Printf ("--> [%T] %T %T", callId.ToPrintf (), command.ToPrintf (), paramsJson.ToPrintf ()));

    GS::ObjectState data;
    GS::ObjectState error;
    bool ok = false;
    double mainThreadMs = 0.0;
    GS::UniString backendName ("none");

    GS::UniString backend;
    GS::UniString name;
    GS::UniString dispatcherSchemaError;

    GS::ObjectState params;
    const bool validCommand = SplitCommand (command, backend, name);
    if (validCommand && backend == "Tapioca")
        backend = "EvP"; // API 2 canonical native namespace.

    if (!validCommand) {
        error = MakeError ("BadCommand", "Command must be '<Backend>.<Name>', e.g. 'Tapioca.GetStatus'.", command);
    }
    else if (!paramsJson.IsEmpty () && JSON::ConvertToObjectState (paramsJson, params) != NoError) {
        error = MakeError ("BadParams", "params is not valid JSON.", paramsJson);
    }
    else if (backend == "EvP" && FindDispatcherVerb (name) != nullptr &&
             !ValidateDispatcherSchema (name, params, true, dispatcherSchemaError)) {
        backendName = "native";
        error = MakeError ("SchemaValidationFailed", "Invalid parameters: " + dispatcherSchemaError, command);
    }
    else if (!IsCancelExemptVerb (backend, name) && RunCancel::Get ().IsCancelled ()) {
        // E9 ENFORCEMENT — the run has been cancelled, so refuse everything the
        // command asks for from here on. Python's api.call maps code=="Cancelled"
        // onto evp.Cancelled, so the command unwinds through its own finally
        // blocks whether or not its author ever wrote a check_cancel() call.
        //
        // Refusing BEFORE the job is queued is deliberate, and it is why the gate
        // itself is left alone: a job already dispatched onto the main thread is
        // running real work (a transaction replay is one atomic
        // ACAPI_CallUndoableCommand), and abandoning the wait would not stop it —
        // it would only make Python believe a write was cancelled that in fact
        // committed. So the in-flight hop finishes, the NEXT call is refused, and
        // stop latency is one hop rather than the gate's 30 s timeout. Cancelling
        // BETWEEN transactions is clean; inside one is not interruptible by design.
        backendName = "native";
        error = MakeError ("Cancelled",
                           GS::UniString ("The command was cancelled — ") +
                               RunCancel::ReasonText (RunCancel::Get ().Reason ()) + ".",
                           command);
    }
    else if (DispatchWebUIVerb (backend, name, command, params, data, error, ok)) {
        backendName = "native";
    }
    else if (backend == "EvP" && name == "SetTracing") {
        // Handled by the bus itself: it toggles the bus, touches no ACAPI, and
        // must not cost a main-thread hop.
        backendName = "native";
        bool enabled = true;
        params.Get ("enabled", enabled);
        SetApiTracing (enabled);
        data.Add ("enabled", enabled);
        ok = true;
    }
    else if (backend == "EvP" && name == "GetErrorTrail") {
        // evp.errors.trail() — the failures reported since the add-on loaded, so a
        // command can put them in its OWN log next to the run they belong to. Bus-
        // local like SetTracing: a mutex-guarded ring, no ACAPI, so no gate hop.
        //
        // A command usually calls this in an `except` block, at the one moment the
        // gate may already be unusable — which is exactly why it must not need it.
        backendName = "native";
        GS::Int32 limit = 10;
        params.Get ("limit", limit);
        data.Add ("entries", evp::RecentFailures (limit > 0 ? (UInt32) limit : 0));
        data.Add ("total", (GS::Int32) evp::FailureCount ());
        data.Add ("logPath", evp::ApiErrorLogPath ());
        ok = true;
    }
    else if (backend == "EvP" && (name == "SetStatus" || name == "ShowAlert")) {
        // evp.ui.* — feedback that belongs on screen, not only in a log file.
        //
        // ⚠️ POST, NEVER INVOKE. A modal alert does not return until the USER acts,
        // so Invoke would hold the gate for human time and then report a bogus
        // timeout ("posted but never dispatched") while everything is in fact fine.
        // That is gate rule #2, learned the hard way. Post also means these never
        // block the script, which is what you want for progress messages.
        backendName = "native";

        GS::UniString message;
        params.Get ("message", message);

        const bool isAlert = (name == "ShowAlert");
        GS::UniString gateError;
        const bool posted = MainThreadGate::Get ().Post (
            [message, isAlert] () {
                if (isAlert) {
                    evp::ReportAlert (message);
                }
                else if (ControlPalette::HasInstance ()) {
                    ControlPalette::GetInstance ().SetCommandStatus (message);
                }
            },
            gateError);

        if (!posted) {
            error = MakeError ("GateUnavailable", gateError, command);
        }
        else {
            WebUIRunState::Get ().SetStatus (message);
            data.Add ("shown", message);
            ok = true;
        }
    }
    else if (backend == "EvP" && name == "ShowResults") {
        // Feature D — evp.ui.table(). One-shot results table in the palette.
        //
        // ⚠️ POST, NEVER INVOKE — same rule as SetStatus/ShowAlert. This touches DG
        // and must return immediately: a fire-and-forget display must not hold the
        // gate. Headers arrive as a plain string array; each row is a JSON string
        // {"cells":[...]} (the shape ObjectState reads back reliably), parsed inside
        // ShowResults. Arrays are captured by value into the posted lambda.
        backendName = "native";

        GS::Array<GS::UniString> headers, rows;
        params.Get ("headers", headers);
        params.Get ("rows", rows);

        GS::UniString gateError;
        const bool posted = MainThreadGate::Get ().Post (
            [headers, rows] () {
                if (ControlPalette::HasInstance ())
                    ControlPalette::GetInstance ().ShowResults (headers, rows);
            },
            gateError);

        if (!posted) {
            error = MakeError ("GateUnavailable", gateError, command);
        }
        else {
            WebUIRunState::Get ().SetResults (headers, rows);
            data.Add ("rows", (GS::Int32) rows.GetSize ());
            ok = true;
        }
    }
    else if (backend == "EvP" && name == "ShowResultText") {
        // A report-style result uses a native read-only MultiLineEdit. POST keeps
        // this display fire-and-forget just like ShowResults: DG stays on the main
        // thread and a worker never waits on repainting or human text selection.
        backendName = "native";
        GS::UniString resultText;
        params.Get ("text", resultText);

        GS::UniString gateError;
        const bool posted = MainThreadGate::Get ().Post (
            [resultText] () {
                if (ControlPalette::HasInstance ())
                    ControlPalette::GetInstance ().ShowResultText (resultText);
            },
            gateError);

        if (!posted) {
            error = MakeError ("GateUnavailable", gateError, command);
        }
        else {
            WebUIRunState::Get ().SetResultText (resultText);
            data.Add ("shown", resultText);
            ok = true;
        }
    }
    else if (backend == "EvP" && name == "GetCurrentParams") {
        // A command's Python frame receives its parameters only once, at Run.
        // Long-lived commands that deliberately react to a control change can ask
        // for a fresh snapshot, but DG is main-thread-only so this is a single
        // bounded gate hop, never an inline control read from the worker.
        backendName = "native";
        const auto paramsJson = std::make_shared<GS::UniString> ();
        GS::UniString gateError;
        const bool gated = MainThreadGate::Get ().Invoke (
            [paramsJson] () {
                if (ControlPalette::HasInstance ())
                    *paramsJson = ControlPalette::GetInstance ().CurrentParamsJson ();
            },
            MainThreadGate::DefaultTimeoutMs, gateError);
        if (!gated) {
            error = MakeError ("GateUnavailable", gateError, command);
        }
        else {
            data.Add ("paramsJson", *paramsJson);
            ok = true;
        }
    }
    else if (backend == "EvP" && name == "PollCancel") {
        // E9 — evp.runtime.should_cancel()/check_cancel() poll this from the
        // command's worker thread, potentially every loop iteration. Atomic reads
        // only: no ACAPI, no DG, so it must NOT cost a gate hop (~3ms of
        // main-thread time per tick would be the whole budget of a tight loop).
        // Same reasoning, same inline branch, as PollSelectionPrompt/SetTracing.
        //
        // Reached by both zones: embedded calls it straight through _evp, and the
        // external subprocess arrives here over the HTTP bus.
        backendName = "native";
        RunCancel& run = RunCancel::Get ();
        const bool isCancelled = run.IsCancelled (); // also enforces timeout_s
        data.Add ("cancelled", isCancelled);
        data.Add ("running", run.IsRunning ());
        data.Add ("reason", GS::UniString (isCancelled ? RunCancel::ReasonText (run.Reason ()) : ""));
        ok = true;
    }
    else if (backend == "EvP" && name == "PollSelectionPrompt") {
        // evp.ui.request_selection() polls this from its worker thread. It reads
        // only atomic flags — no ACAPI, no DG — so it must NOT cost a gate hop, the
        // same reasoning as SetTracing. A tight poll through the gate would be 3ms
        // of main-thread time per tick for nothing.
        backendName = "native";
        bool continued = false, cancelled = false, active = false;
        if (ControlPalette::HasInstance ()) {
            const ControlPalette& palette = ControlPalette::GetInstance ();
            continued = palette.IsSelectionPromptContinued ();
            cancelled = palette.IsSelectionPromptCancelled ();
            active = palette.IsSelectionPromptActive ();
        }
        else {
            // No palette means no one can ever press Continue — report cancelled so
            // the caller fails fast instead of spinning forever.
            cancelled = true;
        }
        data.Add ("continued", continued);
        data.Add ("cancelled", cancelled);
        data.Add ("active", active);
        ok = true;
    }
    else if (backend == "EvP" && (name == "ShowSelectionPrompt" || name == "HideSelectionPrompt")) {
        // Enter/leave the prompt state. This touches DG, so it goes to the main
        // thread — and POST, never INVOKE: Begin/End are quick, but the rule stands
        // that the gate is never held from here (see SetStatus/ShowAlert above).
        backendName = "native";

        const bool show = (name == "ShowSelectionPrompt");
        GS::UniString message;
        params.Get ("message", message);

        GS::UniString gateError;
        const bool posted = MainThreadGate::Get ().Post (
            [show, message] () {
                if (!ControlPalette::HasInstance ())
                    return;
                if (show)
                    ControlPalette::GetInstance ().BeginSelectionPrompt (message);
                else
                    ControlPalette::GetInstance ().EndSelectionPrompt ();
            },
            gateError);

        if (!posted) {
            error = MakeError ("GateUnavailable", gateError, command);
        }
        else {
            data.Add ("shown", show);
            ok = true;
        }
    }
    else if (backend == "EvP" && name == "CommitTransaction") {
        backendName = "native";

        GS::UniString txName ("EvP command");
        GS::Array<GS::UniString> steps;
        params.Get ("name", txName);
        params.Get ("steps", steps);

        ReplayOutcome outcome;
        const auto mainStarted = std::chrono::steady_clock::now ();

        GS::UniString gateError;
        const bool gated = MainThreadGate::Get ().Invoke ([&] () { ReplayBatch (txName, steps, outcome); },
                                                          MainThreadGate::DefaultTimeoutMs, gateError);

        mainThreadMs = MillisSince (mainStarted);

        // E25 — a committed batch changed the model, and Archicad will not tell
        // us about our own writes. Bumped once for the whole transaction, not
        // per step: the batch is one undo step to the user, so it is one change.
        // A rolled-back batch changed nothing and must NOT bump.
        if (gated && outcome.allOk)
            geomsrv::ChangeTracker::Get ().RecordSelfWrite ();

        if (!gated) {
            error = MakeError ("GateTimeout", gateError, command);
        }
        else if (!outcome.allOk) {
            error = MakeError ("TransactionFailed",
                               GS::UniString::Printf ("step %u (%T) failed: %T — the whole batch was rolled back, "
                                                      "nothing was committed.",
                                                      (unsigned) outcome.failedIndex, outcome.failedCommand.ToPrintf (),
                                                      outcome.failedError.ToPrintf ()),
                               txName);
        }
        else {
            data.Add ("results", outcome.results);
            data.Add ("steps", (GS::Int32) steps.GetSize ());
            ok = true;
        }
    }
    else if (backend == "EvP" && IsGateFreeCommand (name)) {
        // E2 geometry queries: no ACAPI, so no gate. They read the immutable
        // snapshot through a mutex-guarded store, exactly like the buffer API.
        // Running them inline is what makes a thousands-of-rays heatmap viable —
        // at ~3ms per marshalled hop it would otherwise take minutes.
        backendName = "native";

        const GS::String commandName (name.ToCStr ().Get ());
        const geomsrv::NativeCommandResult result = geomsrv::ExecuteNativeCommand (commandName, params);

        if (!result.ok) {
            error = MakeError (NativeFailureCode (result), result.error, command);
        }
        else {
            data = result.data;
            ok = true;
        }
    }
    else if (backend == "EvP") {
        backendName = "native";

        // Native commands are main-thread-only, so hop the gate. Everything the
        // lambda touches is captured by reference and Invoke blocks until it has
        // run — see the gate's contract; on timeout it revokes, and the error
        // path below never reads these.
        geomsrv::NativeCommandResult execution;
        const auto mainStarted = std::chrono::steady_clock::now ();

        GS::UniString gateError;
        const bool gated = MainThreadGate::Get ().Invoke (
            [&] () {
                const GS::String commandName (name.ToCStr ().Get ());

                // The outer CallScope belongs to the CALLER's thread; this lambda
                // runs on the main thread, where that thread-local is not visible.
                // Re-open it here or every failure a native command reports — the
                // overwhelming majority of them — lands in the log with no call_id,
                // no command name and no params, which is most of the value.
                const evp::CallScope mainScope (callId, command, paramsJson);

                // Immediate mode: a write still needs an undo scope, and the
                // command no longer opens one itself (it cannot — it must stay
                // nestable for transactions). So the dispatcher supplies exactly
                // one here, giving this call its own undo step. Reads get none:
                // an undoable command that changes nothing still costs the user
                // an undo step.
                bool isWrite = false;
                if (geomsrv::IsWriteCommand (commandName, isWrite) && isWrite) {
                    const GSErrCode undoErr = ACAPI_CallUndoableCommand ("EvP: " + name, [&] () -> GSErrCode {
                        execution = geomsrv::ExecuteNativeCommand (commandName, params);
                        return execution.ok ? NoError : APIERR_GENERAL;
                    });

                    // The undo scope failed for a reason the command never
                    // reported — so the code Archicad returned is the ONLY
                    // evidence, and "err=-2130313111" is not evidence. Decode it.
                    // (APIERR_COMMANDFAILED here means the inner call threw;
                    // APIERR_REFUSEDCMD means a scope was already open, i.e. a
                    // command opened its own — the P1 bug, see WriteCommand.)
                    if (undoErr != NoError && execution.error.IsEmpty ()) {
                        execution.error = EVP_ACAPI_FAIL ("ACAPI_CallUndoableCommand", undoErr,
                                                          GS::UniString ("immediate-mode undo scope for EvP.") + name);
                    }

                    // E25 — THE MODEL JUST CHANGED AND NOTHING ELSE WILL SAY SO.
                    // Archicad does not notify an add-on of its own changes
                    // (proved live: a bus write to a confirmed-observed element
                    // produced no notification, while a human edit to the SAME
                    // element did). So a viewer watching the token would miss
                    // every change made by another EvP command unless the
                    // dispatcher bumps it here.
                    if (undoErr == NoError && execution.ok)
                        geomsrv::ChangeTracker::Get ().RecordSelfWrite ();
                }
                else {
                    execution = geomsrv::ExecuteNativeCommand (commandName, params);
                }
            },
            MainThreadGate::DefaultTimeoutMs, gateError);

        mainThreadMs = MillisSince (mainStarted);

        if (!gated) {
            error = MakeError ("GateTimeout", gateError, command);
        }
        else if (!execution.ok) {
            error = MakeError (NativeFailureCode (execution), execution.error, command);
        }
        else {
            data = execution.data;
            ok = true;
        }
    }
    else if (backend == "API") {
        // Archicad's official JSON interface, executed IN-PROCESS — no HTTP layer,
        // no port to discover, no loopback hop. `command` already carries the
        // JSON interface's own name ("API.GetProductInfo"), so it passes through
        // verbatim: the namespace states the backend AND is the wire name.
        backendName = "json-api";

        GS::ObjectState request;
        request.Add ("command", command);
        request.Add ("parameters", params);

        GS::ObjectState jsonResult;
        GSErrCode jsonErr = NoError;
        const auto mainStarted = std::chrono::steady_clock::now ();

        GS::UniString gateError;
        const bool gated =
            MainThreadGate::Get ().Invoke ([&] () { jsonErr = ACAPI_Command_ExecuteJSONRequest (request, jsonResult); },
                                           MainThreadGate::DefaultTimeoutMs, gateError);

        mainThreadMs = MillisSince (mainStarted);

        bool succeeded = false;
        jsonResult.Get ("succeeded", succeeded);

        if (!gated) {
            error = MakeError ("GateTimeout", gateError, command);
        }
        else if (jsonErr == APIERR_BADNAME) {
            error = MakeError ("UnknownCommand", "No JSON interface command named " + command + ".", command);
        }
        else if (jsonErr != NoError && !succeeded) {
            // The detail lives in the result's own error field, not the errcode.
            GS::ObjectState jsonError;
            GS::UniString message ("the JSON interface command failed");
            GS::Int32 code = 0;
            if (jsonResult.Get ("error", jsonError)) {
                jsonError.Get ("message", message);
                jsonError.Get ("code", code);
            }
            error = MakeError ("CommandFailed",
                               GS::UniString::Printf ("%T (json code %d; %T)", message.ToPrintf (), (int) code,
                                                      evp::DescribeErr (jsonErr).ToPrintf ()),
                               command);
        }
        else if (!succeeded) {
            GS::ObjectState jsonError;
            GS::UniString message ("the JSON interface command reported failure");
            if (jsonResult.Get ("error", jsonError))
                jsonError.Get ("message", message);
            error = MakeError ("CommandFailed", message, command);
        }
        else {
            // Unwrap {"succeeded":true,"result":{...}} so scripts see the payload,
            // not the JSON interface's envelope inside ours.
            GS::ObjectState payload;
            if (jsonResult.Get ("result", payload))
                data = payload;
            ok = true;
        }
    }
    else if (backend == "Tapir") {
        // E1 — route to an INSTALLED Tapir through the official JSON interface.
        //
        // This is a PROXY, deliberately, and it does not retire the absorb-Tapir
        // decision: it unblocks the ports that need Tapir commands today, and each
        // command gets reimplemented natively later where it earns it. The
        // namespace still states the backend honestly, so a trace shows exactly
        // which calls still depend on an add-on we do not ship.
        //
        // Schema is from the local `archicad` pypi package (the only trustworthy
        // source for API.* shapes — see the reference folder), NOT from Tapir docs:
        //   request  {"command":"API.ExecuteAddOnCommand","parameters":{
        //              "addOnCommandId":{"commandNamespace":"TapirCommand",
        //                                "commandName":<name>},
        //              "addOnCommandParameters":{...}}}
        //   response result.addOnCommandResponse
        backendName = "tapir";

        GS::ObjectState commandId;
        commandId.Add ("commandNamespace", GS::UniString ("TapirCommand"));
        commandId.Add ("commandName", name);

        GS::ObjectState inner;
        inner.Add ("addOnCommandId", commandId);
        inner.Add ("addOnCommandParameters", params);

        GS::ObjectState request;
        request.Add ("command", GS::UniString ("API.ExecuteAddOnCommand"));
        request.Add ("parameters", inner);

        GS::ObjectState jsonResult;
        GS::UniString gateError;
        const auto mainStarted = std::chrono::steady_clock::now ();
        GSErrCode execErr = NoError;

        const bool gated =
            MainThreadGate::Get ().Invoke ([&] () { execErr = ACAPI_Command_ExecuteJSONRequest (request, jsonResult); },
                                           MainThreadGate::DefaultTimeoutMs, gateError);
        mainThreadMs =
            std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - mainStarted).count ();

        if (!gated) {
            error = MakeError ("GateTimeout", gateError, command);
        }
        else if (execErr != NoError) {
            // APIERR_BADNAME here means the command id did not resolve at all —
            // overwhelmingly "Tapir is not installed", which is worth saying
            // outright rather than leaving as an error number.
            const GS::UniString detail = (execErr == APIERR_BADNAME)
                                             ? GS::UniString ("API.ExecuteAddOnCommand rejected the command id. Is the "
                                                              "Tapir add-on installed and loaded in this Archicad?")
                                             : GS::UniString::Printf ("ACAPI_Command_ExecuteJSONRequest failed: %T",
                                                                      evp::DescribeErr (execErr).ToPrintf ());
            error = MakeError ("TapirUnavailable", detail, command);
        }
        else {
            bool succeeded = false;
            jsonResult.Get ("succeeded", succeeded);
            if (!succeeded) {
                // The real message lives in result.error.message; the errcode alone
                // is useless to a script author.
                GS::ObjectState innerError;
                GS::UniString message ("the Tapir command reported a failure");
                if (jsonResult.Get ("error", innerError))
                    innerError.Get ("message", message);
                error = MakeError ("TapirCommandFailed", message, command);
            }
            else {
                // Unwrap TWICE: the JSON interface's own envelope, then
                // addOnCommandResponse — so a script sees the add-on's payload and
                // nothing else.
                GS::ObjectState payload, addOnResponse;
                if (jsonResult.Get ("result", payload) && payload.Get ("addOnCommandResponse", addOnResponse)) {
                    data = addOnResponse;
                }
                else {
                    data = payload; // command returned nothing structured
                }
                ok = true;
            }
        }
    }
    else {
        error = MakeError ("UnknownBackend",
                           "Unknown backend '" + backend + "'. Expected Tapioca.*, EvP.*, API.* or Tapir.*.", command);
    }

    if (ok && backend == "EvP" && FindDispatcherVerb (name) != nullptr) {
        GS::UniString schemaError;
        if (!ValidateDispatcherSchema (name, data, false, schemaError)) {
            ok = false;
            data.Clear ();
            error = MakeError ("SchemaValidationFailed", "Invalid dispatcher response: " + schemaError, command);
        }
    }

    GS::ObjectState meta;
    meta.Add ("backend", backendName);
    meta.Add ("zone", zone);
    meta.Add ("duration_ms", MillisSince (started));
    meta.Add ("main_thread_ms", mainThreadMs);
    meta.Add ("api_version", GS::UniString (ApiVersion));
    meta.Add ("call_id", callId);

    // Exactly one of data/error is present; the other key is simply absent (see
    // MakeError — ObjectState has no null, and absent reads as None in Python).
    GS::ObjectState envelope;
    envelope.Add ("ok", ok);
    if (ok) {
        envelope.Add ("data", data);
    }
    else {
        envelope.Add ("error", error);

        // Every failure the script sees is written down, unconditionally. This is
        // the layer that catches what no command can report for itself: a gate
        // timeout, an unknown backend, params that never parsed. Recorded even
        // when a command already logged the underlying cause, because the two
        // answer different questions -- "what broke" and "what did the script
        // get back" -- and reading them adjacent is how you tell a swallowed
        // error from a reported one.
        //
        // "Cancelled" is the one exclusion, and it is not a judgement call: once
        // a run is cancelled EVERY later call is refused with it (that is how a
        // command unwinds without opting in), so logging them would bury the real
        // failure under dozens of entries saying the user pressed Stop.
        GS::UniString code, message, detail;
        error.Get ("code", code);
        error.Get ("message", message);
        error.Get ("detail", detail);
        if (code != "Cancelled")
            evp::LogEnvelopeFailure (command, callId, code, message, detail, paramsJson);
    }
    envelope.Add ("meta", meta);

    const GS::UniString json = ToJson (envelope);
    Trace (GS::UniString::Printf ("<-- [%T] %T", callId.ToPrintf (), json.ToPrintf ()));
    return json;
}

} // namespace evp
