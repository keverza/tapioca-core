// The palette shell's RIGHT-CLICK HANDLERS — a further implementation file for the
// same class, on the precedent of ControlPaletteLayout.cpp and ControlPaletteRun.cpp.
//
// WHY IT IS STILL THE SHELL: these are DG observer events, and CLAUDE.md (and every
// other handler in ControlPalette.cpp) assigns the DG event routing to the shell. A
// sub-object may not receive one — the shell is the sole registered observer. So the
// ROUTES are here and the MENU is a sub-object (Palette/PaletteContextMenu.hpp),
// which is the same division the action bar and the results table use.
//
// WHY IT IS ITS OWN FILE: ControlPalette.cpp sits exactly on its recorded size cap
// in tools/quality/check_cpp.py and may not grow. Its entry there says the next
// feature needing lines needs its own file; this is that.
//
// ── WHY THREE HANDLERS ─────────────────────────────────────────────────────────
// One panel-level handler does NOT cover a panel. Measured in Archicad with only
// PanelContextMenuRequested overridden:
//
//     background   yes        text edits / generated inputs   NO
//     buttons      yes        command list + search field     NO
//     static text  yes
//
// Two different DG causes, and each has its own hook:
//
//   * DG::ListBox OVERRIDES SpecContextMenuRequested (DGListBox.hpp:454) and
//     re-dispatches the event as ListBoxContextMenuRequested. It is intercepted
//     before the panel ever sees it, so the panel handler cannot reach a list box.
//     ListView, TreeView and TabBar do the same.
//
//   * Edit controls raise the event at ITEM level. DGEditControl.hpp overrides no
//     Spec* context method at all, so nothing re-routes it — but the panel handler
//     is not where it lands either. ItemContextMenuRequested is the hook every
//     control shares, and every generated parameter control and the command search
//     field is attached to THIS object, so one override serves all of them.
//     (If a native edit control turns out to consume the right-click entirely, this
//     override will simply never fire for it and the inputs stay as they are —
//     nothing else regresses. That is the one outcome still unmeasured.)
//
// ── CONTEXT SENSITIVITY ────────────────────────────────────────────────────────
// The menu is not one menu. RegionOf () turns the item under the pointer into a
// region name — "panel", "params", "param:<name>", "commands", "results" — and the
// selected command's own entries (@tapioca.menu in its Python source, carried
// through CommandInfo::menuItems) are filtered to it. The built-in three are always
// there underneath, so every region is a superset of the same floor.
//
// A chosen entry runs through RunSelected, which is the action-bar path: a menu
// entry IS an action, and nothing about executing one is new machinery.
//
// THIS ADDS NO ROUTE. ControlPalette already derives from DG::PanelObserver and
// DG::ListBoxObserver, already reaches ItemObserver through six of its bases, and
// already Attach()es every item involved. No .grc item, no resource, no new Attach,
// no new observer base class.
//
// MAIN THREAD: DG::ContextMenu::Display runs its own modal event loop and does not
// return until the menu closes. Everything Archicad-facing goes through
// MainThreadGate, so an open menu blocks the gate — which is why ShowContextMenu
// refuses to open one while a run, a selection prompt, or an Archicad busy state is
// live. The menu is opened only when the main thread has nothing else in flight,
// and then it blocks nothing that was not already idle.

#include "ControlPalette.hpp"

#include "Python/PathUtils.hpp" // EvpDataDir / AppendTextLine — the region trace

#include <ctime>

