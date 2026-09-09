// The palette shell's GRASSHOPPER BAND — a fifth implementation file for the
// same class, on the precedent of ControlPaletteParams.cpp,
// ControlPaletteLayout.cpp, ControlPaletteRun.cpp and ControlPaletteDynamo.cpp.
//
// WHY IT IS THE SHELL, and not a sub-object: everything here writes the shell's
// own items — the band's own status line and its three buttons — and it drives
// the relayout that a new schema forces. A sub-object answers the shell and
// never drives it (tools/quality/check_cpp.py enforces exactly that), so none of
// this could live in one without inverting the relationship. The parts that CAN
// answer rather than drive already do: Palette/WorkflowRows decides what is
// drawn and Palette/WorkflowPanel draws it.
//
// WHY IT IS ITS OWN FILE: a workflow session is a whole third way to produce
// something from this palette, beside a Python command and a Dynamo graph, and
// the three shell files it would otherwise land in each read as one concern.
// Their size caps say so in tools/quality/check_cpp.py, and the answer that rule
// asks for is a new home, not a bigger file.
//
// ⚠️ EVERYTHING HERE RUNS ON ARCHICAD'S MAIN THREAD, AND THE SCHEMA IS POLLED
// RATHER THAN PUSHED FOR EXACTLY THAT REASON. A schema arrives on the bridge's
// IO thread; rebuilding DG items from there would be a cross-thread DG call, the
// one thing DG does not survive. So the controller stores what arrived and this
// band notices it on the next idle. The latency is one idle tick, and the
// alternative is a crash that reproduces once a week.
//
// ⚠️ NOTHING HERE BLOCKS ON THE WORKER. Every button sends and returns; the
// answer arrives later and is picked up by the same poll. That is the rule the
// whole process boundary exists to keep (GhWorkerHost.hpp, rule 4).

#include "ControlPalette.hpp"

#include "Grasshopper/GhWorkerHost.hpp"
#include "Grasshopper/GhWorkflowController.hpp"
#include "PaletteMetrics.hpp"
#include "PaletteScroll.hpp"

#include "DGFileDialog.hpp"
#include "FileTypeManager.hpp"

#include <chrono>

using namespace evp::palette;

namespace {

// The band's own buttons, left to right.
constexpr short WorkflowButtonWidth = 96;
constexpr short WorkflowButtonGap = 6;

GS::UniString FromStd (const std::string& text)
{
    return GS::UniString (text.c_str (), CC_UTF8);
}

std::string ToStd (const GS::UniString& text)
{
    return std::string (text.ToCStr (0, MaxUSize, CC_UTF8).Get ());
}

// A monotonic millisecond clock for the controller's settle window. The
// controller never reads a clock itself -- that is what lets its tests advance
// time by hand -- so someone has to, and the idle tick is the only place in this
// palette that runs on a cadence.
uint64_t NowMs ()
{
    using namespace std::chrono;
    return (uint64_t) duration_cast<milliseconds> (steady_clock::now ().time_since_epoch ()).count ();
}

// One line the user can act on, from the controller's status.
GS::UniString DescribeStatus (const evp::grasshopper::WorkflowStatus& status, bool hasHost)
{
    if (!hasHost)
        return "Grasshopper: not running. Open Tapioca > Grasshopper Editor to start it.";

    if (status.failure != evp::grasshopper::protocol::FailureCode::None) {
        // The category's own sentence, plus whatever the worker said about this
        // particular instance of it. The category alone is too general to act on
        // and the worker's message alone has no shape.
        GS::UniString line = GS::UniString ("Grasshopper: ") +
                             GS::UniString (evp::grasshopper::protocol::DescribeFailureCode (status.failure)) + ".";
        if (!status.message.empty ())
            line += " " + FromStd (status.message);
        return line;
    }

    if (status.busy)
        return "Grasshopper: solving...";

    if (status.hasCurrentSolution)
        return GS::UniString::Printf ("Grasshopper: solution %u is current.",
                                      (unsigned int) status.currentSolutionRevision);

    return GS::UniString ("Grasshopper: ") +
           GS::UniString (evp::grasshopper::protocol::DescribeSessionState (status.state)) + ".";
}

} // namespace

void ControlPalette::CreateWorkflowBand ()
{
    // The sub-object's own items first, then the band's. One call from the
    // shell's constructor rather than two, because the shell's line budget only
    // ever goes down and a band that needs two lines to come up can spend one of
    // them here instead.
    workflow.Create ();

    const DG::Rect seed (Margin, 0, Margin + WorkflowButtonWidth, RowHeight);

    workflowStatusText = std::make_unique<DG::LeftText> (*this, seed);
    workflowStatusText->SetText ("Grasshopper: not running.");

    workflowLoadButton = std::make_unique<DG::Button> (*this, seed);
    workflowLoadButton->SetText ("Definition...");
    workflowLoadButton->Attach (*this);

    workflowSolveButton = std::make_unique<DG::Button> (*this, seed);
    workflowSolveButton->SetText ("Solve");
    workflowSolveButton->Attach (*this);

    workflowCancelButton = std::make_unique<DG::Button> (*this, seed);
    workflowCancelButton->SetText ("Cancel");
    workflowCancelButton->Attach (*this);
}

