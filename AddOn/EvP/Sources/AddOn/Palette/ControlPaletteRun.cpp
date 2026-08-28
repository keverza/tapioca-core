// The palette shell's RUN STATE — a second implementation file for the same class,
// on the precedent of ControlPaletteLayout.cpp.
//
// WHY IT IS STILL THE SHELL, and not a sub-object: CLAUDE.md assigns the run
// orchestration to ControlPalette itself, beside the DG events and the band layout.
// The one Run/Cancel/Stopping button IS the shell's button, the status line it
// writes is the shell's, and a run's completion has to reach the palette that
// started it. Splitting that into an object would put a seam through the middle of
// one decision — which of Run's three roles is live — and the palette seam check
// (tools/quality/check_structure.py) exists to catch exactly that.
//
// So what moved here is the FILE, not the concern: everything about a run in flight
// lives in one place, and ControlPalette.cpp keeps the DG event routing, the
// splitters and the placement it is supposed to own. The size cap in
// tools/quality/check_cpp.py named this extraction as the one owed next; the room it
// frees is what the preview band and the action bar are spending.
//
// Everything here runs on the MAIN thread. A worker never calls these directly — it
// Posts through MainThreadGate, which is why FinishRun can touch DG freely.

#include "ControlPalette.hpp"

#include "Python/CommandLaunch.hpp" // composes a run from what the palette selected
#include "Python/PathUtils.hpp"     // StartupTrace — the run path's crash breadcrumbs
#include "Python/MainThreadGate.hpp"
#include "Python/PythonHost.hpp"
#include "Python/RunCancel.hpp" // E9 — the running command's cancel token
#include "Python/ForcedParamMerge.hpp"
#include "Preview/PreviewRuntimeState.hpp"

// E3 — enter/leave the selection-prompt state. Both run on the main thread (the
// dispatcher Posts them); the worker thread only reads the atomic flags.
void ControlPalette::BeginSelectionPrompt (const GS::UniString& message)
{
    promptContinued.store (false);
    promptCancelled.store (false);
    promptActive.store (true);
    // Reuse the command-status line for the prompt text; RefreshRunGate is
    // suppressed while active so the idle poll cannot stamp over it.
    lastGateMessage = message;
    commandStatus.SetText (message);
    RefreshRunGate (); // the button keeps its Cancel role — a prompt is
                       // exactly when the user may want out (F1)
    Layout ();         // reveals + positions the Continue button
    // Layout reflows every control below the status line (the button adds a row).
    // These calls come from a Posted lambda, OUTSIDE the DG event flow that would
    // normally repaint — so force a full redraw or the vacated rects keep their old
    // pixels and text ghosts on top of text.
    Redraw ();
}

// E9 — the run is over. Main thread only (the worker Posts it), so it can touch DG
// freely. Everything here is generation-guarded: a completion from a run the user
// already abandoned must not hide the Stop button of the one now in flight.
void ControlPalette::FinishRun (uint64_t generation, const GS::UniString& status)
{
    evp::RunCancel::Get ().EndRun (generation);

    if (generation != runGeneration)
        return; // a stale completion — the live run owns the UI

    const bool wasAutomaticPreview = generation == automaticPreviewGeneration;
    if (wasAutomaticPreview) {
        automaticPreview.Finished (generation);
        automaticPreviewGeneration = 0;
        const evp::CommandInfo* const selected = SelectedCommand ();
        if (selected == nullptr || selected->folder != automaticPreviewFolder)
            preview.SetKind (selected != nullptr ? selected->previewKind : GS::UniString ("text"));
        automaticPreviewFolder.Clear ();
    }

    runActive.store (false);
    stopRequested.store (false); // F1 — clear the third state, whatever ended the
                                 // run: press, timeout, panel close or completion
    SetCommandStatus (status);
    lastGateMessage = status; // or the idle poll would stamp over the result
    // The run stored a result, so there is now something for an action to act on.
    // Enabled whatever the outcome: a run that failed part way through still wrote
    // whatever it wrote, and refusing to let the user export that is refusing them
    // the evidence. The Python side says so by name when there is nothing stored.
    if (!wasAutomaticPreview)
        actionBar.SetEnabled (true);
    Layout ();         // reflows back up if the prompt row was showing
    RefreshRunGate (); // Cancel/Stopping... becomes Run again
    Redraw ();         // repaint the reflow — see BeginSelectionPrompt
}

void ControlPalette::EndSelectionPrompt ()
{
    promptActive.store (false);
    lastGateMessage.Clear (); // let RefreshRunGate own the status line again
    Layout ();                // hides the Continue button, reflows back up
    RefreshRunGate ();        // restore Run's enabled state
    Redraw ();                // repaint the reflow — see BeginSelectionPrompt
}