// The guard, the position, the display and the dispatch — shared by all three
// events, so the three handlers below are each two lines and there is exactly one
// place the menu's behaviour lives.
namespace {

// One line into logs\commands.log per right-click, beside the run headers the same
// file already carries.
//
// WHY IT EXISTS: the menu entries a command declares are the only other evidence of
// where a click landed, and they only report the region a user actually PICKS. A
// region whose menu never opened and a region whose entry was simply not chosen
// look identical from the log — which is exactly the question that could not be
// answered the first time this was measured. This line is written whether or not
// anything is picked, and whether or not the menu opens at all.
//
// "--" rather than the run header's "=====": a right-click is not a run, and a
// reader scanning for runs must not have to filter these out by eye.
void TraceContextMenu (const GS::UniString& region, const GS::UniString& outcome)
{
    const GS::UniString dataDir = evp::EvpDataDir ();
    if (dataDir.IsEmpty ())
        return;

    const std::time_t now = std::time (nullptr);
    std::tm local {};
#if defined(_WIN32)
    localtime_s (&local, &now);
#else
    local = *std::localtime (&now);
#endif

    evp::AppendTextLine (
        dataDir + GS::UniString ("\\logs\\commands.log"),
        GS::UniString::Printf ("-- %02d:%02d:%02d right-click: region=", local.tm_hour, local.tm_min, local.tm_sec) +
            region + "  " + outcome);
}

} // namespace

// Which region of the palette the pointer was over, in the vocabulary
// @tapioca.menu declares against. The whole of the palette's context sensitivity is
// this function: everything downstream is a string compare (evp::PaletteContextMenu
// ::ShowsIn), so a new region is one line here and one word in the Python docstring.
//
// Each band answers for its own items — the shell asks, it does not reach in. That
// is the same seam ButtonClicked and PanelWheelTracked already use.
GS::UniString ControlPalette::RegionOf (const DG::Item* clicked) const
{
    if (clicked == nullptr) // bare panel, between the bands
        return "panel";

    // A generated parameter control names its own port, so an entry can be pinned
    // to one row: "param:offset" appears over offset's field, its label and (for a
    // FilePath) its Browse button, and nowhere else.
    const GS::UniString param = params.ParamNameAt (clicked);
    if (!param.IsEmpty ())
        return GS::UniString ("param:") + param;

    if (commandsPanel.IsSource (clicked)) // the list and its search field
        return "commands";
    if (results.IsSource (clicked))
        return "results";

    // Buttons, the status lines, the server band: all palette furniture. A command
    // cannot pin an entry to them, and the built-ins are the same everywhere, so
    // they answer as the panel does.
    return "panel";
}

