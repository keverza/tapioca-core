#include "Palette/PaletteContextMenu.hpp"

#include "ResourceMDIDIds.hpp" // AC_MDID_DEV / AC_MDID_LOC, parsed from AddOnFix.grc

namespace evp {

namespace {

// The menu's own command ids. They are ours to choose: a DG::Command is identified
// by the (id, developerId, productId) triple, and the developer/product half is our
// MDID — so these small numbers cannot collide with Archicad's own commands. The
// command-declared entries start well above the built-ins so the two ranges can
// never meet however many entries a command declares.
enum MenuCommandId : ULong { RunCommandId = 1, RescanCommandId = 2, HideCommandId = 3, FirstEntryCommandId = 100 };

// The region a "param:<name>" entry is pinned to, without the name.
const char* const ParamRegionPrefix = "param:";

} // namespace

bool PaletteContextMenu::ShowsIn (const GS::UniString& declared, const GS::UniString& clicked)
{
    if (declared == "panel") // everywhere, which is the default a command gets
        return true;

    // "params" is the whole parameter block, so it also covers a click that
    // resolved to one named control. "param:<name>" is that control alone.
    if (declared == "params")
        return clicked.BeginsWith (ParamRegionPrefix);

    return declared == clicked;
}

PaletteContextMenu::PaletteContextMenu ()
    : runCommand (RunCommandId, AC_MDID_DEV, AC_MDID_LOC), rescanCommand (RescanCommandId, AC_MDID_DEV, AC_MDID_LOC),
      hideCommand (HideCommandId, AC_MDID_DEV, AC_MDID_LOC)
{
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

DG::Command PaletteContextMenu::EntryCommand (USize index, const GS::UniString& label)
{
    while (entryCommands.size () <= index) {
        const ULong id = FirstEntryCommandId + (ULong) entryCommands.size ();
        const DG::Command command (id, AC_MDID_DEV, AC_MDID_LOC);
        commands.Add (command, new DG::CommandDescriptor (command, label));
        entryCommands.push_back (command);
    }

    const DG::Command& command = entryCommands[index];
    // The slot is reused across commands and regions, so the text is set every
    // time — this is the relabel the pool exists for.
    commands[command]->SetText (label);
    return command;
}

PaletteContextMenu::Result PaletteContextMenu::Display (const DG::NativePoint& at, bool runReady,
                                                        const CommandInfo* info, const GS::UniString& region)
{
    // Which of the selected command's entries belong over THIS region, in the order
    // the author declared them. Kept as indices into menuItems so the answer can be
    // turned back into the name run_action takes.
    std::vector<USize> shown;
    if (info != nullptr) {
        // The three arrays are written together by the scanner but cross the bus
        // separately — so trust the shortest rather than indexing off the end. A
        // malformed scan costs an entry, never a crash. Same rule as ActionBar.
        const USize count =
            GS::Min (info->menuItems.GetSize (), GS::Min (info->menuLabels.GetSize (), info->menuRegions.GetSize ()));
        for (USize index = 0; index < count; ++index) {
            if (ShowsIn (info->menuRegions[index], region))
                shown.push_back (index);
        }
    }

    // Built per display, not held: the entries change with the command AND with the
    // region, and DG::Menu has no way to remove what it already has.
    DG::Menu menu ("-");

    // Slot N of the pool is menu position N, which is what makes the chosen id
    // readable back as an index into `shown` below.
    for (USize slot = 0; slot < shown.size (); ++slot)
        menu.AddMenuItem (DG::MenuSimpleItem (EntryCommand (slot, info->menuLabels[shown[slot]])));

    if (!shown.empty ())
        menu.AddMenuItem (DG::MenuSeparatorItem ());

    // The two things the palette does with the command it is showing, then the one
    // thing it does with itself. The command's own entries come FIRST: they are what
    // the user aimed at, and the built-ins are the same three everywhere.
    menu.AddMenuItem (DG::MenuSimpleItem (runCommand));
    menu.AddMenuItem (DG::MenuSimpleItem (rescanCommand));
    menu.AddMenuItem (DG::MenuSeparatorItem ());
    menu.AddMenuItem (DG::MenuSimpleItem (hideCommand));

    // Per-click state. The descriptors persist, so this is a mutation, not a
    // rebuild — which is the cheap half of why they are pooled.
    commands[runCommand]->SetStatus (runReady);

    DG::ContextMenu contextMenu ("-", &menu);
    contextMenu.SetEnabledCommands (commands);

    // ⚠️ Blocking: this does not return until the menu closes.
    DG::CommandEvent* const chosen = contextMenu.Display (at);
    if (chosen == nullptr) // dismissed
        return Result ();

    const ULong id = chosen->GetCommand ().GetCommandId ();
    delete chosen; // the caller owns the event DG hands back

    Result result;
    switch (id) {
        case RunCommandId:
            result.choice = Choice::Run;
            return result;
        case RescanCommandId:
            result.choice = Choice::Rescan;
            return result;
        case HideCommandId:
            result.choice = Choice::Hide;
            return result;
        default:
            break;
    }

    // A command entry. The id says which POOL SLOT was picked, and the slots were
    // filled in the order `shown` lists — so the slot is the position in that list.
    if (id >= FirstEntryCommandId && info != nullptr) {
        const USize slot = (USize) (id - FirstEntryCommandId);
        if (slot < shown.size ()) {
            result.choice = Choice::Action;
            result.action = info->menuItems[shown[slot]];
        }
    }
    return result;
}

} // namespace evp
