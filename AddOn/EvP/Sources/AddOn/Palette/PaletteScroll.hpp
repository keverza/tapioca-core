#ifndef GEOMETRYSERVER_PALETTE_PALETTESCROLL_HPP
#define GEOMETRYSERVER_PALETTE_PALETTESCROLL_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "DGModule.hpp"

#include <memory>

namespace evp {

// F4 — the palette's lower band scrolls under a fixed header.
//
// THERE IS NO SCROLLING CONTAINER IN DG. No item clips its children and there is
// no scroll view to parent items into (DGPanel.hpp / DGDialog.hpp — Panel and
// Palette have no scroll API at all). So this is VIRTUAL scrolling: the shell lays
// its column out in "virtual" coordinates, exactly as it did before this existed,
// and every item goes on the panel through this object, which
//
//   * subtracts the current offset from the rect, and
//   * takes the item OFF the panel when the shifted rect is not fully inside the
//     viewport. That is not cosmetic: a half-scrolled row would draw straight over
//     the fixed header, because it is a SIBLING of it, not a child. Short items
//     therefore appear and disappear whole (`Place`), while the two list boxes —
//     tall, and scrollable in their own right — are clipped to the viewport instead
//     (`PlaceClamped`), which is what keeps a scroll of the outer column smooth
//     rather than making a 150px list blink out in one step.
//
// Offset 0 (content shorter than the viewport) reproduces the old layout item for
// item, and hides the bar.
//
// Nothing here calls the shell: the shell drives it (Begin -> Place… -> End) and
// asks it what moved. The scroll bar is attached to the shell, which is the sole
// registered DG observer, like every other item a sub-object builds.
class PaletteScroll {
public:
    PaletteScroll (const DG::Panel& panel, DG::ScrollBarObserver& observer);

    // Builds the bar. Called from the shell's constructor BODY, after
    // BeginEventProcessing, so DG item creation stays in one place.
    void Create ();

    // --- the layout protocol, once per Layout(), in this order ---------------
    // The scrolled band occupies [viewportTop, viewportBottom) in DIALOG
    // coordinates, and virtual coordinates start at viewportTop — so an unscrolled
    // panel places everything exactly where it used to.
    void Begin (short viewportTop, short viewportBottom);
    // `contentBottom` is the virtual y the column ended at. Clamps the offset and
    // sizes the bar. Returns true when the clamp MOVED the offset, which means the
    // column was just placed against a stale one and has to be placed again.
    bool End (short contentBottom);

    // Is a virtual band fully on screen? The test `Place` applies, exposed because
    // a sub-object sometimes has to know before it commits to a row.
    bool IsVisible (short virtualTop, short virtualBottom) const;

    // Put `item` at `virtualRect`: offset it, and show or hide it. Null-tolerant so
    // a caller with a unique_ptr member does not need its own guard.
    void Place (DG::Item* item, const DG::Rect& virtualRect) const;
    // As Place, but the rect is CLIPPED to the viewport instead of the item being
    // hidden — for an item that carries its own scrollbar and stays usable at any
    // height. Hidden only once less than MinClampedHeight of it would be left.
    void PlaceClamped (DG::Item* item, const DG::Rect& virtualRect) const;

    short Offset () const { return offset; }
    // A DG event reports a dialog-relative y; splitter maths works in the virtual
    // coordinates Layout placed things in, so it has to be converted back.
    short ToVirtual (short dialogY) const { return (short) (dialogY + offset); }

    bool IsSource (const DG::Item* item) const;
    // The bar was dragged — adopt its value. True when the offset moved.
    bool FollowBar ();
    // The wheel turned over the panel. True when the offset moved.
    bool Wheel (short trackValue);

    // Below this, a clamped item is not worth showing — it would be a sliver.
    static constexpr short MinClampedHeight = 48;

private:
    short MaxOffset () const;
    short ClampOffset (short value) const;

    const DG::Panel&               panel;
    DG::ScrollBarObserver&         observer;
    std::unique_ptr<DG::ScrollBar> bar;

    short offset        = 0;
    short viewTop       = 0;
    short viewBottom    = 0;
    short contentHeight = 0;
};

}   // namespace evp

#endif