bool ControlPalette::ShowContextMenu (const DG::Item* clicked)
{
    // The main-thread guard. A false return leaves `processed` alone, which hands
    // the click back to DG — so a right-click during a run behaves exactly as it
    // did before this file existed rather than appearing to do nothing.
    //
    // runActive: a command is running and its completion has to reach the main
    //            thread through the gate.
    // promptActive: E3 — the run is waiting for the user to select and press
    //            Continue, so the palette is mid-conversation.
    // itemsDisabled: Archicad has told us it is busy (project open/close).
    const GS::UniString region = RegionOf (clicked);

    if (runActive.load () || promptActive.load () || itemsDisabled.load ()) {
        // Traced, not silent: "the menu did not open" and "the menu opened and you
        // dismissed it" are indistinguishable to the user, and only one of them is
        // this add-on refusing on purpose.
        TraceContextMenu (region, "refused (a run is in flight)");
        return false;
    }

    // ⚠️ ONE CLICK, POSSIBLY TWO EVENTS. Now that both the item and the panel hook
    // are live, a control that raises the item event and then lets the panel event
    // through would open the menu twice in a row for one right-click. DG documents
    // no ordering here, so this is measured defensively rather than assumed: a
    // second request arriving within a blink of the last menu CLOSING is the same
    // click, and is dropped. A human cannot deliberately right-click twice that
    // fast; a duplicated event always is that fast.
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ();
    if (now - lastContextMenuAt < std::chrono::milliseconds (300)) {
        // Also traced. If DG never duplicates, this line never appears and the guard
        // can go; if it does, the log says so instead of the count of runs being
        // quietly double what the user clicked.
        TraceContextMenu (region, "dropped (duplicate event for one click)");
        return false;
    }

    // DG::ContextMenu wants SCREEN coordinates. The events carry a position of their
    // own (ev.GetPosition ()), but the shipped DevKit example reads the mouse
    // instead (Examples/DG_Test/Src/FontPopupDialog.cpp), and that is the path
    // proven to place the menu correctly — and the one path that is identical for
    // all three events.
    DG::MousePosData mouse;
    if (!mouse.Retrieve ())
        return false;

    const evp::PaletteContextMenu::Result picked = contextMenu.Display (
        mouse.GetMouseOffsetInNativeUnits (), WhatIsMissing ().IsEmpty (), SelectedCommand (), region);
    // Display BLOCKS for as long as the menu is open, so the stamp goes here, on the
    // close — not on the open.
    lastContextMenuAt = std::chrono::steady_clock::now ();

    switch (picked.choice) {
        case evp::PaletteContextMenu::Choice::Run:
            RunSelected ();
            break;
        case evp::PaletteContextMenu::Choice::Rescan:
            Rescan (); // exactly what the Rescan button does, and no more
            break;
        case evp::PaletteContextMenu::Choice::Hide:
            Hide ();
            break;
        case evp::PaletteContextMenu::Choice::Action:
            // The region goes WITH it: the command reads it as `ctx.region`, so one
            // entry declared for a whole area can still tell which control it was
            // aimed at.
            // A menu entry IS an action: the same RunSelected the action bar's
            // buttons use, so the worker, the token, the Cancel role and the
            // completion path are all the ones that already exist. Nothing about
            // running a right-click entry is a special case.
            RunSelected (picked.action, region);
            break;
        case evp::PaletteContextMenu::Choice::None:
            break;
    }

    // What the user did with it — the half of the evidence the command's own log
    // line cannot carry, because a built-in entry runs no command code at all.
    GS::UniString outcome ("shown, nothing picked");
    switch (picked.choice) {
        case evp::PaletteContextMenu::Choice::Run:
            outcome = "shown, ran the command";
            break;
        case evp::PaletteContextMenu::Choice::Rescan:
            outcome = "shown, rescanned";
            break;
        case evp::PaletteContextMenu::Choice::Hide:
            outcome = "shown, hid the palette";
            break;
        case evp::PaletteContextMenu::Choice::Action:
            outcome = GS::UniString ("shown, ran ") + picked.action;
            break;
        case evp::PaletteContextMenu::Choice::None:
            break;
    }
    TraceContextMenu (region, outcome);

    // The menu was shown, so the click was ours whether or not anything was picked.
    return true;
}

// The panel background, the buttons and the static text.
void ControlPalette::PanelContextMenuRequested (const DG::PanelContextMenuEvent& ev, bool* needHelp, bool* processed)
{
    // Decline the "What's this?" route — this panel has no help anchors.
    if (needHelp != nullptr)
        *needHelp = false;

    // GetItem () is the control under the pointer, or null over bare panel — the
    // one thing the region needs.
    if (ShowContextMenu (ev.GetItem ()) && processed != nullptr)
        *processed = true;
}

// Every generated parameter control and the command search field. One override
// covers them all because they all attach to this object.
void ControlPalette::ItemContextMenuRequested (const DG::ItemContextMenuEvent& ev, bool* needHelp, bool* processed)
{
    if (needHelp != nullptr)
        *needHelp = false;

    if (ShowContextMenu (ev.GetSource ()) && processed != nullptr)
        *processed = true;
}

// The command list and the results table — the two list boxes, which intercept the
// event themselves and would never reach the panel handler.
//
// ⚠️ ev.GetItem () is the ROW UNDER THE POINTER, which is not necessarily the
// SELECTED row, so this handler passes GetSource () — the list box — and the region
// is "commands", not one row. Run therefore still runs the SELECTED command, not
// the one that was right-clicked. Making the click select first is the next thing
// this region should gain.
void ControlPalette::ListBoxContextMenuRequested (const DG::ListBoxContextMenuEvent& ev, bool* processed)
{
    if (ShowContextMenu (ev.GetSource ()) && processed != nullptr)
        *processed = true;
}