// Everything standing between the user and a working run, most blocking first.
// Empty == ready. This drives the Run button AND the status line together, so the
// button can never sit there enabled waiting to fail.
GS::UniString ControlPalette::WhatIsMissing () const
{
    // FIRST INSTRUCTION, before anything command-specific: the two things that have
    // to be true before any command can run, named in the order they are done. The
    // server is how external commands reach Archicad at all.
    // A Dynamo command reaches Archicad through the owned runner, so the bus it
    // does not use cannot be what is missing.
    const evp::CommandInfo* const info = SelectedCommand ();
    if (!IsDynamoCommand (info) && !serverBand.IsRunning ())
        return "Start server, pick command.";

    if (info == nullptr)
        return "Pick a command.";

    const GS::UniString runnerWait = DynamoGateMessage (info);
    if (!runnerWait.IsEmpty ())
        return runnerWait;

    // A required parameter with nothing usable in it.
    const GS::UniString unsetInput = params.WhatIsMissing ();
    if (!unsetInput.IsEmpty ())
        return unsetInput;

    // NOTE: selection is deliberately NOT a gate. It changes outside this panel
    // with nothing to observe, so the button could only be driven by polling — and
    // a Run button that lags behind the model is worse than one that is always
    // pressable. Commands that need a selection say so in their own message and
    // check it themselves, where the answer is current.
    if (info->needsSelection)
        return GS::UniString ();

    return GS::UniString ();
}

