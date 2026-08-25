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
    // it is always relevant.
    // FRAMELESS: the box around it is drawn by the cell below, so a frame here would
    // be a second outline inside the first.
    searchField = std::make_unique<DG::TextEdit> (panel, DG::Rect (Margin, 0, Margin + 100, RowHeight), 0,
                                                  DG::TextEdit::NoFrame);
    searchField->Attach (observer);
    searchField->Show ();

    // ...and the cell that draws the BOX around it. It spans the whole row and sits
    // UNDER the field, which is the arrangement DG's z-order actually gives us: a
    // text edit paints and takes its clicks above a user item, whatever order they
    // were created in. The first attempt had these the other way round and the field
    // wiped the chevron off the screen every time it repainted.
    //
    // So the drawn cell owns the outline, the divider and the chevron; the field is
    // frameless and sits inside it. Nothing has to line up with anything: the box and
    // the arrow are one paint, in one coordinate space.
    comboFrame = std::make_unique<DG::UserItem> (panel, DG::Rect (Margin, 0, Margin + 100, RowHeight),
                                                 DG::UserItem::Normal, DG::UserItem::NoFrame);
    // NO BACKGROUND COLOUR. A user item with one has its whole rect filled with it
    // before its update runs — and this item's rect covers the field, so that fill
    // painted over the text every time anything asked the cell to repaint. (That is
    // the bug where the command was invisible until the field had focus, and where
    // typing produced nothing on screen.) The paint below covers every pixel the
    // cell owns, and deliberately not one pixel more.
    comboFrame->ResetBackgroundColor ();
    // Hover feedback needs the pointer's comings and goings, and DG does not send
    // them to a user item unless it is asked to track the mouse. Only the arrow strip
    // is uncovered, so only the arrow strip can raise them — which is exactly the
    // region that should light up.
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

    // The cell is the WHOLE row, so a press on it is not necessarily a press on the
    // arrow: the border it draws around the field belongs to it too. Only the strip
    // toggles — clicking the box's edge should do what clicking a combo's edge does,
    // which is nothing.
    if (!InArrowStrip (ev.GetMouseOffset ()))
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

