// The palette shell's DYNAMO RUNNER — a fourth implementation file for the same
// class, on the precedent of ControlPaletteParams.cpp, ControlPaletteLayout.cpp
// and ControlPaletteRun.cpp.
//
// WHY IT IS THE SHELL, and not a sub-object: everything here writes the shell's
// own items. The state line is the shell's statusText, the graph choice REPLACES
// the shell's selected command, and a finished run has to reach the palette that
// started it through FinishRun. A sub-object answers the shell and never drives
// it (tools/quality/check_cpp.py enforces exactly that), so none of this could
// live in one without inverting the relationship.
//
// WHY IT IS ITS OWN FILE: the runner is a whole second way to run a command, and
// the three shell files it would otherwise land in each read as one concern —
// the DG event routing, the top-to-bottom band pass, and a run in flight. Their
// size caps say so in tools/quality/check_cpp.py, and the answer that rule asks
// for is a new home, not a bigger file.
//
// WHAT IS DIFFERENT ABOUT A DYNAMO COMMAND: it does not go through Python and it
// does not need the HTTP bus. The runner is a separate .NET 10 process holding
// one reusable, UI-less DynamoModel (Dynamo/DynamoHost.hpp) — so the gate asks
// whether THAT is ready rather than whether the server is running, and the run
// path is a worker thread talking to it instead of evp::LaunchCommand.
//
// Everything here runs on the MAIN thread except the worker body in
// LaunchDynamoRun, which touches no DG at all and Posts its completion through
// MainThreadGate — the same contract as every other run.

#include "ControlPalette.hpp"

#include "Dynamo/DynamoGraphInputs.hpp"
#include "Dynamo/DynamoHost.hpp"
#include "Python/MainThreadGate.hpp"
#include "Python/RunCancel.hpp"
#include "ObjectStateJSONConversion.hpp"

#include <thread>
#include <utility>

// The one place that decides whether a command belongs to the runner. The
// catalog carries `runtime` as free-form text, so it is compared in exactly one
// spot, and a null selection answers "no" rather than making every caller ask.
bool ControlPalette::IsDynamoCommand (const evp::CommandInfo* command) const
{
    return command != nullptr && command->runtime == "dynamo";
}

void ControlPalette::StartDynamoRunner ()
{
    // Said before the process has answered: starting it is asynchronous, and a
    // line that appears only once the runner is up would read as a panel that
    // forgot to draw something.
    statusText.SetText ("Dynamo runner is stopped.");
    statusText.Show ();
    evp::dynamo::StartHeadless ();
}

// Above the action row, beside the server's address: the two background things
// the panel can run, each on its own line, neither of which is the other's.
short ControlPalette::PlaceDynamoStatus (short top, short left, short right)
{
    statusText.SetRect (DG::Rect (left, top, right, (short) (top + 16)));
    return 22;
}

void ControlPalette::RefreshDynamoStatus ()
{
    // Compared before it is written: SetText on an unchanged string still
    // invalidates the item, and this runs on every idle tick.
    const GS::UniString status = evp::dynamo::HeadlessStatusText ();
    if (status == lastDynamoStatus)
        return;
    lastDynamoStatus = status;
    statusText.SetText (status);
}

GS::UniString ControlPalette::DynamoGateMessage (const evp::CommandInfo* command) const
{
    if (!IsDynamoCommand (command) || evp::dynamo::IsHeadlessReady ())
        return GS::UniString ();
    // Whatever the runner itself says — "starting", "failed", a version — is
    // already the most specific thing there is to tell the user.
    return evp::dynamo::HeadlessStatusText ();
}

void ControlPalette::DynamoGraphChosen (const GS::UniString& graphPath)
{
    // Any FilePath row's Browse comes through here — most of them are ordinary
    // parameters, and a cancelled dialog reports nothing at all. Both leave the
    // block alone and only re-gate Run, which the new path may have satisfied.
    if (!graphPath.IsEmpty () && IsDynamoCommand (SelectedCommand ())) {
        // The .dyn's top-level Dynamo Player metadata IS the form: only nodes the
        // graph author exposed as inputs become rows, and an unsupported type is a
        // refusal rather than a control that would send a value Dynamo cannot bind.
        evp::CommandInfo populated;
        GS::UniString error;
        if (!evp::dynamo::PopulateLoaderInputs (graphPath, populated, error)) {
            SetCommandStatus (error);
            return;
        }
        if (commandsPanel.ReplaceSelected (std::move (populated)))
            RebuildCommandBlock ();
    }
    RefreshRunGate ();
}

bool ControlPalette::LaunchDynamoRun (const GS::UniString& paramsJson, uint64_t generation, const GS::UniString& title,
                                      GS::UniString& error)
{
    // The graph is one of the collected parameters — the loader's first row — so
    // it is read back out of the same JSON the runner is about to receive rather
    // than tracked separately and allowed to disagree with it.
    GS::ObjectState values;
    GS::UniString graphPath;
    if (JSON::ConvertToObjectState (paramsJson, values) != NoError || !values.Get ("graph", graphPath) ||
        graphPath.IsEmpty ()) {
        error = "select a Dynamo graph";
        return false;
    }

    try {
        std::thread ([graphPath, paramsJson, generation, title] () {
            GS::UniString result;
            GS::UniString runError;
            const bool succeeded = evp::dynamo::RunHeadlessGraph (graphPath, paramsJson, generation, result, runError);

            // A cancel that landed while the graph was evaluating wins over both
            // outcomes: the user already stopped caring which one it was.
            GS::UniString status;
            if (evp::RunCancel::Get ().IsCancelled (generation))
                status = title + ": CANCELLED - " +
                         GS::UniString (evp::RunCancel::ReasonText (evp::RunCancel::Get ().Reason ())) + ".";
            else if (succeeded)
                status = title + ": " + result;
            else
                status = title + ": FAILED - " + runError;

            // The worker owns no DG. If the gate cannot take the completion the
            // run token still has to be released, or the palette would sit on a
            // Cancel button for a run that is already over.
            GS::UniString gateError;
            if (!evp::MainThreadGate::Get ().Post (
                    [generation, status] () {
                        if (ControlPalette::HasInstance ())
                            ControlPalette::GetInstance ().FinishRun (generation, status);
                    },
                    gateError))
                evp::RunCancel::Get ().EndRun (generation);
        }).detach ();
    }
    catch (...) {
        error = "could not create the Dynamo run thread";
        return false;
    }
    return true;
}
