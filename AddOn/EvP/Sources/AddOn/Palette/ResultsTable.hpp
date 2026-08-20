#ifndef GEOMETRYSERVER_PALETTE_RESULTSTABLE_HPP
#define GEOMETRYSERVER_PALETTE_RESULTSTABLE_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "DGModule.hpp"

#include <memory>

namespace evp {

class PaletteScroll;

// Feature D — the palette's one-shot results table (evp.ui.table).
//
// Built at runtime like the shell's Continue/Stop buttons, so it needs no .grc
// item (a .grc item would renumber every id after it — see ResourceIds.hpp).
// Hidden until a command shows results, and cleared when another command is
// selected, so a stale table never lingers under the wrong parameters.
//
// The shell owns the palette, the event subscription and the splitter BELOW this
// table; this object owns the list box, its cached content and its height. That
// is why Height/SetHeight/Top/IsVisible are public: the shell's splitter maths
// and placement persistence need them, so the table's height is part of its
// interface, not an internal detail.
class ResultsTable {
public:
    // The shell stays the sole registered DG observer: the box is attached to
    // `observer` (the palette), never to this object.
    ResultsTable (const DG::Panel& panel, DG::ListBoxObserver& observer);

    // Builds the list box. Called from the shell's constructor BODY rather than
    // from here, so item creation keeps happening exactly where it did before —
    // after BeginEventProcessing, alongside the other runtime-built items.
    void Create ();

    // Replace whatever was there before. The caller re-lays out and redraws:
    // this runs from a Posted dispatcher call, outside the DG event flow.
    void Show (const GS::Array<GS::UniString>& headers, const GS::Array<GS::UniString>& rowJsons);

    // A selectable, plain-text result surface. It shares the result band and its
    // splitter with the table, but deliberately uses no column geometry. Commands
    // whose result is a report rather than a grid use this through evp.ui.text().
    void ShowText (const GS::UniString& text);

    // Hide the table and forget its content.
    void Clear ();

    // Position the table in the band starting at `top`, and return the height it
    // used — 0 when there is nothing to show, so it costs the layout no space.
    // `clip` is the shell's virtual scroll (F4): `top` is a VIRTUAL y, and the box
    // reaches the panel through clip, which offsets and clips it.
    short PlaceAt (short top, short left, short right, const PaletteScroll& clip);

    bool  IsVisible () const { return visible; }
    short Height () const { return height; }
    void  SetHeight (short value) { height = value; }
    // Where PlaceAt last put the table, so a splitter drag can turn a dialog-
    // relative y back into a height.
    short Top () const { return top; }

    // How small the table may be dragged before it stops shrinking.
    static constexpr short MinHeight = 60;

    // Event routing, for the shell's single ListBoxSelectionChanged handler.
    bool          IsSource (const DG::Item* item) const {
        return (box != nullptr && item == box.get ()) ||
               (textBox != nullptr && item == textBox.get ());
    }
    short         SelectedRow () const;
    // The element GUID a row points at, or empty. Rows are 1-based, as DG counts.
    GS::UniString GuidAt (short listRow) const;

    // A single click on a row selects that row's element in Archicad. Runs on the
    // main thread (it is a DG event), so it drives the SetSelection native command
    // directly — no gate hop. A row without a GUID, or a GUID Archicad no longer
    // knows, is a quiet no-op: clicking a plain data row must not clear the
    // model's selection.
    void SelectRowElement (short listRow) const;

private:
    // A DG ListBox's scroll type is fixed at construction, and DG_LT_HSCROLL keeps a
    // horizontal scrollbar on screen even when the columns fit. So the table is
    // REBUILT with the right scroll type whenever it crosses the fit/scroll boundary:
    // VScroll-only when it is wide enough (no stray horizontal bar), HVScroll when the
    // columns no longer fit and horizontal scrolling is actually useful. The last
    // shown data is cached so the rebuilt box can be repopulated without another call.
    void Build (bool withHScroll);
    void Populate ();
    bool NeedsHScroll (short innerWidth) const;
    void EnsureScrollType (short innerWidth);
    // Recompute the column boundaries for the given inner width — the panel, and
    // thus the table, is resizable.
    void LayoutColumns (short innerWidth);

    const DG::Panel&     panel;
    DG::ListBoxObserver& observer;

    std::unique_ptr<DG::SingleSelListBox> box;
    std::unique_ptr<DG::MultiLineEdit>     textBox;
    bool                                  visible     = false;
    bool                                  showingText = false;
    short                                 columnCount = 0;
    bool                                  hScroll     = true;   // current box has HScroll

    // The last data Show was given, kept so a scroll-type rebuild can refill a
    // freshly-rebuilt box without the caller calling again.
    GS::Array<GS::UniString> headers;
    GS::Array<GS::UniString> rows;      // one JSON string per row
    GS::Array<short>         preferredColumnWidths;

    // Parallel to the table's rows (0-based): the element GUID a row points at, or
    // empty. A single click on a row with a GUID selects that element in Archicad —
    // so a results table doubles as a navigator back to the model.
    GS::Array<GS::UniString> rowGuids;

    short height = 140;   // resizable — the shell's splitter drives it
    short top    = 0;
};

}   // namespace evp

#endif
