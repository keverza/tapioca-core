#include "APIEnvir.h"
#include "ACAPinc.h"

#include "Palette/CommandListPanel.hpp"
#include "Palette/PaletteMetrics.hpp" // Margin / RowHeight — shared with the shell
#include "Palette/CommandFilter.hpp"  // F2 search: scoring/ranking, DevKit-free
#include "DGNativeContexts.hpp"       // UserItemUpdateNativeContext — the arrow cell draws itself
#include "ControlPalette.hpp"         // the shell — the sole registered DG observer

#include <algorithm>

using namespace evp::palette;

namespace evp {

namespace {

// The chevron the arrow cell draws — two strokes, no glyph. It used to be U+25BC
// set as a native button's TEXT, which meant the .grc codepage had to be worked
// around and, worse, that the cell rendered as a raised push button next to the
// field. A combo's arrow is part of the field, so it is drawn: the cell is a
// DG::UserItem carrying the FIELD's background colour, with this on top.
constexpr float ChevronHalfWidth = 3.5f;
constexpr float ChevronHeight = 2.5f;
constexpr float ChevronStroke = 1.0f;

// The field's own background, from the appearance manager rather than a literal
// white — the palette follows Archicad's theme, and a hard-coded white cell would
// be a bright square in a dark one.
Gfx::Color FieldBackground ()
{
    return DG::ColorCatalog::GetColor (DG::ColorId::StandardTextBackgroundColor);
}

// `t` of the way from `from` to `to`. The appearance manager publishes text and
// background colours but NO control border and NO hover colour, so both are mixed
// from the two it does publish: that keeps them right side up in a dark theme,
// which a literal grey would not be.
Gfx::Color Blend (const Gfx::Color& from, const Gfx::Color& to, float t)
{
    const auto mix = [t] (UChar a, UChar b) {
        return (UChar) ((float) a + ((float) b - (float) a) * t);
    };
    return Gfx::Color (mix (from.GetRed (), to.GetRed ()), mix (from.GetGreen (), to.GetGreen ()),
                       mix (from.GetBlue (), to.GetBlue ()));
}

// The frame the cell shares with the field. A third of the way to the text colour
// is the weight Windows' own edit border sits at (~#ABADB3 on white).
Gfx::Color FieldBorder ()
{
    return Blend (FieldBackground (), DG::ColorCatalog::GetColor (DG::ColorId::ControlTextColor), 0.35f);
}

// The pointer is over the cell. A WASH OF THE SELECTION COLOUR, not a grey: grey
// reads as disabled, and Archicad's own hovers are its blue. A quarter strength,
// because this is the arrow half of one field rather than a button — it
// acknowledges the pointer rather than lighting up.
Gfx::Color HoverBackground ()
{
    return Blend (FieldBackground (), DG::ColorCatalog::GetColor (DG::ColorId::SelectedContentBackgroundColor),
                  0.25f);
}

// How far inside the drawn box the field sits: the border, and a pixel of air so
// the text does not touch it.
constexpr short FrameInset = 2;

// The arrow strip's width at the row's right end. Narrow on purpose — it is part
// of the field, so anything wider reads as a second control rather than one box.
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
    // it is always relevant. NATIVE FRAME, as DG gives it by default: the platform's
    // own edit box, with the background, the focus ring and the text it comes with.
    searchField = std::make_unique<DG::TextEdit> (panel, DG::Rect (Margin, 0, Margin + 100, RowHeight));
    searchField->Attach (observer);
    searchField->Show ();

    // ...and the arrow half, ABUTTING it — not over it. Three arrangements were
    // tried before this one and each failed on the same DG rule:
    //
    //   * a hand-drawn border on a cell beside the field could not be aligned to the
    //     field's frame, because item rects are logical units and the frame is
    //     snapped in device pixels: at 150% scaling no integer rect lands on it;
    //   * a full-row cell UNDER a frameless field had DG fill its whole rect before
    //     every update, painting over the text — the field went grey and typing
    //     showed nothing, because DG does not clip siblings and will not repaint one
    //     because another drew.
    //
    // So NEITHER side draws a frame by hand: this cell asks DG for one and takes
    // exactly the field's top and bottom. Two native frames, drawn by the same code
    // on the same rows, cannot disagree about where a pixel is — and where they meet
    // is the divider a combo draws between its text and its arrow. The cell's own
    // paint is interior only: the hover wash and the chevron.
    //
    // StaticFrame, which is DG's FLAT border. ClientFrame is the other candidate and
    // was tried first; it is the sunken 3D client edge, which read as a well next to
    // a flat field rather than as the other half of it.
    comboFrame = std::make_unique<DG::UserItem> (panel, DG::Rect (Margin, 0, Margin + 100, RowHeight),
                                                 DG::UserItem::Normal, DG::UserItem::StaticFrame);
    comboFrame->SetBackgroundColor (FieldBackground ());
    // Hover feedback needs the pointer's comings and goings, and DG does not send
    // them to a user item unless it is asked to track the mouse.
    comboFrame->EnableMouseMoveEvent ();
    comboFrame->Attach (observer);
    comboFrame->Show ();
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
    if (open != wasOpen)
        RedrawCombo ();

