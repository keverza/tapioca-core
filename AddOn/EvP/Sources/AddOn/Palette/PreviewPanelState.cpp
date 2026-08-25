#include "Palette/PreviewPanelState.hpp"

#include <algorithm>

namespace evp::previewpanel {
namespace {

constexpr int ControlHeight = 22;
constexpr int Gap = 6;
constexpr int CanvasSlotHeight = 224;
constexpr int ExpandedHeight = CanvasSlotHeight + 3 * ControlHeight + 3 * Gap;

bool IsExternal (Host host)
{
    return host == Host::Overlay || host == Host::PopOut;
}

Rect ButtonRect (int left, int right, int index)
{
    const int width = (right - left - 3 * Gap) / 4;
    const int x = left + index * (width + Gap);
    return { x, 0, index == 3 ? right : x + width, ControlHeight };
}

} // namespace

bool HostState::ExternalStartingOrActive () const
{
    return IsExternal (target) && (transition || current == target);
}

bool HostState::CanvasCollapsed () const
{
    return IsExternal (target) || IsExternal (current);
}

void CanvasInputState::SetAvailable (bool value)
{
    available = value;
    if (!available) {
        pointerInside = false;
        heldButtons = 0;
    }
}

void CanvasInputState::SetPointerInside (bool inside)
{
    pointerInside = available && inside;
    if (!pointerInside)
        heldButtons = 0;
}

bool CanvasInputState::Press (unsigned buttonMask)
{
    if (!CanRoutePointer () || buttonMask == 0)
        return false;
    heldButtons |= buttonMask;
    return true;
}

bool CanvasInputState::Release (unsigned buttonMask)
{
    if (!available || buttonMask == 0)
        return false;
    heldButtons &= ~buttonMask;
    return true;
}

bool CanvasInputState::ReleaseAll ()
{
    const bool hadInput = pointerInside || heldButtons != 0;
    pointerInside = false;
    heldButtons = 0;
    return hadInput;
}

bool CanvasInputState::CanRoutePointer () const
{
    return available && pointerInside;
}

bool CanvasInputState::IsDragging () const
{
    return heldButtons != 0;
}

Layout BuildLayout (int left, int right, int bottom, bool enabled, bool canvasCollapsed)
{
    Layout layout;
    if (!enabled) {
        layout.height = ControlHeight;
        layout.enableControl = { left, bottom - ControlHeight, right, bottom };
        return layout;
    }

    layout.height = canvasCollapsed ? 2 * ControlHeight + Gap : ExpandedHeight;
    const int top = bottom - layout.height;
    layout.enableControl = { left, top, right, top + ControlHeight };

    int buttonTop = bottom - ControlHeight;
    layout.overlayButton = ButtonRect (left, right, 0);
    layout.popOutButton = ButtonRect (left, right, 1);
    layout.returnButton = ButtonRect (left, right, 2);
    layout.hideButton = ButtonRect (left, right, 3);
    for (Rect* rect : { &layout.overlayButton, &layout.popOutButton, &layout.returnButton, &layout.hideButton }) {
        rect->top += buttonTop;
        rect->bottom += buttonTop;
    }

    if (canvasCollapsed)
        return layout;

    const int canvasTop = top + ControlHeight + Gap;
    layout.canvas = { left, canvasTop, right, canvasTop + CanvasSlotHeight };

    const int footerTop = top + ControlHeight + Gap + CanvasSlotHeight + Gap;
    const int width = right - left;
    const int labelWidth = std::min (140, std::max (56, width / 3));
    layout.nodeSelector = { left, footerTop, left + width / 2, footerTop + ControlHeight };
    layout.scrubber = { left + width / 2 + Gap, footerTop, right - labelWidth - Gap, footerTop + ControlHeight };
    layout.frameLabel = { right - labelWidth, footerTop, right, footerTop + ControlHeight };
    layout.showCanvas = true;
    layout.showPreviewControls = true;
    return layout;
}

} // namespace evp::previewpanel
