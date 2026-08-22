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

// The guard, the position, the display and the dispatch — shared by all three
// events, so the three handlers below are each two lines and there is exactly one
// place the menu's behaviour lives.
bool ControlPalette::ShowContextMenu ()
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
    if (runActive.load () || promptActive.load () || itemsDisabled.load ())
        return false;

    // ⚠️ ONE CLICK, POSSIBLY TWO EVENTS. Now that both the item and the panel hook
    // are live, a control that raises the item event and then lets the panel event
    // through would open the menu twice in a row for one right-click. DG documents
    // no ordering here, so this is measured defensively rather than assumed: a
    // second request arriving within a blink of the last menu CLOSING is the same
    // click, and is dropped. A human cannot deliberately right-click twice that
    // fast; a duplicated event always is that fast.
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ();
    if (now - lastContextMenuAt < std::chrono::milliseconds (300))
        return false;

    // DG::ContextMenu wants SCREEN coordinates. The events carry a position of their
    // own (ev.GetPosition ()), but the shipped DevKit example reads the mouse
    // instead (Examples/DG_Test/Src/FontPopupDialog.cpp), and that is the path
    // proven to place the menu correctly — and the one path that is identical for
    // all three events.
    DG::MousePosData mouse;
    if (!mouse.Retrieve ())
        return false;

    const evp::PaletteContextMenu::Choice choice =
        contextMenu.Display (mouse.GetMouseOffsetInNativeUnits (), WhatIsMissing ().IsEmpty ());
    // Display BLOCKS for as long as the menu is open, so the stamp goes here, on the
    // close — not on the open.
    lastContextMenuAt = std::chrono::steady_clock::now ();

    switch (choice) {
        case evp::PaletteContextMenu::Choice::Run:
            RunSelected ();
            break;
        case evp::PaletteContextMenu::Choice::Rescan:
            Rescan (); // exactly what the Rescan button does, and no more
            break;
        case evp::PaletteContextMenu::Choice::Hide:
            Hide ();
            break;
        case evp::PaletteContextMenu::Choice::None:
            break;
    }

    // The menu was shown, so the click was ours whether or not anything was picked.
    return true;
}

// The panel background, the buttons and the static text.
//
// ev.GetItem () is what a REGION-AWARE menu would branch on — it is the control
// under the pointer, or null over bare panel. Deliberately unused: this menu is the
// same everywhere, so there is nothing yet to branch on. See
// docs/architecture/api/RMB-CONTEXT-MENU-LIMITS.md §2 for what each region can be
// identified by.
void ControlPalette::PanelContextMenuRequested (const DG::PanelContextMenuEvent& /*ev*/, bool* needHelp,
                                                bool* processed)
{
    // Decline the "What's this?" route — this panel has no help anchors.
    if (needHelp != nullptr)
        *needHelp = false;

    if (ShowContextMenu () && processed != nullptr)
        *processed = true;
}

// Every generated parameter control and the command search field. One override
// covers them all because they all attach to this object.
void ControlPalette::ItemContextMenuRequested (const DG::ItemContextMenuEvent& /*ev*/, bool* needHelp, bool* processed)
{
    if (needHelp != nullptr)
        *needHelp = false;

    if (ShowContextMenu () && processed != nullptr)
        *processed = true;
}

// The command list and the results table — the two list boxes, which intercept the
// event themselves and would never reach the panel handler.
//
// ⚠️ ev.GetItem () is the ROW UNDER THE POINTER, which is not necessarily the
// SELECTED row. The menu's Run therefore still runs the selected command, not the
// one that was right-clicked. Uniform with every other region for now; making the
// click select first is the first thing a per-region menu should change.
void ControlPalette::ListBoxContextMenuRequested (const DG::ListBoxContextMenuEvent& /*ev*/, bool* processed)
{
    if (ShowContextMenu () && processed != nullptr)
        *processed = true;
}