// The combo's paint: the box, the divider, the arrow strip and the chevron, all in
// ONE item's coordinate space — which is the whole point of drawing it this way.
// Two earlier attempts put a border on a cell BESIDE the field and tried to line it
// up with the field's native frame; that cannot be done from logical units on a
// scaled display, and does not have to be.
//
// The text area is deliberately NOT filled. The field is a separate item painting on
// top of this one, and DG does not clip siblings — anything drawn under it would
// erase the text until the field happened to repaint.
bool CommandListPanel::HandleUserItemUpdate (const DG::UserItemUpdateEvent& ev)
{
    if (!comboFrame || ev.GetSource () != comboFrame.get ())
        return false;

    NewDisplay::UserItemUpdateNativeContext context (ev);

    const float width = (float) comboFrame->GetWidth ();
    const float height = (float) comboFrame->GetHeight ();

    // ASK THE ITEM WHAT A PIXEL IS. Everything here is in the item's own logical
    // units, which the display scales — 1.0 is one device pixel at 100%, one and a
    // half at 150%, two at 200%. A line asked for in logical units therefore comes
    // back soft and off-centre on a scaled display. The factor is read live rather
    // than assumed, because it is per-MONITOR: dragging the palette to a second
    // screen changes it, and so does the machine this is built for.
    const float scale = (float) GS::Max (0.1, comboFrame->GetResolutionFactor ());
    const float hairline = 1.0f / scale; // one DEVICE pixel, whatever the scaling
    const float arrowLeft = width - (float) ArrowWidth;

    // Everything the box encloses EXCEPT the field's own rect: the inset ring around
    // it, and the arrow strip. The field paints its own background and its text, and
    // this item must not put a single pixel over that — see ResetBackgroundColor in
    // Create for what happens when it does.
    const Gfx::Color field = FieldBackground ();
    const float inset = (float) FrameInset;
    context.FillRect (0.0f, 0.0f, width, inset, field.GetRed (), field.GetGreen (), field.GetBlue ());
    context.FillRect (0.0f, height - inset, width, height, field.GetRed (), field.GetGreen (), field.GetBlue ());
    context.FillRect (0.0f, 0.0f, inset, height, field.GetRed (), field.GetGreen (), field.GetBlue ());

    // The arrow strip, which is the half of the row this item really owns.
    const Gfx::Color background = hovered ? HoverBackground () : field;
    context.FillRect (arrowLeft, hairline, width - hairline, height - hairline, background.GetRed (),
                      background.GetGreen (), background.GetBlue ());

    // The box, on the device pixel's CENTRE (half a hairline in): a line on the
    // boundary is half outside the item and comes back thin and pale.
    context.SetLineWidth (hairline);
    context.SetForeColor (FieldBorder ());
    context.FrameRect (hairline / 2.0f, hairline / 2.0f, width - hairline / 2.0f, height - hairline / 2.0f);

    // ...and the divider between the two halves, the one line a native combo draws
    // between its text and its arrow.
    context.MoveTo (arrowLeft, hairline);
    context.LineTo (arrowLeft, height - hairline);

    // A chevron, centred IN THE STRIP, pointing THE WAY THE LIST WILL MOVE: down to
    // open, up to collapse. Stroked rather than filled — a solid triangle reads as a
    // button's marker, a chevron as the field's own affordance, which is what a
    // native combo draws.
    const float cx = (arrowLeft + width) / 2.0f;
    const float cy = height / 2.0f;
    const float tip = open ? -ChevronHeight / 2.0f : ChevronHeight / 2.0f;
    // The chevron itself stays in logical units — it is an ICON, so it grows with
    // the display the way the field's text does. Only its stroke is pinned to the
    // device grid, and never below one device pixel.
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

// Is a point inside the cell over the arrow strip rather than over the border the
// cell draws around the field? One question, asked by both the press and the hover.
bool CommandListPanel::InArrowStrip (const DG::Point& at) const
{
    return comboFrame != nullptr && at.GetX () >= (short) (comboFrame->GetWidth () - ArrowWidth);
}

// The pointer moved, arrived or left. Only the strip highlights, so the answer is
// its position and not merely which item it is over — the cell spans the row, and a
// pointer crossing the box's border on its way into the text would otherwise flash
// the arrow. Only the cell repaints: the rest of the palette has not changed, and
// repainting a band because a cursor crossed it flickers.
bool CommandListPanel::HandleUserItemHover (const DG::Item* item, const DG::Point& at, bool inside)
{
    if (!comboFrame || item != comboFrame.get ())
        return false;

    const bool nowHovered = inside && InArrowStrip (at);
    if (hovered == nowHovered)
        return true; // ours, but nothing moved

    hovered = nowHovered;
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
    // WITHOUT ERASING. An erase clears the whole row, the field's text included, and
    // the field will not repaint just because a sibling did. The paint covers every
    // pixel this item is responsible for, so there is nothing an erase would add.
    if (comboFrame)
        comboFrame->Redraw (false);
    // ...and the field goes back on top afterwards regardless. DG does not clip
    // siblings, so the only guarantee that the text survives a repaint of the cell
    // under it is to repaint the text after it.
    if (searchField)
        searchField->Redraw ();
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

    // The drawn cell takes the whole row and the field sits INSIDE it, inset by the
    // border it draws, stopping where the arrow strip begins. The strip is therefore
    // the one part of the row no other item covers, which is what makes it clickable
    // and what makes its hover events arrive.
    comboFrame->SetRect (DG::Rect (left, y, right, (short) (y + RowHeight)));
    comboFrame->Show ();
    searchField->SetRect (DG::Rect ((short) (left + FrameInset), (short) (y + FrameInset),
                                    (short) (right - ArrowWidth), (short) (y + RowHeight - FrameInset)));
    searchField->Show ();

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
