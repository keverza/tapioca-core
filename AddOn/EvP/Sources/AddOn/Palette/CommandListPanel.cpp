#include "APIEnvir.h"
#include "ACAPinc.h"

#include "Palette/CommandListPanel.hpp"
#include "Palette/PaletteMetrics.hpp" // Margin / RowHeight — shared with the shell
#include "Palette/CommandFilter.hpp"  // F2 search: scoring/ranking, DevKit-free
#include "ControlPalette.hpp"         // the shell — the sole registered DG observer

#include <algorithm>

using namespace evp::palette;

namespace evp {

namespace {

// U+25BC BLACK DOWN-POINTING TRIANGLE, as UTF-8. Built at runtime as a UniString for
// the same reason as the shell's play/stop glyphs: the .grc is compiled through a
// 1252 codepage, which would mangle it.
GS::UniString DropArrowLabel ()
{
    return GS::UniString ("\xE2\x96\xBC", CC_UTF8);
}

// The arrow's width on the combo's row. Narrow on purpose — it is a native button,
// so anything wider reads as a second action rather than part of one control.
constexpr short ArrowWidth = 20;

} // namespace

CommandListPanel::CommandListPanel (const DG::Panel& panel, ControlPalette& observer, DG::SingleSelListBox& list)
    : panel (panel), observer (observer), list (list)
{
}

void CommandListPanel::Create ()
{
    list.SetTabFieldCount (1);
    // The combo starts CLOSED, and a .grc item starts visible — so the list has to be
    // taken off screen explicitly, the mirror image of the rule that a runtime-created
    // item starts hidden.
    list.Hide ();

    // The combo's text half. Runtime-built like the shell's Continue button so no
    // .grc id is renumbered. Shown immediately: unlike a generated parameter control
    // it is always relevant.
    searchField = std::make_unique<DG::TextEdit> (panel, DG::Rect (Margin, 0, Margin + 100, RowHeight));
    searchField->Attach (observer);
    searchField->Show ();

    // ...and its arrow. A plain Button, not a PushCheck: the dropdown is not a state
    // the user holds, it is one that a pick ends, so a button that stayed pressed
    // would be telling the truth for about a second and lying afterwards.
    dropButton = std::make_unique<DG::Button> (panel, DG::Rect (Margin, 0, Margin + ArrowWidth, RowHeight));
    dropButton->SetText (DropArrowLabel ());
    dropButton->Attach (observer);
    dropButton->Show ();
}

void CommandListPanel::FocusSearch ()
{
    if (searchField)
        searchField->SetFocus ();
}

// Running the scanner and reading its JSON is Palette/CommandScan's job — this only
// moves the answer onto the panel.
GS::UniString CommandListPanel::Rescan ()
{
    const ScanOutcome outcome = ScanCommandFolders ();
    commands = outcome.commands;

    bool selectionChanged = false;
    ApplyFilter (selectionChanged); // the caller rebuilds the block regardless

    // The scan decided what is selected, so the closed combo has to say so — this is
    // what puts the first command's title in the field at startup.
    if (!editing)
        AdoptSelection ();

    return outcome.status;
}

GS::UniString CommandListPanel::Query () const
{
    if (!editing || !searchField)
        return GS::UniString ();
    return searchField->GetText ();
}

void CommandListPanel::AdoptSelection ()
{
    const CommandInfo* const info = Selected ();
    displayText = (info != nullptr) ? info->title : GS::UniString ();
    editing = false;
    if (searchField)
        searchField->SetText (displayText);
    // Move the watermark WITH the text: the idle poll's whole test is "did the field
    // stop matching what I last put there", and text we wrote ourselves must not
    // come back to us one tick later as if the user had typed it.
    lastSearchText = displayText;
}

bool CommandListPanel::RefreshSearch (bool& selectionChanged)
{
    selectionChanged = false;
    if (!searchField)
        return false;

    const GS::UniString text = searchField->GetText ();
    if (text == lastSearchText)
        return false;

    lastSearchText = text;
    // Anything other than the selection's own title is a query, and a query the user
    // cannot see the results of is useless — so typing is the second way the dropdown
    // opens, next to the arrow.
    const bool wasOpen = open;
    editing = (text != displayText);
    if (editing)
        open = true;

    const bool rebuilt = ApplyFilter (selectionChanged);
    // OPENING counts as a change even when the rows did not move — a keystroke that
    // ranks the same set (a trailing space, a character that narrows nothing) still
    // has to make the list appear, and only the caller can lay it out.
    return rebuilt || open != wasOpen;
}

bool CommandListPanel::HandleButtonClicked (const DG::ButtonClickEvent& ev)
{
    if (!dropButton || ev.GetSource () != dropButton.get ())
        return false;

    if (open) {
        CloseList ();
        return true;
    }

    // Opening from the arrow is a BROWSE, not a search: drop whatever query was in
    // the field so the whole list is there to scroll, which is the one thing the
    // arrow can do that typing cannot.
    AdoptSelection ();
    bool selectionChanged = false;
    ApplyFilter (selectionChanged);
    open = true;
    return true;
}

void CommandListPanel::CloseList ()
{
    open = false;
    AdoptSelection ();
}

// F2 — the list is whatever the query lets through, best match first. Called on
// rescan, on every keystroke the idle poll notices, and when the arrow browses; the
// only place rows are made.
bool CommandListPanel::ApplyFilter (bool& selectionChanged)
{
    // Remember WHAT is selected, not WHICH ROW — the rows are about to change
    // meaning. Re-selecting row 1 blindly would silently point Run at a different
    // command than the one that was on screen a moment ago.
    const CommandInfo* const was = Selected ();
    const GS::UniString wasFolder = (was != nullptr) ? was->folder : GS::UniString ();

    const GS::UniString query = Query ();
    const std::vector<std::string> terms = SearchTermsOf (query);

    // Decide the whole answer BEFORE touching DG: ranking is arithmetic, rebuilding
    // a list box is not, and this runs on every keystroke.
    std::vector<std::pair<int, UIndex>> ranked; // (score, index into `commands`)
    for (UIndex i = 0; i < commands.size (); ++i) {
        const int score = ScoreCommand (Searchable (commands[i]), terms);
        if (score > 0)
            ranked.push_back ({ score, i });
    }
    // Best first, STABLY — with an empty query every score is equal, so the list
    // keeps exactly the category grouping the scan sorted it into.
    std::stable_sort (
        ranked.begin (), ranked.end (),
        [] (const std::pair<int, UIndex>& a, const std::pair<int, UIndex>& b) { return a.first > b.first; });

    std::vector<UIndex> next;
    for (const std::pair<int, UIndex>& hit : ranked)
        next.push_back (hit.second);

    // A keystroke that changes nothing must not rebuild the list — that is visible
    // flicker for no result. The empty case falls through: its row quotes the query.
    if (!next.empty () && next == visibleCommands)
        return false;

    visibleCommands = next;

    // One repaint, not one per row.
    list.DisableDraw ();
    list.DeleteItem (DG::ListBox::AllItems);

    short reselect = 1;
    for (UIndex row = 0; row < visibleCommands.size (); ++row) {
        const CommandInfo& info = commands[visibleCommands[row]];
        const GS::UniString rowLabel = info.category + " / " + info.title;
        list.AppendItem ();
        // The number is the command's ORIGINAL position, not the row: "run 7" has
        // to mean the same command whatever happens to be typed in the box.
        list.SetTabItemText ((short) (row + 1), 1,
                             GS::UniString::Printf ("%d.  %T", (int) (visibleCommands[row] + 1), rowLabel.ToPrintf ()));
        if (info.folder == wasFolder)
            reselect = (short) (row + 1);
    }

    // Nothing matched — say so IN THE LIST rather than leave an empty box that reads
    // as a broken scan. Disabled: it is not a command.
    if (visibleCommands.empty () && !commands.empty ()) {
        list.AppendItem ();
        list.SetTabItemText (1, 1, GS::UniString ("No command matches \"") + query + "\"");
        list.DisableItem (1);
    }
    else if (!visibleCommands.empty ()) {
        list.SelectItem (reselect);
    }
    list.EnableDraw ();

    const CommandInfo* const now = Selected ();
    selectionChanged = (now == nullptr || now->folder != wasFolder);
    return true;
}

const CommandInfo* CommandListPanel::Selected () const
{
    const short selected = list.GetSelectedItem ();
    if (selected < 1 || (size_t) selected > visibleCommands.size ())
        return nullptr; // no selection, or the "nothing matched" row
    return &commands[visibleCommands[(size_t) selected - 1]];
}

// The combo spans the panel: field, then the drop arrow at the right edge. It has no
// caption — the shell's "Commands" label is retired, because a combo holding a
// command's name directly under the row that runs it says so by itself.
//
// Placed STRAIGHT ON THE PANEL, not through PaletteScroll: this band is part of the
// palette's fixed head, so it stays on screen at any panel height, and the dropdown
// opens by pushing the scrolled column down rather than over it.
short CommandListPanel::PlaceAt (short bandTop, short left, short right)
{
    short y = bandTop;

    const short arrowLeft = (short) (right - ArrowWidth);
    searchField->SetRect (DG::Rect (left, y, arrowLeft, (short) (y + RowHeight)));
    searchField->Show ();
    dropButton->SetRect (DG::Rect (arrowLeft, y, right, (short) (y + RowHeight)));
    dropButton->Show ();
    y += RowHeight + 4;

    top = y;
    if (open) {
        list.SetRect (DG::Rect (left, y, right, (short) (y + height)));
        list.Show ();
        y += height;
    }
    else {
        list.Hide ();
    }

    return (short) (y - bandTop);
}

bool CommandListPanel::IsSource (const DG::Item* item) const
{
    return item == &list;
}

} // namespace evp
