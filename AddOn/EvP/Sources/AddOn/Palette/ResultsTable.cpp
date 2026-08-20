#include "APIEnvir.h"
#include "ACAPinc.h"

#include "Palette/ResultsTable.hpp"
#include "Palette/PaletteMetrics.hpp"
#include "Palette/PaletteScroll.hpp"   // F4 — the box reaches the panel through it
#include "AddOnCommands.hpp"       // geomsrv::ExecuteNativeCommand — SetSelection
#include "Python/PathUtils.hpp"    // AppendTextLine / ScanLogPath

#include "ObjectState.hpp"
#include "ObjectStateJSONConversion.hpp"

using namespace evp::palette;   // Margin — the seed rect only; PlaceAt gets the real one

namespace {

constexpr short ResultsHeaderHeight   = 16;
constexpr short ResultsScrollbarWidth = 18;   // reserve so columns don't run under it
constexpr short ResultsMinColWidth    = 48;
constexpr short ResultsMaxColWidth    = 640;
constexpr short ResultsCellPadding    = 18;
constexpr short ResultsAverageCharWidth = 7;

short PreferredColumnWidth (const GS::UniString& text)
{
    const Int32 width = (Int32) text.GetLength () * ResultsAverageCharWidth + ResultsCellPadding;
    return (short) GS::Min ((Int32) ResultsMaxColWidth,
                            GS::Max ((Int32) ResultsMinColWidth, width));
}

}   // namespace

