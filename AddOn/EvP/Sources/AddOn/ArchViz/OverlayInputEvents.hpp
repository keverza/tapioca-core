#ifndef EVP_ARCHVIZ_OVERLAYINPUTEVENTS_HPP
#define EVP_ARCHVIZ_OVERLAYINPUTEVENTS_HPP

// Win32 message decoding only. Routing, cursor ownership and session state remain
// in OverlayInput.cpp; these helpers allocate nothing and call no ACAPI.
#include "ArchViz/OverlayHitMap.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

namespace geomsrv::archviz::overlayinput::events {
inline uint32_t Held (WPARAM keys)
{
    uint32_t held = 0;
    if ((keys & MK_LBUTTON) != 0)
        held |= Bit (Button::Left);
    if ((keys & MK_RBUTTON) != 0)
        held |= Bit (Button::Right);
    if ((keys & MK_MBUTTON) != 0)
        held |= Bit (Button::Middle);
    if ((keys & MK_XBUTTON1) != 0)
        held |= Bit (Button::X1);
    if ((keys & MK_XBUTTON2) != 0)
        held |= Bit (Button::X2);
    return held;
}

inline uint32_t PhysicalButtons ()
{
    uint32_t held = 0;
    const int keys[] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
    for (int button = 0; button < 5; ++button)
        if ((::GetAsyncKeyState (keys[button]) & 0x8000) != 0)
            held |= 1u << button;
    return held;
}

inline bool EventOf (const MSG& message, Event& event)
{
    event.held = Held (GET_KEYSTATE_WPARAM (message.wParam));
    const Button x = GET_XBUTTON_WPARAM (message.wParam) == XBUTTON2 ? Button::X2 : Button::X1;
    switch (message.message) {
        case WM_MOUSEMOVE:
            event.kind = EventKind::Move;
            return true;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            event.kind = EventKind::Press;
            event.button = Button::Left;
            return true;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK:
            event.kind = EventKind::Press;
            event.button = Button::Right;
            return true;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONDBLCLK:
            event.kind = EventKind::Press;
            event.button = Button::Middle;
            return true;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONDBLCLK:
            event.kind = EventKind::Press;
            event.button = x;
            return true;
        case WM_LBUTTONUP:
            event.kind = EventKind::Release;
            event.button = Button::Left;
            return true;
        case WM_RBUTTONUP:
            event.kind = EventKind::Release;
            event.button = Button::Right;
            return true;
        case WM_MBUTTONUP:
            event.kind = EventKind::Release;
            event.button = Button::Middle;
            return true;
        case WM_XBUTTONUP:
            event.kind = EventKind::Release;
            event.button = x;
            return true;
        case WM_MOUSEWHEEL:
            event.kind = EventKind::Wheel;
            event.wheel = float (GET_WHEEL_DELTA_WPARAM (message.wParam)) / float (WHEEL_DELTA);
            return true;
        case WM_MOUSEHWHEEL:
            event.kind = EventKind::Wheel;
            return true;
        default:
            return false;
    }
}
} // namespace geomsrv::archviz::overlayinput::events
#endif
