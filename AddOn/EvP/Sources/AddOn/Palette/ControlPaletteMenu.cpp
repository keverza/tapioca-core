// The palette shell's RIGHT-CLICK HANDLER — a further implementation file for the
// same class, on the precedent of ControlPaletteLayout.cpp and ControlPaletteRun.cpp.
//
// WHY IT IS STILL THE SHELL: PanelContextMenuRequested is a DG::PanelObserver
// event, and CLAUDE.md (and every other handler in ControlPalette.cpp) assigns the
// DG event routing to the shell. A sub-object may not receive it — the shell is the
// sole registered observer. So the ROUTE is here and the MENU is a sub-object
// (Palette/PaletteContextMenu.hpp), which is the same division the action bar and
// the results table use.
//
// WHY IT IS ITS OWN FILE: ControlPalette.cpp sits exactly on its recorded size cap
// in tools/quality/check_cpp.py and may not grow. Its entry there says the next
// feature needing lines needs its own file; this is that.
//
// THIS ADDS NO ROUTE. ControlPalette already derives from DG::PanelObserver and
// already calls Attach (*this) — this is one more override on an observer that is
// registered either way. No .grc item, no resource, no new Attach, no new
// observer base class.
//
// MAIN THREAD: DG::ContextMenu::Display runs its own modal event loop and does not
// return until the menu closes. Everything Archicad-facing goes through
// MainThreadGate, so an open menu blocks the gate — which is why the guard below
// refuses to open one while a run, a selection prompt, or an Archicad busy state is
// live. The menu is opened only when the main thread has nothing else in flight,
// and then it blocks nothing that was not already idle.

#include "ControlPalette.hpp"

void ControlPalette::PanelContextMenuRequested (const DG::PanelContextMenuEvent& /*ev*/, bool* needHelp,
                                                bool* processed)
{
    // Decline the "What's this?" route — this panel has no help anchors.
    if (needHelp != nullptr)
        *needHelp = false;

    // The main-thread guard. Leaving `processed` alone hands the click back to DG,
    // so a right-click during a run behaves exactly as it did before this file
    // existed rather than appearing to do nothing.
    //
    // runActive: a command is running and its completion has to reach the main
    //            thread through the gate.
    // promptActive: E3 — the run is waiting for the user to select and press
    //            Continue, so the palette is mid-conversation.
    // itemsDisabled: Archicad has told us it is busy (project open/close).
    if (runActive.load () || promptActive.load () || itemsDisabled.load ())
        return;

    // DG::ContextMenu wants SCREEN coordinates. The event carries a position of its
    // own (ev.GetPosition ()), but the shipped DevKit example reads the mouse
    // instead (Examples/DG_Test/Src/FontPopupDialog.cpp), and that is the path
    // proven to place the menu correctly on both platforms.
    DG::MousePosData mouse;
    if (!mouse.Retrieve ())
        return;

    // ev.GetItem () is what a REGION-AWARE menu would branch on — it is the control
    // under the pointer, or null over bare panel. Deliberately unused here: this
    // first menu is the same everywhere, so there is nothing yet to branch on. See
    // docs/architecture/api/RMB-CONTEXT-MENU-LIMITS.md §2 for what each region can
    // be identified by, and §7 for what a probe still has to settle before the
    // per-region menus are worth building.
    const evp::PaletteContextMenu::Choice choice =
        contextMenu.Display (mouse.GetMouseOffsetInNativeUnits (), WhatIsMissing ().IsEmpty ());

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
    if (processed != nullptr)
        *processed = true;
}