short ControlPalette::PlaceWorkflowBand (short top, short left, short right, const evp::PaletteScroll& clip)
{
    // ⚠️ THE WHOLE BAND IS HIDDEN UNTIL A WORKER IS UP. A palette that always
    // showed a Grasshopper block would be advertising a feature to every user
    // who has no Rhino installed, and the row of dead buttons would be the first
    // thing they saw. It appears when the host does.
    if (!evp::grasshopper::GhWorkerHost::Get ().IsRunning ()) {
        clip.Place (workflowStatusText.get (), DG::Rect (left, top, left, top));
        clip.Place (workflowLoadButton.get (), DG::Rect (left, top, left, top));
        clip.Place (workflowSolveButton.get (), DG::Rect (left, top, left, top));
        clip.Place (workflowCancelButton.get (), DG::Rect (left, top, left, top));
        return 0;
    }

    short y = top;
    clip.Place (workflowStatusText.get (), DG::Rect (left, y, right, (short) (y + 16)));
    y = (short) (y + 22);

    short x = left;
    clip.Place (workflowLoadButton.get (), DG::Rect (x, y, (short) (x + WorkflowButtonWidth), (short) (y + RowHeight)));
    x = (short) (x + WorkflowButtonWidth + WorkflowButtonGap);
    clip.Place (workflowSolveButton.get (),
                DG::Rect (x, y, (short) (x + WorkflowButtonWidth), (short) (y + RowHeight)));
    x = (short) (x + WorkflowButtonWidth + WorkflowButtonGap);
    clip.Place (workflowCancelButton.get (),
                DG::Rect (x, y, (short) (x + WorkflowButtonWidth), (short) (y + RowHeight)));
    y = (short) (y + RowHeight + RowGap);

    // The generated rows below the buttons, in the same scrolled column.
    y = (short) (y + workflow.PlaceAt (y, left, right, clip));

    return (short) (y - top);
}

void ControlPalette::ShowWorkflowControls ()
{
    workflow.ShowControls ();
}

void ControlPalette::RefreshWorkflowBand ()
{
    evp::grasshopper::GhWorkerHost& host = evp::grasshopper::GhWorkerHost::Get ();
    const bool hasHost = host.IsRunning ();
    evp::grasshopper::GhWorkflowController& controller = host.Workflow ();

    // ⚠️ THE SCHEMA IS COMPARED, NOT WATCHED. A new one means a different
    // definition (or the same one re-read), and rebuilding on every idle would
    // discard whatever the user had typed sixty times a second.
    const std::string schema = controller.SchemaJson ();
    if (hasHost && schema != lastWorkflowSchema) {
        lastWorkflowSchema = schema;
        if (!schema.empty ()) {
            workflow.Rebuild (schema);
            // A new row set changes the height of everything below it, so the
            // whole column is placed again and then shown -- the same order the
            // command block uses, and the reason Rebuild only builds.
            Layout ();
            ShowWorkflowControls ();
            Redraw ();

            const std::vector<std::string>& errors = workflow.SchemaErrors ();
            if (!errors.empty ())
                SetCommandStatus (FromStd (errors.front ()));
        }
    }

    if (hasHost && workflow.HasSchema ()) {
        // The read-back drives the settle window: the controller is told the
        // snapshot whenever it CHANGED, and Tick decides when that has been
        // still long enough to be worth solving. Polling rather than reacting to
        // each keystroke is deliberate -- a per-keystroke event would restart the
        // window on every digit of a number being typed.
        const evp::WorkflowSnapshot snapshot = workflow.Collect ();
        if (snapshot.ok && snapshot.values != lastWorkflowValues) {
            lastWorkflowValues = snapshot.values;
            std::vector<evp::grasshopper::protocol::SessionInputValue> inputs;
            inputs.reserve (snapshot.ids.size ());
            for (size_t index = 0; index < snapshot.ids.size (); ++index)
                inputs.push_back ({ snapshot.ids[index], snapshot.values[index] });
            controller.SetInputs (inputs, evp::grasshopper::protocol::SolveWantsPreview |
                                              evp::grasshopper::protocol::SolveWantsData);
        }

        controller.Tick (NowMs ());
    }

    const evp::grasshopper::WorkflowStatus status = controller.Status ();
    const GS::UniString line = DescribeStatus (status, hasHost);
    // Compared before it is written: SetText on an unchanged string still
    // invalidates the item, and this runs on every idle tick.
    if (line != lastWorkflowStatus) {
        lastWorkflowStatus = line;
        if (workflowStatusText)
            workflowStatusText->SetText (line);
    }

    if (workflowSolveButton) {
        // Solve needs a session with a definition in it; Cancel needs a solve to
        // interrupt. Both are disabled rather than hidden, because a button that
        // comes and goes is harder to find than one that is greyed.
        if (hasHost && workflow.HasSchema ())
            workflowSolveButton->Enable ();
        else
            workflowSolveButton->Disable ();
    }
    if (workflowCancelButton) {
        if (hasHost && status.busy)
            workflowCancelButton->Enable ();
        else
            workflowCancelButton->Disable ();
    }
    if (workflowLoadButton) {
        if (hasHost)
            workflowLoadButton->Enable ();
        else
            workflowLoadButton->Disable ();
    }
}