void ControlPalette::RefreshRunGate ()
{
    // F1 — the one button's role. While a run is in flight it IS the cancel button,
    // so it must stay ENABLED: the old unconditional Disable() here would leave a
    // running command with nothing on screen to stop it. The double-run guard is now
    // the role itself — ButtonClicked routes on runActive and never reaches
    // RunSelected while one is in flight. The status line belongs to the run in both
    // states, hence the early return.
    if (runActive.load ()) {
        if (stopRequested.load ()) {
            // ASCII ellipsis on purpose: the label ultimately comes from a codepage
            // 1252 .grc string, and a real "…" cannot survive that round trip.
            runButton.SetText ("Stopping...");
            runButton.Disable (); // asked once; a second press means nothing
        }
        else {
            runButton.SetText ("Cancel");
            runButton.Enable ();
        }
        return;
    }

    runButton.SetText ("Run");

    // While a command is waiting on a selection, the prompt owns the status line
    // and Run stays disabled — do not let the idle poll fight either.
    if (promptActive.load ()) {
        runButton.Disable ();
        return;
    }

    const GS::UniString missing = WhatIsMissing ();

    if (missing.IsEmpty ())
        runButton.Enable ();
    else
        runButton.Disable ();

    // When nothing BLOCKS the run, a command that works on a selection still says
    // so — as a hint, not a gate, because the panel cannot see the selection change.
    GS::UniString message = missing;
    if (message.IsEmpty ()) {
        const evp::CommandInfo* const info = SelectedCommand ();
        if (info != nullptr && info->needsSelection)
            message = "Select the elements to work on, then press Run.";
    }

    // Only rewrite when it actually changed — this runs on idle, and a status line
    // that reassigns itself constantly flickers and stamps on run results.
    if (message != lastGateMessage) {
        lastGateMessage = message;
        if (!message.IsEmpty ())
            commandStatus.SetText (message);
    }
}
void ControlPalette::RunSelected (const GS::UniString& action, const GS::UniString& menuRegion,
                                  bool automaticPreviewRun)
{
    // BREADCRUMBS, flushed per line into logs\startup.log. A hard crash leaves
    // nothing behind but what was already on disk, and this path now has three
    // callers (the Run button, an action-bar button, a right-click entry) that all
    // end in the same place — so "where did it get to" has to be answerable from
    // the log rather than from a repro nobody can reproduce.
    evp::StartupTrace (GS::UniString ("RunSelected: enter action='") + action + "' region='" + menuRegion + "'");
    // E9 re-entrancy guard. Nothing stopped a second Run press from spawning a
    // second detached worker, and two runs sharing one cancel token would mean Stop
    // could only ever reach the newer one. One run at a time, said out loud.
    if (!automaticPreviewRun)
        CancelAutomaticPreview (false);
    if (runActive.load ()) {
        SetCommandStatus ("A command is already running — press Cancel to end it first.");
        return;
    }

    const evp::CommandInfo* const info = SelectedCommand ();
    if (info == nullptr) {
        SetCommandStatus ("Select a command first.");
        return;
    }

    // Running is a decision — the browse it came from is over. Collapsing here also
    // keeps the run's status line and results from opening below a list that is
    // still standing between them and the parameters they belong to.
    commandsPanel.CloseList ();

    const bool dynamo = IsDynamoCommand (info);
    GS::UniString error;
    if (!dynamo) {
        if (!evp::PythonHost::Get ().EnsureInitialized (error)) {
            SetCommandStatus ("Python init failed - see logs\\scan.log");
            return;
        }
    }

    // What the status line calls this. An action says what it is doing, because
    // "Running Element Info Panel..." while exporting a CSV describes the wrong
    // thing and looks like the command being run a second time - which is
    // exactly the misunderstanding the stored-result rule exists to prevent.
    const GS::UniString title = automaticPreviewRun ? info->title + " preview"
                                : action.IsEmpty () ? info->title
                                                    : info->title + " - " + action;

    // Zone C. The subprocess's ONLY way back to Archicad is the HTTP bus, so the
    // server is not optional here — start it rather than fail, and say so.
    const bool external = (info->runtime == "external");
    unsigned short port = 0;
    if (external) {
        if (!serverBand.IsRunning ())
            serverBand.Start ();
        if (!serverBand.IsRunning ()) {
            SetCommandStatus (title + ": FAILED - runtime=\"external\" needs the " EVP_PRODUCT_NAME " server, "
                                      "which would not start.");
            return;
        }
        port = serverBand.Port ();
    }

    // E9 — arm the cancel token BEFORE the worker exists, so there is no window in
    // which a running command cannot be stopped. The generation it returns is
    // captured BY VALUE into the lambda below (the gate-lambda rule: this frame is
    // gone long before the worker finishes) and is what makes a late cancel or a
    // late completion unable to touch a different run.
    runGeneration = evp::RunCancel::Get ().BeginRun (info->timeoutSeconds);
    const uint64_t generation = runGeneration;
    runActive.store (true);
    stopRequested.store (false);
    RefreshRunGate (); // F1 — Run becomes Cancel; restored by FinishRun

    if (automaticPreviewRun) {
        automaticPreview.Started (generation);
        automaticPreviewGeneration = generation;
        automaticPreviewFolder = info->folder;
        SetCommandStatus ("Generating preview for " + info->title + "...  — press Cancel to stop it.");
    }
    else
        SetCommandStatus ("Running " + title + (external ? " (external)..." : "...") + "  — press Cancel to stop it.");

    // Zone B: the button handler must return so the event loop stays free — it is
    // what dispatches this worker's gate traffic. Zone C needs the same thread for a
    // different reason: RunCommandExternal blocks until the subprocess exits, and
    // that subprocess is calling back into the main thread the whole time. Composing
    // the run and starting that worker is evp::LaunchCommand's job — the palette
    // supplies only what it alone knows.
    evp::StartupTrace ("RunSelected: params collected, composing the run");

    const bool watchArmed = automaticPreviewRun || (evp::preview::PreviewRuntimeState::Get ().IsEnabled () &&
                                                    (info->previewKind == "plan2d" || info->previewKind == "3d"));
    GS::UniString paramsJson = params.CollectJson ();
    if (automaticPreviewRun) {
        const std::string merged =
            evp::MergeForcedParams (paramsJson.ToCStr (0, MaxUSize, CC_UTF8).Get (),
                                    info->previewOverridesJson.ToCStr (0, MaxUSize, CC_UTF8).Get ());
        paramsJson = GS::UniString (merged.c_str (), CC_UTF8);
    }
    // The runner instead of a Python worker — same generation, same gate, same
    // FinishRun on the way back. See ControlPaletteDynamo.cpp.
    if (dynamo) {
        GS::UniString launchError;
        if (!LaunchDynamoRun (paramsJson, generation, title, launchError))
            FinishRun (generation, title + ": FAILED - " + launchError + ".");
        return;
    }

    const evp::CommandLaunchRequest request {
        info->path,          info->folder,       title,      paramsJson, action, menuRegion, info->requiresApi,
        info->requiresTapir, info->requirements, watchArmed, external,   port,   generation
    };

    // Where the outcome lands is the PALETTE's business, so the callback stays here.
    // If the gate could not take the Post, the event loop is dead or gone — but the
    // TOKEN must still be released, or the next run would inherit a token that says a
    // run is in flight. FinishRun does the same EndRun on the happy path, and EndRun
    // ignores a generation that is no longer live.
    evp::LaunchCommand (request, [] (uint64_t finishedGeneration, const GS::UniString& status) {
        GS::UniString gateError;
        if (!evp::MainThreadGate::Get ().Post (
                [finishedGeneration, status] () {
                    if (ControlPalette::HasInstance ())
                        ControlPalette::GetInstance ().FinishRun (finishedGeneration, status);
                },
                gateError))
            evp::RunCancel::Get ().EndRun (finishedGeneration);
    });
}
