#include "APIEnvir.h"
#include "ACAPinc.h"

#include "Palette/PaletteScroll.hpp"
#include "Palette/PaletteMetrics.hpp"   // Margin — the gutter the bar lives in

using namespace evp::palette;

namespace {

// The bar sits INSIDE the panel's right margin rather than taking width off the
// content. That is deliberate: a bar that reserved width would change the content
// width the moment it appeared, which reflows every wrapped description line and
// every column — the exact flicker F4 is supposed to remove. Margin is 14, so a
// 10px bar with a 2px inset leaves a 2px gap to the content's right edge.
constexpr short BarWidth = 10;
constexpr short BarInset = 2;

// One wheel notch. Roughly two parameter rows: small enough that the column moves
// rather than jumps, large enough that a long panel does not need twenty turns.
constexpr short WheelStep = 40;

// A raw Windows wheel delta is 120 per notch; some paths hand DG an already
// normalised +/-1. Anything at or above this is treated as the raw form.
constexpr short RawWheelDelta = 120;

}   // namespace

namespace evp {

PaletteScroll::PaletteScroll (const DG::Panel& panel, DG::ScrollBarObserver& observer) :
    panel    (panel),
    observer (observer)
{
}

void PaletteScroll::Create ()
{
    // Proportional so the thumb states how much of the column is on screen, and
    // NonFocusable so tabbing through the parameters never lands on it. Built at
    // runtime like the Continue button and the results table — a .grc item would
    // renumber every id after it (see ResourceIds.hpp). Hidden until End() finds
    // there is something to scroll.
    bar = std::make_unique<DG::ScrollBar> (
        panel, DG::Rect (0, 0, BarWidth, 100),
        DG::ScrollBar::Proportional, DG::ScrollBar::NonFocusable);
    bar->Attach (observer);
    bar->Hide ();
}

void PaletteScroll::Begin (short viewportTop, short viewportBottom)
{
    viewTop    = viewportTop;
    viewBottom = (short) GS::Max (viewportTop, viewportBottom);
}

bool PaletteScroll::End (short contentBottom)
{
    const short viewport = (short) (viewBottom - viewTop);
    contentHeight = (short) GS::Max ((short) 0, (short) (contentBottom - viewTop));

    const short wanted = ClampOffset (offset);
    const bool  moved  = (wanted != offset);
    offset = wanted;

    if (bar == nullptr)
        return moved;

    // Content fits — no bar, no offset. The panel then looks exactly as it did
    // before F4 existed, which is the point. A panel dragged so short that the band
    // has no usable viewport left gets no bar either: there is nowhere to draw one.
    if (MaxOffset () <= 0 || viewport < MinClampedHeight) {
        if (bar->IsVisible ())
            bar->Hide ();
        return moved;
    }

    const short right = panel.GetWidth ();
    const DG::Rect rect ((short) (right - BarInset - BarWidth), viewTop,
                         (short) (right - BarInset), viewBottom);
    if (bar->GetRect () != rect)
        bar->SetRect (rect);

    // Windows convention, which is what DG wraps: min..max spans the whole content
    // and the page size is the visible part, so the reachable values are exactly
    // 0..(content - viewport) and the thumb draws in proportion.
    bar->SetMin (0);
    bar->SetMax (contentHeight);
    bar->SetPageSize (viewport);
    if (bar->GetValue () != offset)
        bar->SetValue (offset);
    if (!bar->IsVisible ())
        bar->Show ();

    return moved;
}

bool PaletteScroll::IsVisible (short virtualTop, short virtualBottom) const
{
    const short top    = (short) (virtualTop - offset);
    const short bottom = (short) (virtualBottom - offset);
    return top >= viewTop && bottom <= viewBottom;
}

// Show/Hide and SetRect are guarded on the value they would write. DG invalidates on
// every one of them, so re-asserting a rect an item already has is a repaint of that
// item for nothing — and this runs for every item on the panel, on every wheel notch.
void PaletteScroll::Place (DG::Item* item, const DG::Rect& virtualRect) const
{
    if (item == nullptr)
        return;

    if (!IsVisible (virtualRect.GetTop (), virtualRect.GetBottom ())) {
        if (item->IsVisible ())
            item->Hide ();
        return;
    }

    const DG::Rect rect (virtualRect.GetLeft (), (short) (virtualRect.GetTop () - offset),
                         virtualRect.GetRight (), (short) (virtualRect.GetBottom () - offset));
    if (item->GetRect () != rect)
        item->SetRect (rect);
    if (!item->IsVisible ())
        item->Show ();
}

void PaletteScroll::PlaceClamped (DG::Item* item, const DG::Rect& virtualRect) const
{
    if (item == nullptr)
        return;

    const short top    = (short) GS::Max ((short) (virtualRect.GetTop () - offset), viewTop);
    const short bottom = (short) GS::Min ((short) (virtualRect.GetBottom () - offset), viewBottom);

    if ((short) (bottom - top) < MinClampedHeight) {
        if (item->IsVisible ())
            item->Hide ();
        return;
    }

    const DG::Rect rect (virtualRect.GetLeft (), top, virtualRect.GetRight (), bottom);
    if (item->GetRect () != rect)
        item->SetRect (rect);
    if (!item->IsVisible ())
        item->Show ();
}

bool PaletteScroll::IsSource (const DG::Item* item) const
{
    return bar != nullptr && item == bar.get ();
}

bool PaletteScroll::FollowBar ()
{
    if (bar == nullptr)
        return false;
    const short wanted = ClampOffset ((short) bar->GetValue ());
    if (wanted == offset)
        return false;
    offset = wanted;
    return true;
}

// DG reports the wheel as a track value whose unit the DevKit headers do not state
// (DGPanel.hpp — PanelWheelTrackEvent::GetYTrackValue, no comment). Both forms in
// circulation are handled, so one turn of the wheel is one step either way.
bool PaletteScroll::Wheel (short trackValue)
{
    if (trackValue == 0)
        return false;

    short notches = trackValue;
    if (notches >= RawWheelDelta || notches <= -RawWheelDelta)
        notches = (short) (notches / RawWheelDelta);
    if (notches > 3)
        notches = 3;                    // a flick must not throw the column away
    else if (notches < -3)
        notches = -3;

    // Wheel UP shows what is above, i.e. moves the column back towards its start.
    const short wanted = ClampOffset ((short) (offset - notches * WheelStep));
    if (wanted == offset)
        return false;
    offset = wanted;
    return true;
}

short PaletteScroll::MaxOffset () const
{
    return (short) GS::Max ((short) 0, (short) (contentHeight - (viewBottom - viewTop)));
}

short PaletteScroll::ClampOffset (short value) const
{
    return (short) GS::Max ((short) 0, GS::Min (value, MaxOffset ()));
}

}   // namespace evp
