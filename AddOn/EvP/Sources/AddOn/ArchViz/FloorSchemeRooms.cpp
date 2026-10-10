#include "ArchViz/FloorSchemeDetail.hpp"

// T6: rooms of one flat, drawn in its slot frame (facade at v1). Three layouts, from the
// user's sketches (2026-10-10):
//   straight  rooms side by side on the facade, the living room above the hall; hall, bath,
//             WC and storage on the corridor side, the hall where the door meets circulation;
//   corner    (3 rooms and more) the living room in the corner, a bedroom on each facade
//             beside it, more bedrooms along the long facade; hall and WC behind them;
//   through   (sectional flats with two facades) living room on one facade, bedrooms on the
//             other, services between; the hall at the landing.
// Every living room, bedroom and alcove gets a window on its facade; a 1.5R keeps 1.6 m of
// wall between its living-room and alcove windows (checked).
namespace geomsrv::archviz::floorscheme::detail {
namespace {
constexpr double kLivingPrefLarge = 4.0, kLivingMax = 4.5, kBedroomPref = 2.7, kBedroomMax = 3.3;
constexpr double kAlcoveMin = 1.5, kAlcovePref = 1.8, kAlcoveMax = 2.4, kHalf = kPartyWall / 2;
constexpr double kServiceDepth = 2.4, kMiddle = 2.2;

struct Ctx {
    Flat& flat;
    const Slot& slot;
    Box b;
    DoorAt door;
};
void Add (Ctx& c, Box b, RoomKind kind)
{
    if (b.W () < 0.05 || b.H () < 0.05)
        return;
    c.flat.roomList.push_back ({ ToRing (c.slot.frame, b), kind, b.W (), b.H () });
}
// A window on side `side` (0: v0, 1: u1, 2: v1, 3: u0) of room box `r`, `at` -1/0/+1 =
// toward the low end, centred, toward the high end of that side.
void Window (Ctx& c, Box r, int side, RoomKind kind, int at = 0)
{
    const bool alongU = side == 0 || side == 2;
    const double length = alongU ? r.W () : r.H ();
    const double width = (std::min) (length - 0.6, kind == RoomKind::Living   ? 1.8
                                                   : kind == RoomKind::Alcove ? 0.9
                                                                              : 1.2);
    if (width < 0.6)
        return;
    const double lo = alongU ? r.u0 : r.v0;
    const double start = at < 0 ? lo + 0.3 : at > 0 ? lo + length - 0.3 - width : lo + (length - width) / 2;
    const auto& f = c.slot.frame;
    Opening w;
    if (alongU) {
        const double v = side == 0 ? r.v0 : r.v1;
        w = { f.World (start, v), f.World (start + width, v), kind };
    }
    else {
        const double u = side == 3 ? r.u0 : r.u1;
        w = { f.World (u, start), f.World (u, start + width), kind };
    }
    c.flat.windows.push_back (w);
}
// Widths of facade rooms in `width` (walls between them already taken off): minimum, then
// preferred, then maximum, the living room first; anything left goes to the living room.
std::vector<double> Widths (const std::vector<RoomKind>& kinds, double width, int rooms)
{
    std::vector<double> w;
    for (auto k : kinds)
        w.push_back (k == RoomKind::Living   ? kLivingMin
                     : k == RoomKind::Alcove ? kAlcoveMin
                     : k == RoomKind::Hall   ? kHallMin
                                             : kBedroomMin);
    double slack = width;
    for (double x : w)
        slack -= x;
    auto grow = [&] (size_t i, double to) {
        const double add = std::clamp (to - w[i], 0.0, (std::max) (0.0, slack));
        w[i] += add, slack -= add;
    };
    const double livingPref = rooms <= 1 ? 3.5 : rooms == 2 ? 3.7 : kLivingPrefLarge;
    for (int phase = 0; phase < 2; ++phase)
        for (size_t i = 0; i < w.size (); ++i) {
            const auto k = kinds[i];
            if (k == RoomKind::Hall)
                continue;
            grow (i, k == RoomKind::Living   ? (phase ? kLivingMax : livingPref)
                     : k == RoomKind::Alcove ? (phase ? kAlcoveMax : kAlcovePref)
                                             : (phase ? kBedroomMax : kBedroomPref));
        }
    if (slack > 0 && !w.empty ()) {
        // Past the maximum the bedrooms share it with the living room.
        const double share = slack / static_cast<double> (w.size ());
        for (size_t i = 0; i < w.size (); ++i)
            if (kinds[i] != RoomKind::Hall)
                w[i] += share;
    }
    return w;
}
// Services along a strip [u0, u1] x [v0, v1]: the hall over the door, bath, WC (3 rooms and
// up) and storage around it. `doorU` is where the door meets the strip (NaN: either end free).
void Services (Ctx& c, double u0, double u1, double v0, double v1, int rooms, double doorU, bool hall = true)
{
    const double width = u1 - u0;
    std::vector<std::pair<RoomKind, double>> list { { RoomKind::Bath, 2.0 } };
    double left = width - (hall ? kHallMin + kInnerWall : 0) - 2.0;
    if (rooms >= 3 && left >= kWcMin + kInnerWall)
        list.push_back ({ RoomKind::Wc, kWcMin }), left -= kWcMin + kInnerWall;
    // 4 rooms and up have two bathrooms (user, 2026-10-10): the storage becomes the second
    // bath, or the WC grows into one.
    if (rooms >= 4 && left >= kBathMin + kInnerWall)
        list.push_back ({ RoomKind::Bath, (std::min) (2.0, left - kInnerWall) }),
            left -= list.back ().second + kInnerWall;
    else if (rooms >= 4 && list.back ().first == RoomKind::Wc && left >= kBathMin - kWcMin)
        list.back () = { RoomKind::Bath, kBathMin }, left -= kBathMin - kWcMin;
    if (left >= kStorageMin + kInnerWall)
        list.push_back ({ RoomKind::Storage, (std::min) (1.4, left - kInnerWall) }),
            left -= list.back ().second + kInnerWall;
    if (left < 0) {
        list.front ().second = (std::max) (kBathMin, 2.0 + left);
        left = (std::max) (0.0, left + 2.0 - list.front ().second);
    }
    const double hallW = hall ? kHallMin + (std::max) (0.0, left) : 0;
    // Number of services before the hall: the one that puts the hall over the door.
    size_t before = 0;
    if (hall && !std::isnan (doorU)) {
        double best = 1e18;
        for (size_t k = 0; k <= list.size (); ++k) {
            double x = u0;
            for (size_t i = 0; i < k; ++i)
                x += list[i].second + kInnerWall;
            const double miss = std::abs (x + hallW / 2 - doorU);
            if (miss < best)
                best = miss, before = k;
        }
    }
    double x = u0;
    auto place = [&] (RoomKind kind, double size) {
        Add (c, { x, v0, x + size, v1 }, kind);
        x += size + kInnerWall;
    };
    for (size_t i = 0; i < list.size (); ++i) {
        if (hall && i == before)
            place (RoomKind::Hall, hallW);
        place (list[i].first, list[i].second);
    }
    if (hall && before == list.size ())
        place (RoomKind::Hall, hallW);
}
double DoorCentre (const Ctx& c)
{
    if (c.door.side == 1)
        return c.b.u0;
    if (c.door.side == 2)
        return c.b.u1;
    const double lo = (std::max) (c.door.lo, c.b.u0), hi = (std::min) (c.door.hi, c.b.u1);
    return lo <= hi ? (lo + hi) / 2 : (c.b.u0 + c.b.u1) / 2;
}
void Straight (Ctx& c, int corner, bool hall = true)
{
    const Box b = c.b;
    const int whole = (std::max) (1, static_cast<int> (std::floor (c.flat.rooms + 1e-9)));
    const bool half = c.flat.rooms - whole >= 0.5 - 1e-9;
    const double depth = b.H ();
    const double rd = std::clamp (depth - kServiceDepth, (std::min) (depth, kLivingMin + 0.1), 6.0);
    const double back = depth - rd;
    // Living room toward the corner, else above the door; the alcove beside it.
    const double doorU = DoorCentre (c);
    const bool livingLow = corner == 0 || (corner < 0 && doorU - b.u0 <= b.u1 - doorU);
    std::vector<RoomKind> kinds { RoomKind::Living };
    if (half)
        kinds.push_back (RoomKind::Alcove);
    for (int i = 1; i < whole; ++i)
        kinds.push_back (RoomKind::Bedroom);
    const double inner = b.W () - kPartyWall - kInnerWall * (kinds.size () - 1);
    auto w = Widths (kinds, inner, whole);
    if (!livingLow) {
        std::reverse (w.begin (), w.end ());
        std::reverse (kinds.begin (), kinds.end ());
    }
    double u = b.u0 + kHalf;
    for (size_t i = 0; i < w.size (); ++i) {
        const Box r { u, b.v1 - rd, u + w[i], b.v1 };
        Add (c, r, kinds[i]);
        // A 1.5R pushes its two windows apart (living-room and alcove), else centred.
        int at = 0;
        if (half && (kinds[i] == RoomKind::Living || kinds[i] == RoomKind::Alcove)) {
            const bool first = (i == 0);
            at = first ? -1 : 1;
        }
        Window (c, r, 2, kinds[i], at);
        if (corner == 0 && i == 0 && kinds[i] == RoomKind::Living)
            Window (c, r, 3, kinds[i]);
        if (corner == 1 && i + 1 == w.size () && kinds[i] == RoomKind::Living)
            Window (c, r, 1, kinds[i]);
        u += w[i] + kInnerWall;
    }
    if (back < kHallMin)
        return;
    Services (c, b.u0 + kHalf, b.u1 - kHalf, b.v0, b.v0 + back - kInnerWall, whole, doorU, hall);
}
// Living room in the corner, a bedroom on the gable behind it, bedrooms along the facade;
// hall, bath and WC behind the facade bedrooms (sketch 1). False when it does not fit.
bool Corner (Ctx& c, int corner)
{
    const Box b = c.b;
    const int whole = static_cast<int> (std::floor (c.flat.rooms + 1e-9));
    const double depth = b.H ();
    // Leave the gable bedroom its preferred 2.7 m behind the living room.
    const double rd = std::clamp (depth - kBedroomPref - kInnerWall, kLivingMin + 0.1, 5.5);
    const double gableBed = depth - rd - kInnerWall;
    if (whole < 3 || gableBed < kBedroomMin)
        return false;
    const int facadeBeds = whole - 2;
    // Along the facade: living room, then the facade bedrooms; the strip behind the bedrooms
    // must hold a hall and a bath.
    std::vector<RoomKind> kinds { RoomKind::Living };
    for (int i = 0; i < facadeBeds; ++i)
        kinds.push_back (RoomKind::Bedroom);
    const double inner = b.W () - kPartyWall - kInnerWall * facadeBeds;
    if (inner < kLivingMin + facadeBeds * kBedroomMin)
        return false;
    auto w = Widths (kinds, inner, whole);
    double behind = inner - w[0] + kInnerWall * (facadeBeds - 1);
    if (behind < kHallMin + kInnerWall + kBathMin) {
        const double need = kHallMin + kInnerWall + kBathMin - behind;
        if (w[0] - need < kLivingMin)
            return false;
        w[0] -= need;
        w[1] += need;
        behind += need;
    }
    // t runs from the corner; the corner is at u0 (corner 0) or u1 (corner 1).
    auto box = [&] (double t0, double t1, double v0, double v1) {
        return corner == 0 ? Box { b.u0 + t0, v0, b.u0 + t1, v1 } : Box { b.u1 - t1, v0, b.u1 - t0, v1 };
    };
    // The hall is in the strip behind the facade bedrooms: that strip must meet the door side
    // where circulation does (a corridor that ends beside the corner room cannot be reached).
    if (c.door.side == 0) {
        const Box behind = box (kHalf + w[0] + kInnerWall, b.W () - kHalf, b.v0, b.v1);
        if ((std::min) (behind.u1, c.door.hi) - (std::max) (behind.u0, c.door.lo) < kHallMin)
            return false;
    }
    const int gable = corner == 0 ? 3 : 1;
    double t = kHalf;
    const Box living = box (t, t + w[0], b.v1 - rd, b.v1);
    Add (c, living, RoomKind::Living);
    Window (c, living, 2, RoomKind::Living);
    Window (c, living, gable, RoomKind::Living);
    const Box bed = box (t, t + w[0], b.v0, b.v0 + gableBed);
    Add (c, bed, RoomKind::Bedroom);
    Window (c, bed, gable, RoomKind::Bedroom);
    t += w[0] + kInnerWall;
    const double behindStart = t;
    for (size_t i = 1; i < w.size (); ++i) {
        const Box r = box (t, t + w[i], b.v1 - rd, b.v1);
        Add (c, r, RoomKind::Bedroom);
        Window (c, r, 2, RoomKind::Bedroom);
        t += w[i] + kInnerWall;
    }
    const Box strip = box (behindStart, b.W () - kHalf, b.v0, b.v0 + gableBed);
    Services (c, strip.u0, strip.u1, strip.v0, strip.v1, whole, DoorCentre (c));
    return true;
}
// Two facades (v0 and v1): living room on v0, bedrooms on v1, services between; the hall at
// the door on the u end (or the stair hall `extra` already is the hall). False when too shallow.
bool Through (Ctx& c, bool hallOutside)
{
    const Box b = c.b;
    const double depth = b.H ();
    if (!c.slot.through || c.door.side == 0 || depth < 8.0)
        return false;
    const int whole = (std::max) (1, static_cast<int> (std::floor (c.flat.rooms + 1e-9)));
    const bool half = c.flat.rooms - whole >= 0.5 - 1e-9;
    const double width = b.W () - kPartyWall;
    const double low = std::clamp ((depth - kMiddle) * 0.55, kLivingMin + 0.1, 5.0);
    double high = depth - low - kMiddle - 2 * kInnerWall;
    if (high < kBedroomMin)
        return false;
    const bool doorLow = c.door.side == 1;
    // The services strip reaches up to the door (the stair landing) when the bedroom row
    // above it keeps its minimum depth; otherwise the hall takes the end of that row.
    const double reach = (std::max) (c.door.lo, b.v0) + 1.0;
    if (!hallOutside && reach > b.v1 - high - kInnerWall)
        high = (std::max) (kBedroomMin, (std::min) (high, b.v1 - reach - kInnerWall));
    const double doorV = (std::max) (c.door.lo, b.v0) + 0.5;
    const bool hallHigh = !hallOutside && doorV > b.v1 - high;
    // A door wholly within the living-room row (the corridor ends beside it): the hall is there.
    const bool hallLow = !hallOutside && !hallHigh && (std::min) (c.door.hi, b.v1) <= b.v0 + low + 1e-6;
    // v0 facade: living room (and a bedroom from 3 rooms when wide enough).
    std::vector<RoomKind> lowKinds { RoomKind::Living }, highKinds;
    int beds = whole - 1;
    if (beds >= 2 && width >= kLivingMin + kInnerWall + kBedroomMin + (hallLow ? kHallMin + kInnerWall : 0))
        lowKinds.push_back (RoomKind::Bedroom), --beds;
    if (half)
        highKinds.push_back (RoomKind::Alcove);
    for (int i = 0; i < beds; ++i)
        highKinds.push_back (RoomKind::Bedroom);
    if (hallHigh)
        doorLow ? (void) highKinds.insert (highKinds.begin (), RoomKind::Hall) : highKinds.push_back (RoomKind::Hall);
    if (hallLow)
        doorLow ? (void) lowKinds.insert (lowKinds.begin (), RoomKind::Hall) : lowKinds.push_back (RoomKind::Hall);
    auto strip = [&] (std::vector<RoomKind> kinds, double v0, double v1, int side) {
        if (kinds.empty ())
            return true;
        const double inner = width - kInnerWall * (kinds.size () - 1);
        double need = 0;
        for (auto k : kinds)
            need += k == RoomKind::Living   ? kLivingMin
                    : k == RoomKind::Hall   ? kHallMin
                    : k == RoomKind::Alcove ? kAlcoveMin
                                            : kBedroomMin;
        if (inner < need - 1e-6)
            return false;
        const auto w = Widths (kinds, inner, whole);
        double u = b.u0 + kHalf;
        for (size_t i = 0; i < w.size (); ++i) {
            const Box r { u, v0, u + w[i], v1 };
            Add (c, r, kinds[i]);
            if (kinds[i] != RoomKind::Hall)
                Window (c, r, side, kinds[i]);
            u += w[i] + kInnerWall;
        }
        return true;
    };
    const size_t mark = c.flat.roomList.size ();
    const size_t windows = c.flat.windows.size ();
    if (!strip (lowKinds, b.v0, b.v0 + low, 0) || !strip (highKinds, b.v1 - high, b.v1, 2)) {
        c.flat.roomList.resize (mark);
        c.flat.windows.resize (windows);
        return false;
    }
    const double m0 = b.v0 + low + kInnerWall, m1 = b.v1 - high - kInnerWall;
    Services (c, b.u0 + kHalf, b.u1 - kHalf, m0, m1, whole, doorLow ? b.u0 : b.u1,
              !hallOutside && !hallHigh && !hallLow);
    return true;
}
} // namespace

void LayRooms (Flat& flat, const Slot& slot, Box b, int corner, DoorAt door)
{
    Ctx c { flat, slot, b, door };
    flat.roomList.clear ();
    flat.windows.clear ();
    const bool hallOutside = !slot.extra.Empty ();
    if (!Through (c, hallOutside) && !(corner >= 0 && Corner (c, corner)))
        Straight (c, corner, !hallOutside);
    // The entrance: where the hall meets the door side.
    for (const auto& r : flat.roomList) {
        if (r.kind != RoomKind::Hall)
            continue;
        const auto q0 = slot.frame.Local (r.shape[0]), q2 = slot.frame.Local (r.shape[2]);
        const Box h { (std::min) (q0.x, q2.x), (std::min) (q0.y, q2.y), (std::max) (q0.x, q2.x),
                      (std::max) (q0.y, q2.y) };
        if (door.side == 0) {
            const double lo = (std::max) (h.u0, door.lo), hi = (std::min) (h.u1, door.hi);
            const double mid = lo <= hi ? (lo + hi) / 2 : (h.u0 + h.u1) / 2;
            flat.door = { slot.frame.World (mid - 0.45, b.v0), slot.frame.World (mid + 0.45, b.v0), RoomKind::Hall };
        }
        else {
            const double u = door.side == 1 ? b.u0 : b.u1;
            const double lo = (std::max) (h.v0, door.lo), hi = (std::min) (h.v1, door.hi);
            const double mid = lo <= hi ? (lo + hi) / 2 : (h.v0 + h.v1) / 2;
            flat.door = { slot.frame.World (u, mid - 0.45), slot.frame.World (u, mid + 0.45), RoomKind::Hall };
        }
        break;
    }
}
} // namespace geomsrv::archviz::floorscheme::detail
