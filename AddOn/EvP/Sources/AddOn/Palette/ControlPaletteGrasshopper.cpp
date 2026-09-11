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
#include "Grasshopper/HostState.hpp"            // HostState; GhWorkerHost only forward-declares it
#include "NativeCommands/SelectionSetStore.hpp" // the definition's selection inputs are configured here
#include "PaletteMetrics.hpp"
#include "PaletteScroll.hpp"
#include "ResourceIds.hpp" // the band's Iconoir button art

#include "ArchViz/ArchVizPanel.hpp" // the Diligent viewer the preview is drawn in

#include "DGFileDialog.hpp"
#include "FileTypeManager.hpp"

#include <chrono>
#include <ctime>

using namespace evp::palette;

namespace {

// The band's own buttons, left to right: Definition, Reload, Solve, Cancel,
// Preview, Clear. Square icon cells rather than a share of the width -- six
// labelled buttons want about 560 pixels and a docked palette is often 320.
constexpr short WorkflowButtonCount = 9;
constexpr short WorkflowButtonGap = 4;
// Icon plus one word. Six of these want about 480 pixels, so the row WRAPS
// rather than overflowing -- see PlaceWorkflowBand.
constexpr short WorkflowButtonWidth = 76;
constexpr short WorkflowButtonHeight = 24;

// The transcript's smallest useful height. It normally runs to the bottom of the
// band instead (see PlaceWorkflowBand): on a command whose panel holds nothing
// else, every pixel below the inputs is the transcript's, and a fixed box left
// two thirds of the panel empty under it.
constexpr short MinWorkflowLogHeight = 96;

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

// The wall clock, for the transcript's headers only. Separate from NowMs and
// from a DIFFERENT clock deliberately: the settle window needs a monotonic one
// (a user changing the system time must not make a pending solve fire or hang),
// and a header needs a readable one. WorkflowLog is handed the text rather than
// reading either, so its tests can state the time.
std::string ClockText ()
{
    const std::time_t now = std::time (nullptr);
    std::tm parts = {};
    if (localtime_s (&parts, &now) != 0)
        return std::string ();

    char text[16] = {};
    if (std::strftime (text, sizeof (text), "%H:%M:%S", &parts) == 0)
        return std::string ();
    return std::string (text);
}

// One line the user can act on, from the controller's status.
// Which of its three jobs the Attach button is doing right now.
//
// ⚠️ ATTACHING HAS A WAIT IN THE MIDDLE, WHICH IS WHY ONE WORD IS NOT
// ENOUGH. Start spawns a worker and the worker connects within seconds; attach
// opens a pipe and then waits for a human to connect a Rhino to it, which may be
// a minute or never. A button that still said "Attach" during that wait would
// leave the user with nothing that means "never mind" -- and the same button
// once a peer is on the line has to mean disconnect, because the attached peer
// is the one thing the power button must not claim to own.
enum class AttachFace {
    Connect,     // nothing is up: open the pipe and wait
    Cancel,      // waiting for a peer: close the pipe, having started nothing
    Disconnect,  // a peer is connected: drop it, and leave it running
    Unavailable, // a worker WE spawned owns the host; attaching is not a choice
};

AttachFace WorkflowAttachFace (const evp::grasshopper::GhWorkerHost& host)
{
    if (!host.IsAttachedPeer ()) {
        const evp::grasshopper::HostState state = host.State ();
        const bool idle = state == evp::grasshopper::HostState::NotStarted ||
                          state == evp::grasshopper::HostState::Stopped || state == evp::grasshopper::HostState::Failed;
        return idle ? AttachFace::Connect : AttachFace::Unavailable;
    }

    // Attached and Running means the handshake completed -- a peer answered.
    // Attached and anything else means the pipe is open and nobody has.
    return host.IsRunning () ? AttachFace::Disconnect : AttachFace::Cancel;
}

short AttachFaceIcon (AttachFace face)
{
    switch (face) {
        case AttachFace::Cancel:
            return PaletteIconXmarkId;
        case AttachFace::Disconnect:
            return PaletteIconStopSolidId;
        case AttachFace::Connect:
        case AttachFace::Unavailable:
            break;
    }
    return PaletteIconIpAddressId;
}

const char* AttachFaceText (AttachFace face)
{
    switch (face) {
        case AttachFace::Cancel:
            return "Cancel";
        case AttachFace::Disconnect:
            return "Detach";
        case AttachFace::Connect:
        case AttachFace::Unavailable:
            break;
    }
    return "Connect";
}

GS::Array<GS::UniString> ToUniStringArray (const std::vector<std::string>& names)
{
    GS::Array<GS::UniString> out;
    for (const std::string& name : names)
        out.Push (GS::UniString (name.c_str (), CC_UTF8));
    return out;
}

GS::UniString DescribeStatus (const evp::grasshopper::WorkflowStatus& status, bool hasHost)
{
    if (!hasHost)
        return "Grasshopper: not running. Pick a definition below and it starts, headless.";

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

    // ⚠️ WHOSE PROCESS IT IS BELONGS IN THE STATUS LINE. "Grasshopper: loaded"
    // means something different when the peer is the user's own Rhino: Stop
    // will disconnect rather than close it, and a crash there is theirs to
    // notice. One word, and it is the word that makes Stop predictable.
    const bool attached = evp::grasshopper::GhWorkerHost::Get ().IsAttachedPeer ();

    const GS::UniString peer = attached ? GS::UniString (" (attached peer)") : GS::UniString ();

    if (status.hasCurrentSolution)
        return GS::UniString::Printf ("Grasshopper: solution %u is current.",
                                      (unsigned int) status.currentSolutionRevision) +
               peer;

    return GS::UniString ("Grasshopper: ") +
           GS::UniString (evp::grasshopper::protocol::DescribeSessionState (status.state)) + "." + peer;
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

    // ⚠️ DG::Button RATHER THAN DG::IconButton, BECAUSE A Button CARRIES BOTH.
    // DGButton.hpp: Button is an ItemIconProperty AND an ItemTextProperty;
    // IconButton is only the first. Six icons in a row with nothing to read is
    // a guessing game, so each button gets its icon AND a one-word caption --
    // which needs a control that can hold the two.
    //
    // ⚠️ NO TOOLTIPS, AND NOT FOR WANT OF TRYING. DG has no SetTooltipText: a
    // tooltip is either a 'DHLP' help resource reached through Item::SetHelp or
    // an ItemToolTipRequested override, and this add-on has neither -- no .grc
    // in the tree declares a help resource. The caption is what a reader gets
    // until one exists.
    const auto iconButton = [this, &seed] (short iconId, const char* caption) {
        auto button = std::make_unique<DG::Button> (*this, seed);
        button->SetIcon (DG::Icon (ACAPI_GetOwnResModule (), iconId));
        button->SetText (caption);
        button->Attach (*this);
        return button;
    };

    // ⚠️ THE WORKER NEEDS A STOP, AND UNTIL NOW THE ONLY ONE WAS QUITTING
    // ARCHICAD. Tapioca > Close Grasshopper exists, but a panel that starts a
    // process and cannot stop it is asking the user to keep Archicad open for
    // the sake of a Rhino they are done with -- and Rhino is a licence, not
    // just a process. One button, both ways: filled play to start, filled
    // square to stop, which is what a transport control has meant since tape
    // decks. Its icon and word are swapped in RefreshWorkflowBand, because
    // whether a worker is up is not knowable at construction.
    workflowPowerButton = iconButton (PaletteIconPlaySolidId, "Start");
    workflowLoadButton = iconButton (PaletteIconFolderId, "Open");
    workflowReloadButton = iconButton (PaletteIconRestartId, "Reload");
    workflowSolveButton = iconButton (PaletteIconPlayId, "Solve");
    workflowCancelButton = iconButton (PaletteIconXmarkId, "Cancel");

    // ⚠️ IT OPENS THE VIEWER, NOT THE OVERLAY, AND THE DIFFERENCE IS WHICH
    // ARCHICAD WINDOW IS IN FRONT. The Diligent OVERLAY composites onto the
    // frontmost document canvas, and there is no DevKit call that hands back the
    // 3D window's HWND -- opened with the floor plan in front it lands on the
    // floor plan, which for a Grasshopper preview is the wrong window and a
    // confusing one (AddOnMain.cpp says so where the overlay's menu item is).
    // The viewer is a palette of its own and depends on no window at all, so it
    // is what a button in another palette may safely open.
    workflowPreviewButton = iconButton (PaletteIconCubeScanId, "View");
    workflowClearButton = iconButton (PaletteIconEraseId, "Clear");

    // ⚠️ THE ONE BUTTON HERE THAT CHANGES THE MODEL, AND IT IS A LATCH RATHER
    // THAN AN ACTION FOR THAT REASON. Tapir's element-creation components write
    // when their capsule button is pressed, not while they solve, so a
    // committing solve is a different REQUEST -- SolveWants.Commit -- and not a
    // different button to press afterwards. Off by default; every ordinary
    // solve stays a read.
    //
    // ⚠️ AND THESE WRITES ARE NOT COVERED BY THE BRIDGE'S READ-ONLY GATE.
    // Tapir talks straight to Archicad's JSON port, so nothing in Tapioca's
    // transport sees the write, let alone refuses it. That is precisely why the
    // user has to arm it and why the transcript says so on every solve.
    //
    // The plus glyph, which is already the palette's "add" icon: what a commit
    // does is add to the model, and inventing a ninth piece of art for it would
    // say less.
    workflowCommitButton = iconButton (PaletteIconPlusId, "Not Commit"); // face set on refresh

    // ⚠️ ATTACH STARTS NOTHING, AND THAT IS THE POINT OF IT. Start spawns a
    // worker, which starts an embedded Rhino of its own; this only opens the
    // bridge and waits, so a Grasshopper already running in the user's own
    // Rhino can connect to it. No second RhinoCore, no seat taken from the
    // Rhino they are working in.
    //
    // The ip-address icon rather than a plug or an arrow: the address is what
    // this is about, and the same button grows a field when the transport
    // reaches beyond this machine.
    // Its icon and word are swapped in RefreshWorkflowBand, like the power
    // button's and for the same reason: which of the four jobs above it is doing
    // is not knowable at construction.
    workflowAttachButton = iconButton (PaletteIconIpAddressId, "Connect");

    // ReadOnly and framed, like the results box: it is a transcript, not a
    // field.
    //
    // ⚠️ VScroll, NOT HVScroll. A horizontal bar was a third scroll bar in a
    // column that already has two, to reach text that is clipped at 240
    // characters anyway -- WorkflowLog does the clipping, so nothing here is
    // wide enough to need scrolling sideways.
    workflowLogText = std::make_unique<DG::MultiLineEdit> (
        *this, DG::Rect (Margin, 0, Margin + 100, MinWorkflowLogHeight), DG::MultiLineEdit::VScroll,
        DG::EditControl::Frame, DG::EditControl::Update, DG::EditControl::ReadOnly);
}

short ControlPalette::PlaceWorkflowStatusLine (short top, short left, short right)
{
    if (workflowStatusText == nullptr)
        return 0;

    workflowStatusText->SetRect (DG::Rect (left, top, right, (short) (top + 16)));
    // NOT through `scroll`: this line is in the fixed head, and the whole point
    // of it being there is that a panel scrolled to the bottom still says
    // whether Grasshopper is up.
    if (!workflowStatusText->IsVisible ())
        workflowStatusText->Show ();
    return 22;
}

bool ControlPalette::IsWorkflowCommand (const evp::CommandInfo* command) const
{
    // The same shape as IsDynamoCommand: the catalog carries `runtime` as
    // free-form text, so it is compared in exactly one place and a null
    // selection answers "no" rather than making every caller ask.
    return command != nullptr && command->runtime == "grasshopper";
}

short ControlPalette::PlaceWorkflowBand (short top, short left, short right, const evp::PaletteScroll& clip,
                                         short bottom)
{
    // ⚠️ THE BAND BELONGS TO ONE COMMAND, NOT TO THE PALETTE, AND THAT REPLACED
    // A WHOLE PAGE. It used to appear whenever a worker was running, which put a
    // Grasshopper block under every Python command's parameters; the fix built
    // for that was a Grasshopper PAGE with its own switch, which was worse -- it
    // reimplemented the empty panel that a command declaring no parameters
    // already gives for free, with the command list, the Run button, the status
    // line and the results table all still in place. So the band is one
    // command's panel. Everything else in the palette is then somebody else's
    // problem, which is the entire benefit.
    if (!IsWorkflowCommand (SelectedCommand ())) {
        // HIDDEN, not placed at zero height. PaletteScroll::Place SHOWS whatever
        // it places, so an empty rect would leave five items visible at one
        // pixel -- and the buttons among them would still take clicks.
        DG::Item* const band[] = { workflowPowerButton.get (),  workflowAttachButton.get (),
                                   workflowLoadButton.get (),   workflowReloadButton.get (),
                                   workflowSolveButton.get (),  workflowCommitButton.get (),
                                   workflowCancelButton.get (), workflowPreviewButton.get (),
                                   workflowClearButton.get (),  workflowLogText.get () };
        for (DG::Item* item : band) {
            if (item != nullptr && item->IsVisible ())
                item->Hide ();
        }

        // ⚠️ AND THE GENERATED ROWS, WHICH ARE NOT IN THAT LIST BECAUSE THEY
        // ARE NOT THE SHELL'S. They reach the panel through the scroll, which
        // hides only what it is HANDED -- and this branch hands it nothing, so
        // without this the definition's inputs stayed on screen over the next
        // command's parameters. The rows are kept, values and all: coming back
        // to this command shows the definition as it was left.
        workflow.HideControls ();
        return 0;
    }

    short y = top;
    short x = left;
    DG::Item* const row[] = { workflowPowerButton.get (),  workflowAttachButton.get (),  workflowCommitButton.get (),
                              workflowLoadButton.get (),   workflowReloadButton.get (),  workflowSolveButton.get (),
                              workflowCancelButton.get (), workflowPreviewButton.get (), workflowClearButton.get () };
    for (DG::Item* button : row) {
        // ⚠️ THE ROW WRAPS, IT DOES NOT SHRINK. Six captioned buttons want more
        // width than a docked palette has, and dividing what there is by six
        // gives six buttons too narrow to read. A second line of full-size
        // buttons is legible at any width the palette can be dragged to.
        if (x > left && (short) (x + WorkflowButtonWidth) > right) {
            x = left;
            y = (short) (y + WorkflowButtonHeight + WorkflowButtonGap);
        }
        clip.Place (button, DG::Rect (x, y, (short) (x + WorkflowButtonWidth), (short) (y + WorkflowButtonHeight)));
        x = (short) (x + WorkflowButtonWidth + WorkflowButtonGap);
    }
    y = (short) (y + WorkflowButtonHeight + RowGap);

    // The generated rows below the buttons, in the same scrolled column.
    y = (short) (y + workflow.PlaceAt (y, left, right, clip));

    // ⚠️ THE TRANSCRIPT TAKES EVERY PIXEL LEFT, AND IT CAN BECAUSE IT IS LAST.
    // This band is the only thing in its command's panel, so whatever is below
    // the inputs is the transcript's -- and a fixed box left two thirds of a
    // tall palette empty underneath it. `bottom` is the scrolled viewport's
    // floor, which the shell knows and this does not, so it is passed in.
    // ⚠️ AND IT ENDS EXACTLY THERE, WITH NO GAP AFTER IT. A single trailing
    // RowGap put the column's content one row below the viewport, which gave
    // the palette a scroll bar of its own beside the transcript's -- two
    // vertical bars, side by side, one of which could move the column by six
    // pixels. Whatever this band reports as its height is what the shell adds
    // to the column, so the height has to be the truth.
    const short available = (short) (bottom - y);
    const short logHeight = available > MinWorkflowLogHeight ? available : MinWorkflowLogHeight;
    clip.Place (workflowLogText.get (), DG::Rect (left, y, right, (short) (y + logHeight)));
    y = (short) (y + logHeight);

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
    // ⚠️ THE WORKER STARTING IS A LAYOUT EVENT, AND NOTHING ELSE DELIVERS IT.
    // The band has no height at all without a worker, and a worker is started
    // from the Tapioca menu -- an action this panel never hears about. Without
    // this the band stays collapsed until something unrelated resizes the
    // palette, which reads as the feature not being there at all.
    if (hasHost != lastWorkflowHasHost) {
        lastWorkflowHasHost = hasHost;

        // ⚠️ THE POWER BUTTON'S TWO FACES ARE SWAPPED HERE, NOT IN THE GATES
        // BELOW, AND THE FIRST ATTEMPT AT IT WAS DEAD CODE. Comparing the new
        // face against lastWorkflowHasHost down there can never differ: this
        // block has already assigned it. The transition is the only moment the
        // face changes, so this is where it belongs -- and SetIcon on an
        // unchanged icon still invalidates the item, which on an idle tick is a
        // repaint sixty times a second.
        if (workflowPowerButton) {
            workflowPowerButton->SetIcon (
                DG::Icon (ACAPI_GetOwnResModule (), hasHost ? PaletteIconStopSolidId : PaletteIconPlaySolidId));
            // ⚠️ ONE OWNER PER MODE. The attach button carries the whole
            // attached lifecycle (connect / cancel / detach), so this one speaks
            // only for a worker we spawned; it is disabled below while a peer we
            // did not start is on the line, rather than offering a second word
            // for the same act.
            workflowPowerButton->SetText (hasHost ? "Stop" : "Start");
        }
        if (!hasHost) {
            // The session died with the worker. Keeping its rows would offer
            // controls that write into a document no process holds any more.
            workflow.Clear ();
            // The sets described a definition that is gone with the worker.
            geomsrv::SelectionSetStore::Get ().Clear ();
            lastWorkflowSchema.clear ();
            lastWorkflowValues.clear ();
            // The transcript is NOT cleared: what the last session did is the
            // most useful thing on screen right after it stopped doing it. The
            // line below says the worker went, which is the part that changed.
            workflowLog.Note ("--- the Grasshopper worker stopped ---");
        }
        else {
            workflowLog.Note ("--- Grasshopper worker ready ---");
        }
        Layout ();
        ShowWorkflowControls ();
        Redraw ();
    }

    const std::string schema = controller.SchemaJson ();
    if (hasHost && schema != lastWorkflowSchema) {
        lastWorkflowSchema = schema;
        if (!schema.empty ()) {
            workflow.Rebuild (schema);

            // ⚠️ PER DEFINITION, NOT PER COMMAND, AND THAT IS THE WHOLE RULE.
            // The Grasshopper command declares no selection sets: most
            // definitions never touch an Archicad element, and a selection row
            // that does nothing is worse than no row. This definition's own
            // schema decides -- one row per "Tapioca Selection" input it
            // declares, named after the input, built by the same
            // SelectionSetPanel a Python command's selection_sets produces.
            // An empty list rebuilds to nothing, which is how the row goes away
            // again when the next definition does not ask for elements.
            //
            // Rebuild also Configures the store, so the roles a definition
            // declares are exactly the roles that can be mutated.
            // ⚠️ THE STORE, NOT A SECOND PANEL. The buttons live on the input's
            // own row in this band, so the input keeps the position its author
            // gave it on the canvas -- the same rule every other input follows.
            // Configuring the store is all that is needed from outside: it is
            // what makes a role this definition declares mutable and any other
            // role refused.
            geomsrv::SelectionSetStore::Get ().Configure (ToUniStringArray (workflow.SelectionRoles ()));

            // A new row set changes the height of everything below it, so the
            // whole column is placed again and then shown -- the same order the
            // command block uses, and the reason Rebuild only builds.
            Layout ();
            ShowWorkflowControls ();
            Redraw ();

            workflowLog.Note ("Loaded '" + workflow.WorkflowName () + "'.");

            // ⚠️ THE DEFINITION'S OWN WORDS GO IN THE COMMAND'S DESCRIPTION
            // BAND, replacing the command's. The command is a shell for this
            // panel and has nothing to say about the definition in it; the
            // author does, through a Tapioca Description component. Rebuilt
            // here rather than by the description band polling, because a
            // schema arriving is the only moment it changes.
            const std::string& blurb = workflow.WorkflowDescription ();
            description.Rebuild (FromStd (workflow.WorkflowName ()),
                                 blurb.empty () ? GS::UniString ("This definition carries no description. Wire a "
                                                                 "panel into a Tapioca Description component to give "
                                                                 "it one.")
                                                : FromStd (blurb));

            const std::vector<std::string>& errors = workflow.SchemaErrors ();
            if (!errors.empty ())
                SetCommandStatus (FromStd (errors.front ()));
            // ⚠️ EVERY schema error, not just the first. The status line has
            // room for one and a definition with three bad inputs has three
            // things wrong with it; the transcript is where the other two live.
            for (const std::string& error : errors)
                NoteWorkflowLine ("! " + error);
        }
    }

    // ⚠️ NO SOLVE HAPPENS HERE, AND THE SETTLE WINDOW IS DELIBERATELY UNUSED.
    // The band did solve on a debounced input change, which measured badly the
    // moment a real definition was in it: every character typed into a text
    // input became its own solve, so a five-letter word was five solves of a
    // definition that takes as long as it takes. The controller's debounce is
    // still there and still tested -- it is what a slider drag will want -- but
    // nothing drives it from here. Solve is a button, and a button is a thing
    // the user presses when they mean it.
    //
    // The definition the user picked before a worker existed is loaded here, on
    // the first tick that has one. Nothing else in this palette hears a worker
    // come up.
    if (hasHost && !pendingWorkflowPath.empty ()) {
        const std::string path = pendingWorkflowPath;
        pendingWorkflowPath.clear ();
        LoadWorkflowDefinition (path);
    }

    // ⚠️ THE SOLUTION IS POLLED, NOT PUSHED, FOR THE SAME REASON THE SCHEMA IS.
    // It arrives on the bridge's IO thread and a DG write from there is the one
    // thing DG does not survive. WorkflowLog::Record ignores a revision it has
    // already written, so polling costs a comparison.
    evp::grasshopper::StoredSolution solution;
    if (hasHost && controller.CurrentSolution (solution))
        workflowLog.Record (solution, ClockText ());

    if (workflowLogText && workflowLog.Revision () != lastWorkflowLogRevision) {
        lastWorkflowLogRevision = workflowLog.Revision ();
        const GS::UniString text = FromStd (workflowLog.Text ());
        workflowLogText->SetText (text);
        // The caret parked at the end, which is what scrolls the box there: the
        // newest line is the one just written, and a transcript that has to be
        // dragged to be read is a transcript nobody reads. An empty selection
        // rather than a range -- selecting the text would make it look pasted
        // over on every solve.
        const Int32 end = (Int32) text.GetLength ();
        workflowLogText->SetSelection (DG::CharRange (end, end));
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
    // ⚠️ DEFINITION IS NEVER DISABLED, AND DISABLING IT WAS A DEADLOCK. It
    // starts the worker too, and it used to be greyed out until one was
    // running -- so the only way in was the Editor menu item, which is exactly
    // the canvas this panel exists to avoid opening.
    if (workflowLoadButton)
        workflowLoadButton->Enable ();
    if (workflowPowerButton) {
        // Stopping an attached peer is the attach button's job, and Start would
        // be a second Rhino on top of the one already answering.
        if (evp::grasshopper::GhWorkerHost::Get ().IsAttachedPeer ())
            workflowPowerButton->Disable ();
        else
            workflowPowerButton->Enable ();
    }
    if (workflowAttachButton) {
        // ⚠️ COMPARED, NOT ASSIGNED EVERY IDLE. SetIcon and SetText
        // invalidate the item even when the value is unchanged, and this runs on
        // every idle tick: assigning unconditionally is a repaint sixty times a
        // second. The face is derived from the host, so the previous face is the
        // only thing worth remembering.
        const AttachFace face = WorkflowAttachFace (evp::grasshopper::GhWorkerHost::Get ());
        if ((short) face != lastWorkflowAttachFace) {
            lastWorkflowAttachFace = (short) face;
            workflowAttachButton->SetIcon (DG::Icon (ACAPI_GetOwnResModule (), AttachFaceIcon (face)));
            workflowAttachButton->SetText (AttachFaceText (face));
        }
        if (face == AttachFace::Unavailable)
            workflowAttachButton->Disable ();
        else
            workflowAttachButton->Enable ();
    }
    // The selection verbs change the store, and their counts sit beside them.
    workflow.RefreshSelections ();

    if (workflowCommitButton) {
        // The word carries the state, because there is no checkbox here and a
        // button that looked identical armed and unarmed would be a trap.
        // ⚠️ BOTH FACES NAMED: "Commit" alone read as an action and was
        // pressed as one, but it is a LATCH and the Solve after it writes.
        workflowCommitButton->SetText (workflowCommit ? "Commit" : "Not Commit");
        workflowCommitButton->SetIcon (
            DG::Icon (ACAPI_GetOwnResModule (), workflowCommit ? PaletteIconPlusCircleSolidId : PaletteIconPlusId));
        if (hasHost && !loadedWorkflowPath.empty ())
            workflowCommitButton->Enable ();
        else
            workflowCommitButton->Disable ();
    }
    if (workflowReloadButton) {
        if (hasHost && !loadedWorkflowPath.empty ())
            workflowReloadButton->Enable ();
        else
            workflowReloadButton->Disable ();
    }
}

bool ControlPalette::HandleWorkflowButton (const DG::ButtonClickEvent& ev)
{
    // ⚠️ FIRST, BECAUSE THESE ARE ROWS RATHER THAN BAND CONTROLS. A selection
    // input's five verbs are built by the band's own panel, one row per input, so
    // the band asks it before testing any of its own buttons.
    if (workflow.HandleSelectionButton (ev.GetSource ())) {
        Redraw ();
        return true;
    }

    if (workflowPowerButton && ev.GetSource () == workflowPowerButton.get ()) {
        evp::grasshopper::GhWorkerHost& host = evp::grasshopper::GhWorkerHost::Get ();
        if (host.IsRunning ()) {
            // ⚠️ THE SAME BUTTON MEANS TWO THINGS, AND THE HOST DECIDES WHICH.
            // A worker we spawned is shut down cooperatively and then
            // terminated if it will not go; a peer we attached to is only
            // DISCONNECTED, because it is somebody's Rhino and taking it down
            // is not ours to do. Said in the transcript either way, since which
            // one happened matters to whoever presses it.
            const bool owned = !host.IsAttachedPeer ();
            host.Stop ();
            workflowLog.Note (owned ? "--- stop requested ---" : "--- detached; the peer was left running ---");
        }
        else {
            GS::UniString message;
            if (!host.EnsureHeadless (message))
                workflowLog.Note ("! " + ToStd (message));
            else
                workflowLog.Note ("Starting Grasshopper, headless ...");
            SetCommandStatus (message);
        }
        return true;
    }

    if (workflowAttachButton && ev.GetSource () == workflowAttachButton.get ()) {
        evp::grasshopper::GhWorkerHost& attachHost = evp::grasshopper::GhWorkerHost::Get ();
        const AttachFace face = WorkflowAttachFace (attachHost);

        // ⚠️ THE FACE DECIDES, NOT A SECOND READ OF THE STATE. Whatever the
        // button says is what the user asked for, and deriving it once means the
        // word on screen and the act cannot disagree.
        if (face == AttachFace::Cancel || face == AttachFace::Disconnect) {
            const bool waiting = face == AttachFace::Cancel;
            attachHost.Stop ();
            workflowLog.Note (waiting ? "--- stopped waiting for a peer; nothing was started ---"
                                      : "--- detached; the peer was left running ---");
            SetCommandStatus (
                GS::UniString (waiting ? "Grasshopper: not waiting for a peer." : "Grasshopper: detached."));
            return true;
        }

        GS::UniString message;
        const bool opened = attachHost.AttachLocal (message);
        // The pipe name is IN the message, and it is the one thing the peer
        // needs. Said in the transcript rather than a dialog: a modal that has
        // to be dismissed before the name can be copied is a modal in the way.
        NoteWorkflowLine ((opened ? "" : "! ") + ToStd (message));
        SetCommandStatus (message);
        return true;
    }

    if (workflowCommitButton && ev.GetSource () == workflowCommitButton.get ()) {
        workflowCommit = !workflowCommit;
        // ⚠️ ARMING DOES NOT SOLVE. Toggling this into a solve would make one
        // click both "I mean it" and "do it now", and the whole point of a latch
        // is that the user reads the state before pressing Solve.
        NoteWorkflowLine (workflowCommit ? "Commit ARMED - a latch, not an action: press Solve to write, and that "
                                           "solve presses the definition's Tapir Execute buttons."
                                         : "Not Commit. Solves are reads again.");
        RefreshWorkflowBand ();
        Redraw ();
        return true;
    }

    if (workflowReloadButton && ev.GetSource () == workflowReloadButton.get ()) {
        // ⚠️ THE SAME PATH AS A FRESH PICK, NOT A ReloadDefinition MESSAGE. The
        // protocol has one, and it means "re-read the file you have"; loading
        // the path again means the same thing to the worker and one thing less
        // here. When a reload has to differ from a load -- keeping typed values
        // across an edit, say -- that is when the message earns its place.
        if (loadedWorkflowPath.empty ())
            SetCommandStatus ("No definition is loaded yet.");
        else
            LoadWorkflowDefinition (loadedWorkflowPath);
        return true;
    }

    if (workflowClearButton && ev.GetSource () == workflowClearButton.get ()) {
        workflowLog.Clear ();
        // The transcript clears itself on every solution anyway; this is for
        // reading one solution's block without the notes above it, and for
        // starting a session's reading from a known-empty box.
        workflowLog.Note ("(cleared)");
        return true;
    }

    if (workflowPreviewButton && ev.GetSource () == workflowPreviewButton.get ()) {
        // ⚠️ IT SOLVES NOW, AND OPENING THE VIEWER ALONE WAS THE BUG. A
        // preview is not a window, it is a RESULT: pressing this used to show an
        // empty viewer and wait for somebody to press Solve, which is two presses
        // for one intention and an empty window in between.
        PreviewWorkflowNow ();

        // After the request, not before: the viewer is where the preview will
        // appear, and it is opened whether or not this session has produced one
        // yet -- an empty viewer that says so beats a button that looks dead.
        ArchVizPanel::OpenViewer ();
        return true;
    }

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
        if (evp::grasshopper::GhWorkerHost::Get ().Workflow ().CancelSolve (error)) {
            workflowLog.Note ("Cancel requested.");
        }
        else {
            SetCommandStatus (FromStd (error));
            workflowLog.Note ("! " + error);
        }
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

    // ⚠️ THE WORKER IS STARTED HERE, HEADLESS, AND NOT WHEN THE COMMAND IS
    // SELECTED. Starting Rhino costs seconds and a licence; spending both
    // because somebody clicked down a list would be spending them on a glance.
    // Picking a definition is the first gesture that cannot mean anything else.
    //
    // ⚠️ AND HEADLESS IS THE PANEL'S START, NOT THE MENU'S. Tapioca >
    // Grasshopper Editor asks the same worker for its canvas; this asks for no
    // canvas at all, which is the whole difference between the two (one
    // ShowEditor message -- see GhWorkerHost::EnsureHeadless). A panel that
    // opened the editor to load a file would put a Grasshopper window in front
    // of Archicad every time somebody wanted a number.
    evp::grasshopper::GhWorkerHost& host = evp::grasshopper::GhWorkerHost::Get ();
    if (!host.IsRunning ()) {
        GS::UniString startMessage;
        if (!host.EnsureHeadless (startMessage)) {
            SetCommandStatus (startMessage);
            workflowLog.Note ("! " + ToStd (startMessage));
            return;
        }

        // Remembered rather than loaded: the worker answers when it answers,
        // and the idle tick that sees it come up will load this.
        pendingWorkflowPath = ToStd (path);
        workflowLog.Note ("Starting Grasshopper, then loading " + pendingWorkflowPath + " ...");
        SetCommandStatus (startMessage);
        return;
    }

    LoadWorkflowDefinition (ToStd (path));
}

void ControlPalette::LoadWorkflowDefinition (const std::string& path)
{
    evp::grasshopper::GhWorkflowController& controller = evp::grasshopper::GhWorkerHost::Get ().Workflow ();

    // ⚠️ THE SESSION IS OPENED LAZILY, ON THE FIRST DEFINITION. Opening one when
    // the worker starts would put a session on every worker, including the ones
    // started only to show the editor -- and a session holds a document.
    if (controller.SessionId () == 0) {
        std::string openError;
        if (controller.OpenSession (evp::grasshopper::protocol::SessionMode::Headless, openError) == 0) {
            SetCommandStatus (FromStd (openError));
            workflowLog.Note ("! " + openError);
            return;
        }
    }

    std::string loadError;
    if (!controller.LoadDefinition (path, loadError)) {
        SetCommandStatus (FromStd (loadError));
        workflowLog.Note ("! " + loadError);
        return;
    }

    // Cleared so the next poll rebuilds even if the new definition happens to
    // produce a byte-identical schema to the last one -- reloading the same file
    // after an edit is the ordinary case, and a comparison that skipped it would
    // leave the panel showing rows the document no longer has.
    lastWorkflowSchema.clear ();
    lastWorkflowValues.clear ();
    workflow.Clear ();
    // The next definition declares its own selection inputs, or none.
    geomsrv::SelectionSetStore::Get ().Clear ();
    loadedWorkflowPath = path;

    // ⚠️ A NEW DEFINITION DISARMS THE COMMIT, AND CARRYING IT OVER WOULD BE THE
    // WORST DEFAULT IN THE PANEL. The user armed a commit for the definition
    // they were reading; the next file's Execute buttons write something they
    // have not looked at yet. Arming is per definition, on purpose.
    workflowCommit = false;
    // ⚠️ FORGET, NOT CLEAR. The previous definition's solution block describes a
    // document that is about to be gone, and leaving it above the new one's
    // rows is the misleading state the Clear button was being asked to fix by
    // hand. Forget also drops the logged mark, so the new definition's first
    // solution is written even if its revision counter starts lower.
    workflowLog.Forget ();
    workflowLog.Note ("Loading " + path + " ...");
    SetCommandStatus (GS::UniString ("Loading ") + FromStd (path) + "...");
}

void ControlPalette::SolveWorkflowNow ()
{
    uint32_t wants = evp::grasshopper::protocol::SolveWantsPreview | evp::grasshopper::protocol::SolveWantsData;
    if (workflowCommit)
        wants |= evp::grasshopper::protocol::SolveWantsCommit;

    SendWorkflowSolve (wants, workflowCommit);
}

// ⚠️ PREVIEW ONLY, AND NEVER A COMMIT -- NOT EVEN WHILE COMMIT IS ARMED.
// "Resolve the script but do not create geometry in Archicad" is exactly one bit
// of difference from a solve: Commit is what presses Tapir's Execute buttons, so
// leaving it out is what makes this safe to press while looking at something.
// The arming is left alone rather than cleared: it belongs to the Solve button,
// and silently disarming it here would make the next Solve do less than the user
// had set up.
//
// SolveWantsData is left out too, because the user asked for the preview to
// update and nothing else: a data pass would re-publish every output value into
// the panel's table for a press that was about seeing the geometry.
void ControlPalette::PreviewWorkflowNow ()
{
    SendWorkflowSolve (evp::grasshopper::protocol::SolveWantsPreview, false);
}

void ControlPalette::SendWorkflowSolve (uint32_t wants, bool committing)
{
    const evp::WorkflowSnapshot snapshot = workflow.Collect ();
    workflow.MarkRefused (snapshot.refused);

    // ⚠️ A REFUSED ROW NO LONGER STOPS THE SOLVE, AND STOPPING WAS WRONG. One
    // definition is not one calculation: an enum whose author gave it no
    // choices, or a number typed outside its bounds, has nothing to send -- and
    // the twelve other inputs, and every part of the definition that does not
    // depend on the bad one, are still perfectly solvable. Refusing the whole
    // press made the panel useless until every row was clean, which on a
    // definition being authored is never. The bad rows are marked, named in the
    // transcript, and LEFT AT WHATEVER THE DEFINITION ALREADY HOLDS.
    std::vector<evp::grasshopper::protocol::SessionInputValue> inputs;
    std::vector<std::string> sent;
    inputs.reserve (snapshot.ids.size ());
    sent.reserve (snapshot.ids.size ());
    for (size_t index = 0; index < snapshot.ids.size (); ++index) {
        if (index < snapshot.refused.size () && snapshot.refused[index])
            continue;
        inputs.push_back ({ snapshot.ids[index], snapshot.values[index] });
        sent.push_back (snapshot.values[index]);
    }

    if (!snapshot.errors.empty ()) {
        // The first refusal in the status line and a mark on every offending
        // row: the line says what is wrong and the marks say where, and neither
        // alone is enough on a definition with thirty inputs.
        SetCommandStatus (FromStd (snapshot.errors.front ()));
        // Deduplicated against the line above it, because one bad row is
        // reported by the schema AND by the read-back, in the same words.
        for (const std::string& error : snapshot.errors)
            NoteWorkflowLine ("! " + error);
        Redraw ();
    }

    if (inputs.empty () && !snapshot.ids.empty ()) {
        // Nothing survived. Solving with no inputs at all would silently use
        // the definition's saved values, which is a different answer from the
        // one the panel is showing.
        NoteWorkflowLine ("! Every input was refused, so nothing was sent.");
        return;
    }

    evp::grasshopper::GhWorkflowController& controller = evp::grasshopper::GhWorkerHost::Get ().Workflow ();

    lastWorkflowValues = sent;
    // ⚠️ Preview|Data IS A READ; Commit IS WHAT MAKES A SOLVE WRITE. The bit
    // has been in the protocol since the session messages were written and
    // nothing asked for it, which is why a Tapir definition run from this panel
    // solved cleanly and created nothing.
    if (committing) {
        // Said on EVERY committing solve, not once when the button was armed: a
        // latch the user set two minutes ago is a latch they have forgotten, and
        // this is the line that stops a re-solve writing a second set of walls
        // without warning.
        NoteWorkflowLine ("Commit armed: this solve presses the definition's Tapir Execute buttons.");
    }

    controller.SetInputs (inputs, wants);

    const bool previewOnly = wants == (uint32_t) evp::grasshopper::protocol::SolveWantsPreview;

    std::string error;
    if (controller.SolveNow (error)) {
        NoteWorkflowLine (previewOnly ? "Preview requested: solving without writing anything to Archicad."
                                      : "Solve requested.");
    }
    else {
        SetCommandStatus (FromStd (error));
        NoteWorkflowLine ("! " + error);
    }
}

void ControlPalette::NoteWorkflowLine (const std::string& line)
{
    // ⚠️ THE SAME LINE TWICE RUNNING IS A REPETITION, NOT INFORMATION. An enum
    // with no choices is refused by discovery AND by the read-back, in the same
    // sentence, so the transcript showed it two and three times over. Compared
    // against the previous line only: the same message arriving again after
    // something else happened is a second event and worth saying.
    if (!workflowLog.Lines ().empty () && workflowLog.Lines ().back () == line)
        return;
    workflowLog.Note (line);
}
