#include "Palette/PaletteContextMenu.hpp"

#include "ResourceMDIDIds.hpp" // AC_MDID_DEV / AC_MDID_LOC, parsed from AddOnFix.grc

namespace evp {

namespace {

// The menu's own command ids. They are ours to choose: a DG::Command is identified
// by the (id, developerId, productId) triple, and the developer/product half is our
// MDID — so these small numbers cannot collide with Archicad's own commands.
enum MenuCommandId : ULong { RunCommandId = 1, RescanCommandId = 2, HideCommandId = 3 };

} // namespace

PaletteContextMenu::PaletteContextMenu ()
    : runCommand (RunCommandId, AC_MDID_DEV, AC_MDID_LOC), rescanCommand (RescanCommandId, AC_MDID_DEV, AC_MDID_LOC),
      hideCommand (HideCommandId, AC_MDID_DEV, AC_MDID_LOC), menu ("-")
{
    // The two things the palette does with the command it is showing, then the one
    // thing it does with itself. The separator is what says they are different
    // kinds of act.
    menu.AddMenuItem (DG::MenuSimpleItem (runCommand));
    menu.AddMenuItem (DG::MenuSimpleItem (rescanCommand));
    menu.AddMenuItem (DG::MenuSeparatorItem ());
    menu.AddMenuItem (DG::MenuSimpleItem (hideCommand));

    // The labels the user reads. Deliberately the SAME words as the buttons that do
    // the same work, so the menu never looks like a second, different feature.
    commands.Add (runCommand, new DG::CommandDescriptor (runCommand, "Run"));
    commands.Add (rescanCommand, new DG::CommandDescriptor (rescanCommand, "Rescan Commands"));
    commands.Add (hideCommand, new DG::CommandDescriptor (hideCommand, "Hide Palette"));
}

PaletteContextMenu::~PaletteContextMenu ()
{
    // DG::CommandTable holds raw owned pointers — see the header.
    for (auto it = commands.EnumerateValues (); it != nullptr; ++it)
        delete *it;
}

PaletteContextMenu::Choice PaletteContextMenu::Display (const DG::NativePoint& at, bool runReady)
{
    // Per-click state. The descriptors persist, so this is a mutation, not a
    // rebuild — which is the cheap half of why the menu is built once.
    commands[runCommand]->SetStatus (runReady);

    DG::ContextMenu contextMenu ("-", &menu);
    contextMenu.SetEnabledCommands (commands);

    // ⚠️ Blocking: this does not return until the menu closes.
    DG::CommandEvent* const chosen = contextMenu.Display (at);
    if (chosen == nullptr) // dismissed
        return Choice::None;

    const ULong id = chosen->GetCommand ().GetCommandId ();
    delete chosen; // the caller owns the event DG hands back

    switch (id) {
        case RunCommandId:
            return Choice::Run;
        case RescanCommandId:
            return Choice::Rescan;
        case HideCommandId:
            return Choice::Hide;
        default:
            return Choice::None;
    }
}

} // namespace evp
