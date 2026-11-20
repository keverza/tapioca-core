// ArchViz/OverlayHitMap -- see the header.

#include "ArchViz/OverlayHitMap.hpp"

namespace geomsrv {
namespace archviz {
namespace overlayinput {

Owner CursorOwner (Owner remembered, uint32_t physicalButtons)
{
    return remembered == Owner::Host && physicalButtons == 0 ? Owner::None : remembered;
}

Cursor CursorFor (const HitMap& map, bool overRegion, bool hudGesture)
{
    const Cursor cursor = map.cursor == uint8_t (Cursor::Arrow) && map.hand ? Cursor::Hand : Cursor (map.cursor);
    return map.locked && !overRegion && !hudGesture && cursor == Cursor::Hand ? Cursor::Arrow : cursor;
}

bool IsNavigationCursorResource (uint32_t resource)
{
    switch (resource) {
        case 322:
        case 323:
        case 324:
        case 325:
        case 326:
        case 327:
        case 328:
        case 329:
        case 339:
        case 340:
        case 393:
        case 922:
        case 927:
            return true;
        default:
            return false;
    }
}

int HitMap::Hit (float x, float y, float width, float height) const
{
    for (size_t i = regions.size (); i-- > 0;) {
        const Region& region = regions[i];
        const float scale = region.logical ? dpiScale : 1.0f;
        const float ax = region.fraction[0] * width, ay = region.fraction[1] * height;
        if (x >= ax + region.rect[0] * scale && x < ax + region.rect[2] * scale && y >= ay + region.rect[1] * scale &&
            y < ay + region.rect[3] * scale)
            return int (i);
    }
    return -1;
}

Route Router::Verdict (const Event& event, bool overHud, bool overPanel, Owner& owner)
{
    // A gesture whose release went unseen ends at the first move with nothing held.
    if (owner != Owner::None && event.kind == EventKind::Move && event.held == 0)
        owner = Owner::None;
    switch (event.kind) {
        case EventKind::Press:
            if (event.button == Button::Middle) {
                // Over a panel every button is the panel's (the user, 2026-11-20); elsewhere the
                // middle button navigates, a locked view's included.
                if (owner == Owner::None)
                    owner = overPanel ? Owner::Hud : Owner::Host;
                return owner == Owner::Hud && overPanel ? Route::Take : Route::Pass;
            }
            if (owner == Owner::None)
                owner = !overHud ? Owner::Host : Owner::Hud;
            return owner == Owner::Hud ? Route::Take : Route::Pass;
        case EventKind::Release: {
            if (event.button == Button::Middle) {
                const Route route = owner == Owner::Hud && overPanel ? Route::Take : Route::Pass;
                if (event.held == 0)
                    owner = Owner::None;
                return route;
            }
            // One whose press was never seen is Archicad's: it may be waiting for it.
            const Route route = owner == Owner::Hud ? Route::Take : Route::Pass;
            if ((event.held & ~Bit (Button::Middle)) == 0)
                owner = Owner::None;
            return route;
        }
        case EventKind::Move:
            // The middle button navigates through any gesture, a HUD's included -- but not over a
            // panel, where it is the panel's.
            if ((event.held & Bit (Button::Middle)) != 0)
                return owner == Owner::Hud && overPanel ? Route::Take : Route::Pass;
            if (owner == Owner::Hud)
                return Route::Take;
            if (owner == Owner::Host)
                return Route::Pass;
            return overHud ? Route::Take : Route::Pass;
        case EventKind::Wheel:
            // Over a panel the wheel scrolls it, whether its page or a list inside it scrolls
            // (the user, 2026-11-20: the wheel zoomed the view under the panel); elsewhere the
            // view zooms -- and while a gesture is Archicad's, the wheel is its too.
            return overPanel && event.wheel != 0.0f && owner != Owner::Host ? Route::Take : Route::Pass;
    }
    return Route::Pass;
}

Route Router::Decide (const Event& event, bool overHud, bool overPanel)
{
    return Verdict (event, overHud, overPanel, owner_);
}

Route Router::Preview (const Event& event, bool overHud, bool overPanel) const
{
    Owner owner = owner_;
    return Verdict (event, overHud, overPanel, owner);
}

} // namespace overlayinput
} // namespace archviz
} // namespace geomsrv