bool ControlPalette::HandleWorkflowButton (const DG::ButtonClickEvent& ev)
{
    if (workflowLoadButton && ev.GetSource () == workflowLoadButton.get ()) {
        ChooseWorkflowDefinition ();
        return true;
    }

    if (workflowSolveButton && ev.GetSource () == workflowSolveButton.get ()) {
        SolveWorkflowNow ();
        return true;
    }

    if (workflowCancelButton && ev.GetSource () == workflowCancelButton.get ()) {
        std::string error;
        if (!evp::grasshopper::GhWorkerHost::Get ().Workflow ().CancelSolve (error))
            SetCommandStatus (FromStd (error));
        return true;
    }

    return false;
}

void ControlPalette::ChooseWorkflowDefinition ()
{
    FTM::FileTypeManager fileTypeManager ("Tapioca.Grasshopper");
    const FTM::GroupID filterRoot = fileTypeManager.AddGroup ("Grasshopper definitions");
    // Both spellings: GH_Archive.ReadFromFile picks binary or XML by CONTENT, so
    // offering only .gh would hide half the definitions a user has for no reason
    // this side understands.
    const FTM::TypeID binary =
        fileTypeManager.AddType (FTM::FileType ("Grasshopper definition (*.gh)", "gh", 0, 0, 0), filterRoot);
    const FTM::TypeID xml =
        fileTypeManager.AddType (FTM::FileType ("Grasshopper definition (*.ghx)", "ghx", 0, 0, 0), filterRoot);

    DG::FileDialog dialog (DG::FileDialog::OpenFile);
    if (binary != FTM::UnknownType)
        dialog.AddFilter (binary);
    if (xml != FTM::UnknownType)
        dialog.AddFilter (xml);
    dialog.SetFilterRoot (filterRoot);

    if (!dialog.Invoke ())
        return; // cancelled, which is not worth a status line

    GS::UniString path;
    if (dialog.GetSelectedFile ().ToPath (&path) != NoError || path.IsEmpty ()) {
        SetCommandStatus ("That file's location could not be read.");
        return;
    }

    evp::grasshopper::GhWorkflowController& controller = evp::grasshopper::GhWorkerHost::Get ().Workflow ();

    GS::UniString error;
    // ⚠️ THE SESSION IS OPENED LAZILY, ON THE FIRST DEFINITION. Opening one when
    // the worker starts would put a session on every worker, including the ones
    // started only to show the editor -- and a session holds a document.
    if (controller.SessionId () == 0) {
        std::string openError;
        if (controller.OpenSession (evp::grasshopper::protocol::SessionMode::Headless, openError) == 0) {
            SetCommandStatus (FromStd (openError));
            return;
        }
    }

    std::string loadError;
    if (!controller.LoadDefinition (ToStd (path), loadError)) {
        SetCommandStatus (FromStd (loadError));
        return;
    }

    // Cleared so the next poll rebuilds even if the new definition happens to
    // produce a byte-identical schema to the last one -- reloading the same file
    // after an edit is the ordinary case, and a comparison that skipped it would
    // leave the panel showing rows the document no longer has.
    lastWorkflowSchema.clear ();
    lastWorkflowValues.clear ();
    workflow.Clear ();
    SetCommandStatus (GS::UniString ("Loading ") + path + "...");
}

void ControlPalette::SolveWorkflowNow ()
{
    const evp::WorkflowSnapshot snapshot = workflow.Collect ();
    workflow.MarkRefused (snapshot.refused);

    if (!snapshot.ok) {
        // The first refusal in the status line and a mark on every offending
        // row: the line says what is wrong and the marks say where, and neither
        // alone is enough on a definition with thirty inputs.
        SetCommandStatus (snapshot.errors.empty () ? GS::UniString ("Some inputs were refused.")
                                                   : FromStd (snapshot.errors.front ()));
        Redraw ();
        return;
    }

    evp::grasshopper::GhWorkflowController& controller = evp::grasshopper::GhWorkerHost::Get ().Workflow ();

    std::vector<evp::grasshopper::protocol::SessionInputValue> inputs;
    inputs.reserve (snapshot.ids.size ());
    for (size_t index = 0; index < snapshot.ids.size (); ++index)
        inputs.push_back ({ snapshot.ids[index], snapshot.values[index] });

    lastWorkflowValues = snapshot.values;
    controller.SetInputs (inputs,
                          evp::grasshopper::protocol::SolveWantsPreview | evp::grasshopper::protocol::SolveWantsData);

    std::string error;
    if (!controller.SolveNow (error))
        SetCommandStatus (FromStd (error));
}
