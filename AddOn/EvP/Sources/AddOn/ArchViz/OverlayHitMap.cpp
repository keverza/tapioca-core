// ArchViz/OverlayHitMap -- see the header.

#include "ArchViz/OverlayHitMap.hpp"

namespace geomsrv {
namespace archviz {
namespace overlayinput {

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

Route Router::Verdict (const Event& event, bool overHud, Owner& owner)
{
    // A gesture whose release went unseen ends at the first move with nothing held.
    if (owner != Owner::None && event.kind == EventKind::Move && event.held == 0)
        owner = Owner::None;
    switch (event.kind) {
        case EventKind::Press:
            if (owner == Owner::None)
                owner = event.button == Button::Middle || !overHud ? Owner::Host : Owner::Hud;
            return owner == Owner::Hud ? Route::Take : Route::Pass;
        case EventKind::Release: {
            // One whose press was never seen is Archicad's: it may be waiting for it.
            const Route route = owner == Owner::Hud ? Route::Take : Route::Pass;
            if (event.held == 0)
                owner = Owner::None;
            return route;
        }
        case EventKind::Move:
            if (owner == Owner::Hud)
                return Route::Take;
            if (owner == Owner::Host)
                return Route::Pass;
            return overHud ? Route::Take : Route::Pass;
        case EventKind::Wheel:
            return Route::Pass;
    }
    return Route::Pass;
}

Route Router::Decide (const Event& event, bool overHud)
{
    return Verdict (event, overHud, owner_);
}

Route Router::Preview (const Event& event, bool overHud) const
{
    Owner owner = owner_;
    return Verdict (event, overHud, owner);
}

} // namespace overlayinput
} // namespace archviz
} // namespace geomsrv