namespace evp {

ResultsTable::ResultsTable (const DG::Panel& panel, DG::ListBoxObserver& observer) :
    panel    (panel),
    observer (observer)
{
}

void ResultsTable::Create ()
{
    // Starts with a horizontal scrollbar; Build swaps the scroll type live as the
    // panel is resized. Hidden until a command shows results.
    Build (true);
    textBox = std::make_unique<DG::MultiLineEdit> (
        panel, DG::Rect (Margin, 0, Margin + 100, height), DG::MultiLineEdit::HVScroll,
        DG::EditControl::Frame, DG::EditControl::Update, DG::EditControl::ReadOnly);
    textBox->Hide ();
}

// (Re)create the list box with the chosen scroll type. Separate from Create because
// the scroll type is fixed per box and must be swapped by rebuilding — VScroll-only
// when the columns fit (no stray horizontal bar), HVScroll when they do not. The new
// box starts hidden; PlaceAt sizes and shows it.
void ResultsTable::Build (bool withHScroll)
{
    box = std::make_unique<DG::SingleSelListBox> (
        panel, DG::Rect (Margin, 0, Margin + 100, height),
        withHScroll ? DG::ListBox::HVScroll : DG::ListBox::VScroll, DG::ListBox::PartialItems,
        DG::ListBox::Header, ResultsHeaderHeight, DG::ListBox::Frame);
    box->Attach (observer);   // single click -> the shell selects the row's element
    box->Hide ();
    hScroll = withHScroll;
}

void ResultsTable::Clear ()
{
    if (box == nullptr)
        return;
    box->DeleteItem (DG::ListBox::AllItems);
    box->Hide ();
    if (textBox != nullptr)
        textBox->Hide ();
    visible     = false;
    showingText = false;
    columnCount = 0;
    rowGuids.Clear ();
    headers.Clear ();
    rows.Clear ();
}

// Fill the current box from the cached headers/rows. Used by Show and again after a
// scroll-type rebuild, so a box that was just recreated comes back with the same
// content — no second call from Python.
void ResultsTable::Populate ()
{
    if (box == nullptr)
        return;

    // DG caches tab text while a visible list box is modified. The guard must
    // include deletion, headers and cells: guarding only the insertion pass left
    // old text associated with the wrong tab after a refresh.
    box->DisableDraw ();
    box->DeleteItem (DG::ListBox::AllItems);
    rowGuids.Clear ();

    // At least one column, so an empty-header call still yields a usable table.
    columnCount = (short) GS::Max ((USize) 1, headers.GetSize ());
    preferredColumnWidths.Clear ();
    for (short c = 0; c < columnCount; ++c) {
        const GS::UniString header = ((UIndex) c < headers.GetSize ()) ? headers[c] : GS::UniString ();
        preferredColumnWidths.Push (PreferredColumnWidth (header));
    }
    box->SetTabFieldCount (columnCount);
    // The header is its own layer with its own item count — without matching it to
    // the tab fields the labels clip and the extra columns come up blank.
    box->SetHeaderItemCount (columnCount);

    // A SYNCHRON header sizes itself and ignores SetHeaderItemSize, so LayoutColumns
    // could not move the boundaries: the first column ate the header and the second
    // was clipped to a few letters. Both DevKit examples that size a header by hand
    // (DG_Test's LayerDialog and BuildingMaterialDialog) turn synchron off first,
    // before any SetHeaderItemSize — so do that here, once the items exist.
    box->SetHeaderSynchronState (false);

    for (short c = 0; c < columnCount; ++c) {
        // Positions/sizes are placeholders — LayoutColumns sets the real ones from
        // the live width. Left-justified, end-truncated (a value too wide gets an
        // ellipsis rather than spilling into the next column), header and data styled
        // alike so they read as one column.
        box->SetTabFieldProperties ((short) (c + 1), 0, 100,
                                    DG::ListBox::Left, DG::ListBox::EndTruncate);
        box->SetHeaderItemSizeableFlag ((short) (c + 1), true);
        box->SetHeaderItemStyle ((short) (c + 1), DG::ListBox::Left, DG::ListBox::EndTruncate);
        const GS::UniString header = ((UIndex) c < headers.GetSize ()) ? headers[c] : GS::UniString ();
        box->SetHeaderItemText ((short) (c + 1), header);
    }

    for (const GS::UniString& rowJson : rows) {
        GS::ObjectState os;
        if (JSON::ConvertToObjectState (rowJson, os) != NoError)
            continue;
        GS::Array<GS::UniString> cells;
        os.Get ("cells", cells);

        box->AppendItem ();
        const short row = box->GetItemCount ();
        for (short c = 0; c < columnCount; ++c) {
            const GS::UniString cell = ((UIndex) c < cells.GetSize ()) ? cells[c] : GS::UniString ();
            preferredColumnWidths[(UIndex) c] = GS::Max (preferredColumnWidths[(UIndex) c],
                                                          PreferredColumnWidth (cell));
            box->SetTabItemText (row, (short) (c + 1), cell);
        }

        // Optional per-row text colour, to mark outliers. evp.ui.table resolves a
        // name or tuple to [r,g,b] (0-255); an absent or malformed rgb leaves the
        // row at the theme's default colour.
        GS::Array<GS::Int32> rgb;
        if (os.Get ("rgb", rgb) && rgb.GetSize () == 3)
            box->SetItemColor (row, Gfx::Color ((unsigned char) rgb[0],
                                                (unsigned char) rgb[1],
                                                (unsigned char) rgb[2]));

        // Optional element GUID: a click on this row selects that element. Kept in a
        // parallel array (empty when absent) so the shell's selection handler can
        // look it up by row without reparsing.
        GS::UniString guid;
        os.Get ("guid", guid);
        rowGuids.Push (guid);
    }
    box->EnableDraw ();
}

// Columns split the table's inner width evenly UNTIL that would make them narrower
// than ResultsMinColWidth; past that they hold their width and the table scrolls
// horizontally instead. The header band is a separate layer from the tab fields —
// its item widths must be set to match, or the header labels drift and clip (the
// "Column 1" -> "Colum" glitch). Recomputed on every layout: the panel, and thus
// the table, is resizable.
void ResultsTable::LayoutColumns (short innerWidth)
{
    if (box == nullptr || columnCount < 1)
        return;
    const short usable = (short) GS::Max ((short) 40, (short) (innerWidth - ResultsScrollbarWidth));
    short preferredTotal = 0;
    for (short c = 0; c < columnCount; ++c)
        preferredTotal = (short) (preferredTotal + preferredColumnWidths[(UIndex) c]);
    const bool fit = preferredTotal <= usable;

    for (short c = 0; c < columnCount; ++c) {
        short beg = 0;
        for (short prior = 0; prior < c; ++prior)
            beg = (short) (beg + preferredColumnWidths[(UIndex) prior]);
        const short end = (fit && c == columnCount - 1) ? usable
                                                        : (short) (beg + preferredColumnWidths[(UIndex) c]);
        box->SetTabFieldBeginEndPosition ((short) (c + 1), beg, end);
        box->SetHeaderItemSize ((short) (c + 1), (short) (end - beg));
    }
}

// Would the columns overflow the visible width at this width? Mirrors the fit test
// in LayoutColumns — below the min column width, the table needs to scroll.
bool ResultsTable::NeedsHScroll (short innerWidth) const
{
    if (columnCount < 1)
        return false;
    const short usable = (short) GS::Max ((short) 40, (short) (innerWidth - ResultsScrollbarWidth));
    short preferredTotal = 0;
    for (short c = 0; c < columnCount; ++c)
        preferredTotal = (short) (preferredTotal + preferredColumnWidths[(UIndex) c]);
    return preferredTotal > usable;
}

// Rebuild the box with the right scroll type only when the fit/scroll state flips,
// so a wide panel drops the horizontal bar and a narrow one regains it. Cheap in the
// common case (no change -> early out); a rebuild refills from the cache.
void ResultsTable::EnsureScrollType (short innerWidth)
{
    const bool needH = NeedsHScroll (innerWidth);
    if (needH == hScroll)
        return;
    Build (needH);
    Populate ();
}

void ResultsTable::Show (const GS::Array<GS::UniString>& newHeaders,
                         const GS::Array<GS::UniString>& rowJsons)
{
    if (box == nullptr)
        return;

    if (textBox != nullptr)
        textBox->Hide ();

    // Cache first: a scroll-type rebuild (in PlaceAt) refills from these.
    headers = newHeaders;
    rows    = rowJsons;

    Populate ();
    showingText = false;
    visible = true;
}

void ResultsTable::ShowText (const GS::UniString& text)
{
    if (textBox == nullptr)
        return;

    if (box != nullptr)
        box->Hide ();
    textBox->SetText (text);
    showingText = true;
    visible = true;
}

// Below every parameter, and ONLY while a command has shown results — it takes no
// layout space otherwise. A .grc-free item keeps its last rect, so it must be
// hidden explicitly when unused.
short ResultsTable::PlaceAt (short bandTop, short left, short right, const PaletteScroll& clip)
{
    if (box == nullptr || textBox == nullptr)
        return 0;

    if (!visible) {
        if (box->IsVisible ())
            box->Hide ();
        if (textBox->IsVisible ())
            textBox->Hide ();
        return 0;
    }

    top = bandTop;
    if (showingText) {
        clip.PlaceClamped (textBox.get (), DG::Rect (left, bandTop, right, (short) (bandTop + height)));
        return height;
    }

    const short innerWidth = (short) (right - left);
    // Swap VScroll<->HVScroll if the fit/scroll state changed (may rebuild the box,
    // so it must happen before we take the box's rect below).
    EnsureScrollType (innerWidth);
    // CLAMPED, not hidden, when the column is scrolled across it (F4): the table has
    // its own scrollbar, so a shortened one is still a usable table — and the band
    // keeps its full height either way, so nothing below it moves.
    clip.PlaceClamped (box.get (), DG::Rect (left, bandTop, right, (short) (bandTop + height)));
    LayoutColumns (innerWidth);
    return height;
}

short ResultsTable::SelectedRow () const
{
    return box != nullptr ? box->GetSelectedItem () : (short) 0;
}

GS::UniString ResultsTable::GuidAt (short listRow) const
{
    if (listRow < 1 || (UIndex) listRow > rowGuids.GetSize ())
        return GS::UniString ();
    return rowGuids[(UIndex) (listRow - 1)];
}

// SetSelection replaces the selection (add=false) with just this element, and like
// every ACAPI_Selection_* call it is not undoable, so it needs no undo scope.
void ResultsTable::SelectRowElement (short listRow) const
{
    const GS::UniString guid = GuidAt (listRow);
    if (guid.IsEmpty ())
        return;

    GS::ObjectState elementId, element, params;
    elementId.Add ("guid", guid);
    element.Add ("elementId", elementId);
    GS::Array<GS::ObjectState> elements;
    elements.Push (element);
    params.Add ("elements", elements);
    params.Add ("add", false);   // replace: click one row, select one element

    const geomsrv::NativeCommandResult result =
        geomsrv::ExecuteNativeCommand (GS::String ("SetSelection"), params);
    if (!result.ok) {
        AppendTextLine (ScanLogPath (),
            GS::UniString::Printf ("results row select: SetSelection failed for %T: %T",
                                   guid.ToPrintf (), result.error.ToPrintf ()));
    }
}

}   // namespace evp
