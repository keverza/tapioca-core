#include "ArchViz/FloorSchemeEditDetail.hpp"
#include <algorithm>
#include <cmath>

// A flat's entrance moved (user, 2026-11-15: "allow to change entrance door location"). The door
// stays on the wall it is on -- the flat's wall to its corridor, landing or lobby -- and goes along
// it; the generator lays the flat's rooms again with the hall behind the door (Pins::Door).
namespace geomsrv::archviz::floorscheme::edit {
namespace {
using namespace shared;
using namespace detail;
constexpr double kHalf = 0.45;  // half a door
constexpr double kInside = 0.3; // the pin stands this far inside the flat from the door's wall

bool HasDoor (const Flat& f)
{
    return std::hypot (f.door.b.x - f.door.a.x, f.door.b.y - f.door.a.y) > 1e-6;
}
bool Circulation (const Scheme& s, Vec p)
{
    for (const auto& c : s.corridors)
        if (In (c.shape, p))
            return true;
    for (const auto& l : s.lobbies)
        if (In (l, p))
            return true;
    for (const auto& c : s.cores)
        if (In (c.shape, p))
            return true;
    return false;
}
} // namespace

std::optional<Vec> DoorOf (const Scheme& s, int flat)
{
    if (flat < 0 || flat >= static_cast<int> (s.flats.size ()) || !HasDoor (s.flats[flat]))
        return std::nullopt;
    const auto& door = s.flats[flat].door;
    return Vec { (door.a.x + door.b.x) / 2, (door.a.y + door.b.y) / 2 };
}

std::optional<Vec> DoorPlace (const Scheme& s, int flat, Vec to)
{
    const auto now = DoorOf (s, flat);
    if (!now)
        return std::nullopt;
    const Flat& f = s.flats[flat];
    const Vec along = Unit ({ f.door.b.x - f.door.a.x, f.door.b.y - f.door.a.y });
    std::optional<Vec> best;
    double near = 1e300;
    for (size_t k = 0; k < f.shape.size (); ++k) {
        const Vec a = f.shape[k], b = f.shape[(k + 1) % f.shape.size ()];
        const double length = std::hypot (b.x - a.x, b.y - a.y);
        if (length < 2 * kHalf)
            continue;
        const Vec dir { (b.x - a.x) / length, (b.y - a.y) / length };
        // The wall the door is on, and any stretch of the outline in line with it.
        if (std::abs (dir.x * along.y - dir.y * along.x) > 1e-3 || Distance (*now, a, b) > 0.1)
            continue;
        // Out of the flat on either side of the wall: its outline may run either way round.
        const Vec n { dir.y, -dir.x };
        const double want = Clip (Dot ({ to.x - a.x, to.y - a.y }, dir), kHalf, length - kHalf);
        // The nearest place to `to` where a whole door opens onto circulation.
        for (double off = 0; off <= length; off += 0.05)
            for (const double t : { want - off, want + off }) {
                if (t < kHalf - 1e-9 || t > length - kHalf + 1e-9)
                    continue;
                const auto open = [&] (double u) {
                    const Vec p { a.x + dir.x * u, a.y + dir.y * u };
                    return Circulation (s, { p.x + n.x * 0.25, p.y + n.y * 0.25 }) ||
                           Circulation (s, { p.x - n.x * 0.25, p.y - n.y * 0.25 });
                };
                if (!open (t - kHalf + 0.02) || !open (t) || !open (t + kHalf - 0.02))
                    continue;
                const Vec p { a.x + dir.x * t, a.y + dir.y * t };
                if (const double d = std::hypot (p.x - to.x, p.y - to.y); d < near)
                    near = d, best = p;
            }
    }
    return best;
}

Design MoveDoor (const Scheme& s, Design d, int flat, Vec to, std::string* why)
{
    if (flat < 0 || flat >= static_cast<int> (s.flats.size ()))
        return d;
    const Flat& f = s.flats[flat];
    const auto now = DoorOf (s, flat);
    if (!now)
        return Refuse (std::move (d), why,
                       f.manual ? "This flat's rooms are left to you: it has no entrance to move."
                                : "This flat has no entrance on a corridor, landing or lobby.");
    const auto place = DoorPlace (s, flat, to);
    if (!place)
        return Refuse (std::move (d), why, "No wall of this flat opens onto circulation by a door's width there.");
    if (std::hypot (place->x - now->x, place->y - now->y) < 0.05)
        return Refuse (std::move (d), why, "The entrance is as near there as its wall to the corridor allows.");
    // Into the flat from the wall: towards the side its middle is on.
    const Vec along = Unit ({ f.door.b.x - f.door.a.x, f.door.b.y - f.door.a.y });
    Vec n { -along.y, along.x };
    const Vec mid = Within (f.shape);
    if (Dot ({ mid.x - place->x, mid.y - place->y }, n) < 0)
        n = { -n.x, -n.y };
    std::erase_if (d.pins.doors, [&] (const Pins::Door& p) { return In (f.shape, p.at); });
    d.pins.doors.push_back ({ { place->x + n.x * kInside, place->y + n.y * kInside } });
    return d;
}

Design ResetDoor (const Scheme& s, Design d, int flat)
{
    if (flat >= 0 && flat < static_cast<int> (s.flats.size ()))
        std::erase_if (d.pins.doors, [&] (const Pins::Door& p) { return In (s.flats[flat].shape, p.at); });
    return d;
}
} // namespace geomsrv::archviz::floorscheme::edit