    const bool rebuilt = ApplyFilter (selectionChanged);
    // OPENING counts as a change even when the rows did not move — a keystroke that
    // ranks the same set (a trailing space, a character that narrows nothing) still
    // has to make the list appear, and only the caller can lay it out.
    return rebuilt || open != wasOpen;
}

bool CommandListPanel::HandleUserItemMouseDown (const DG::UserItemMouseDownEvent& ev)
{
    if (!comboFrame || ev.GetSource () != comboFrame.get ())
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
    RedrawCombo ();
    return true;
}

// The cell's paint — INTERIOR ONLY. The frame around it is DG's, drawn with the
// same platform code as the field's next door, which is the whole reason this is a
// framed user item rather than a border drawn by hand.
bool CommandListPanel::HandleUserItemUpdate (const DG::UserItemUpdateEvent& ev)
{
    if (!comboFrame || ev.GetSource () != comboFrame.get ())
        return false;

    NewDisplay::UserItemUpdateNativeContext context (ev);

    // The CLIENT size: what is inside DG's frame. Painting to the item's full size
    // would put the fill under the frame it just drew.
    const float width = (float) comboFrame->GetClientWidth ();
    const float height = (float) comboFrame->GetClientHeight ();

    const Gfx::Color background = hovered ? HoverBackground () : FieldBackground ();
    context.FillRect (0.0f, 0.0f, width, height, background.GetRed (), background.GetGreen (),
                      background.GetBlue ());

    // ASK THE ITEM WHAT A PIXEL IS. Everything here is in the item's own logical
    // units, which the display scales — 1.0 is one device pixel at 100%, one and a
    // half at 150%, two at 200%. A stroke asked for in logical units therefore comes
    // back soft on a scaled display. The factor is read live rather than assumed,
    // because it is per-MONITOR: dragging the palette to a second screen changes it,
    // and so does the machine this is built for.
    const float scale = (float) GS::Max (0.1, comboFrame->GetResolutionFactor ());
    const float hairline = 1.0f / scale; // one DEVICE pixel, whatever the scaling

    // A chevron, centred, pointing THE WAY THE LIST WILL MOVE: down to open, up to
    // collapse. Stroked rather than filled — a solid triangle reads as a button's
    // marker, a chevron as the field's own affordance, which is what a native combo
    // draws. The chevron itself stays in logical units: it is an ICON, so it grows
    // with the display the way the field's text does, and only its stroke is pinned
    // to the device grid.
    const float cx = width / 2.0f;
    const float cy = height / 2.0f;
    const float tip = open ? -ChevronHeight / 2.0f : ChevronHeight / 2.0f;
    context.SetForeColor (DG::ColorCatalog::GetColor (DG::ColorId::ControlTextColor));
    context.SetLineWidth (GS::Max (hairline, ChevronStroke));
    context.MoveTo (cx - ChevronHalfWidth, cy - tip);
    context.LineTo (cx, cy + tip);
    context.LineTo (cx + ChevronHalfWidth, cy - tip);
    return true;
}

// The display's scaling changed under the palette — a second monitor, or the
// system setting moved. The cell's paint is derived from the factor, so it has to
// be taken again; DG resizes the item for us, but the pixels are ours.
bool CommandListPanel::HandleResolutionChanged (const DG::Item* item)
{
    if (!comboFrame || item != comboFrame.get ())
        return false;

    RedrawCombo ();
    return true;
}

// The pointer arrived or left. The cell IS the arrow now — it no longer spans the
// field — so there is nothing to ask about the position. Only the cell repaints: the
// rest of the palette has not changed, and repainting a band because a cursor
// crossed it flickers.
bool CommandListPanel::HandleUserItemHover (const DG::Item* item, bool inside)
{
    if (!comboFrame || item != comboFrame.get ())
        return false;

    if (hovered == inside)
        return true; // ours, but nothing moved

    hovered = inside;
    RedrawCombo ();
    return true;
}

void CommandListPanel::CloseList ()
{
    open = false;
    AdoptSelection ();
    RedrawCombo ();
}

// The chevron says which way the list is about to move, so every change of `open`
// has to reach it — including the one typing makes, which never touches the cell.
void CommandListPanel::RedrawCombo ()
{
    if (comboFrame)
        comboFrame->Redraw ();
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

    // Field and arrow ABUT on the same top and bottom. Both frames are DG's, so the
    // pair reads as one box without either of them measuring the other.
    const short arrowLeft = (short) (right - ArrowWidth);
    searchField->SetRect (DG::Rect (left, y, arrowLeft, (short) (y + RowHeight)));
    searchField->Show ();
    comboFrame->SetRect (DG::Rect (arrowLeft, y, right, (short) (y + RowHeight)));
    comboFrame->Show ();

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
