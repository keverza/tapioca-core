#include "ArchViz/EditLayers.hpp"
#include <algorithm>
#include <cmath>

// Where the view puts its labels: screen boxes that do not overlap, along a direction or
// beside an anchor (EditLayers.hpp PlaceLabel, PlaceAlong).
namespace geomsrv::archviz::editlayers {

bool Overlaps (const Box& a, const Box& b, float margin)
{
    // Separating axes: the two boxes' own, along and across each.
    const float acx = (a.x0 + a.x1) * 0.5f, acy = (a.y0 + a.y1) * 0.5f, bcx = (b.x0 + b.x1) * 0.5f,
                bcy = (b.y0 + b.y1) * 0.5f;
    const float ahw = (a.x1 - a.x0) * 0.5f, ahh = (a.y1 - a.y0) * 0.5f, bhw = (b.x1 - b.x0) * 0.5f,
                bhh = (b.y1 - b.y0) * 0.5f;
    const float axes[4][2] = { { a.ux, a.uy }, { -a.uy, a.ux }, { b.ux, b.uy }, { -b.uy, b.ux } };
    for (const auto& k : axes) {
        const float ra = ahw * std::abs (a.ux * k[0] + a.uy * k[1]) + ahh * std::abs (-a.uy * k[0] + a.ux * k[1]);
        const float rb = bhw * std::abs (b.ux * k[0] + b.uy * k[1]) + bhh * std::abs (-b.uy * k[0] + b.ux * k[1]);
        if (std::abs ((bcx - acx) * k[0] + (bcy - acy) * k[1]) >= ra + rb + margin)
            return false;
    }
    return true;
}

void Upright (float& dx, float& dy)
{
    const float d = std::hypot (dx, dy);
    if (!(d > 1.0e-6f)) {
        dx = 1.0f, dy = 0.0f;
        return;
    }
    dx /= d, dy /= d;
    if (dx < -1.0e-3f || (std::abs (dx) <= 1.0e-3f && dy > 0.0f)) // screen y runs down
        dx = -dx, dy = -dy;
}

Box PlaceLabel (float ax, float ay, float w, float h, float dx, float dy, float away, std::vector<Box>& taken,
                bool* found)
{
    return PlaceAlong (ax, ay, w, h, 1.0f, 0.0f, dx, dy, away, taken, found);
}

Box PlaceAlong (float ax, float ay, float w, float h, float ux, float uy, float dx, float dy, float away,
                std::vector<Box>& taken, bool* found)
{
    const auto at = [&] (float distance) {
        const float cx = ax + dx * distance, cy = ay + dy * distance;
        return Box { cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f, ux, uy };
    };
    const float step = std::max (6.0f, h * 0.75f);
    Box chosen = at (away);
    bool placed = false;
    for (int k = 0; k < 16 && !placed; ++k)
        for (const float side : { 1.0f, -1.0f }) {
            const Box b = at (side * (away + step * float (k)));
            bool free = true;
            for (const Box& t : taken)
                free = free && !Overlaps (b, t);
            if (free) {
                chosen = b;
                placed = true;
                break;
            }
        }
    if (found != nullptr)
        *found = placed;
    if (placed || found == nullptr)
        taken.push_back (chosen);
    return chosen;
}

} // namespace geomsrv::archviz::editlayers
