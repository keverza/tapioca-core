#include "ArchViz/FloorSchemeEdit.hpp"
#include "ArchViz/FloorSchemeDetail.hpp"
#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace geomsrv::archviz::floorscheme::edit {
namespace {
using namespace detail;
// A flat no narrower than the smallest flat type's rooms (a 1.5R: living room and alcove).
const double kNarrowest = RoomFrontage (1.5).min;

void Say (std::string* why, const char* format, va_list args)
{
    if (why) {
        char text[256];
        std::vsnprintf (text, sizeof (text), format, args);
        *why = text;
    }
}
// `d` unchanged, and why it could not change in `why` (printf style).
Design Refuse (Design d, std::string* why, const char* format, ...)
{
    va_list args;
    va_start (args, format);
    Say (why, format, args);
    va_end (args);
    return d;
}
// An edit made, with a word on it in `why` (printf style).
void Remark (std::string* why, const char* format, ...)
{
    va_list args;
    va_start (args, format);
    Say (why, format, args);
    va_end (args);
}

bool In (const Ring& ring, Vec p)
{
    return ring.size () >= 3 && Inside ({ ToPath (ring) }, p);
}
double Distance (Vec p, Vec a, Vec b)
{
    const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    const double t = l2 > 0 ? std::clamp (((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.0, 1.0) : 0;
    return std::hypot (a.x + t * dx - p.x, a.y + t * dy - p.y);
}
// A band's flats as the user sees them, along the band's centre line: where each flat starts and
// ends, its room count (0: the generator's choice), and the band's pins written back from them.
struct Frozen {
    int band = -1;
    Vec origin, dir;                   // the centre line: origin + dir * u
    std::vector<double> lo, hi, rooms; // per flat, in order along dir
    std::vector<int> flats;            // scheme indices, in the same order
    Vec At (double u) const
    {
        return { origin.x + dir.x * u, origin.y + dir.y * u };
    }
    double U (Vec p) const
    {
        return Dot ({ p.x - origin.x, p.y - origin.y }, dir);
    }
};
std::optional<Frozen> Freeze (const Scheme& s, int band)
{
    if (band < 0 || band >= static_cast<int> (s.bands.size ()))
        return std::nullopt;
    const auto& b = s.bands[band];
    Frozen f;
    f.band = band;
    f.dir = Unit ({ b.b.x - b.a.x, b.b.y - b.a.y });
    f.origin = b.a;
    const cp::PathsD shape { ToPath (b.shape) };
    struct Item {
        double lo, hi;
        int flat;
    };
    std::vector<Item> items;
    for (size_t i = 0; i < s.flats.size (); ++i) {
        if (s.flats[i].band != band)
            continue;
        // The flat's extent inside its band: strips the mould added past the band do not count.
        double lo = 1e18, hi = -1e18;
        for (const auto& part : cp::Intersect ({ ToPath (s.flats[i].shape) }, shape, cp::FillRule::NonZero, kPrecision))
            for (const auto& q : part) {
                const double u = f.U ({ q.x, q.y });
                lo = (std::min) (lo, u), hi = (std::max) (hi, u);
            }
        if (lo < hi)
            items.push_back ({ lo, hi, static_cast<int> (i) });
    }
    if (items.empty ())
        return std::nullopt;
    std::sort (items.begin (), items.end (), [] (const Item& a, const Item& c) { return a.lo < c.lo; });
    for (size_t k = 0; k < items.size (); ++k) {
        // Neighbours meet at one wall: the midpoint of the hairline between their extents.
        const double lo = k == 0 ? items[k].lo : (items[k - 1].hi + items[k].lo) / 2;
        const double hi = k + 1 == items.size () ? items[k].hi : (items[k].hi + items[k + 1].lo) / 2;
        f.lo.push_back (lo), f.hi.push_back (hi);
        f.rooms.push_back (s.flats[items[k].flat].rooms);
        f.flats.push_back (items[k].flat);
    }
    return f;
}
// The band's pins from a frozen band: a wall at every boundary, one flat per span, its rooms.
Design Write (const Scheme& s, Design d, const Frozen& f)
{
    const Ring& band = s.bands[f.band].shape;
    auto outside = [&] (Vec p) { return !In (band, p); };
    std::erase_if (d.pins.walls, [&] (const Pins::Wall& w) { return !outside (w.at); });
    std::erase_if (d.pins.counts, [&] (const Pins::Count& c) { return !outside (c.at); });
    std::erase_if (d.pins.rooms, [&] (const Pins::Rooms& r) { return !outside (r.at); });
    for (size_t k = 0; k < f.lo.size (); ++k) {
        if (k > 0)
            d.pins.walls.push_back ({ f.At (f.lo[k]) });
        const Vec mid = f.At ((f.lo[k] + f.hi[k]) / 2);
        d.pins.counts.push_back ({ mid, 1 });
        if (f.rooms[k] > 0)
            d.pins.rooms.push_back ({ mid, f.rooms[k] });
    }
    return d;
}
// Every stair of the wing pinned where it stands, so editing one leaves the others in place.
Design PinStairs (const Scheme& s, Design d, int wing)
{
    if (wing < 0)
        return d;
    const Ring& rect = s.wings[wing].rect;
    std::erase_if (d.pins.cores, [&] (const Pins::Core& c) { return In (rect, c.centre); });
    for (const auto& core : s.cores)
        if (In (rect, core.centre))
            d.pins.cores.push_back ({ core.centre });
    return d;
}
// The wing's stairs pinned `count` to it, one in the middle of each of `count` equal stretches,
// and the one at `keep` (when given) in its stretch instead.
Design SpaceStairs (const Scheme& s, Design d, int wing, int count, const Vec* keep)
{
    const auto& w = s.wings[wing];
    std::erase_if (d.pins.cores, [&] (const Pins::Core& c) { return In (w.rect, c.centre); });
    const Vec dir = Unit ({ w.b.x - w.a.x, w.b.y - w.a.y });
    const int held = keep ? static_cast<int> (std::floor (Dot ({ keep->x - w.a.x, keep->y - w.a.y }, dir) /
                                                          (std::max) (1e-9, w.length) * count))
                          : -1;
    for (int k = 0; k < count; ++k) {
        if (keep && k == std::clamp (held, 0, count - 1)) {
            d.pins.cores.push_back ({ *keep });
            continue;
        }
        const double t = w.length * (k + 0.5) / count;
        // Beside the corridor on the side the stairs are now, else the centre line.
        Vec at { w.a.x + dir.x * t, w.a.y + dir.y * t };
        for (const auto& core : s.cores)
            if (In (w.rect, core.centre)) {
                const Vec n { -dir.y, dir.x };
                const double off = Dot ({ core.centre.x - w.a.x, core.centre.y - w.a.y }, n);
                at = { w.a.x + dir.x * t + n.x * off, w.a.y + dir.y * t + n.y * off };
                break;
            }
        d.pins.cores.push_back ({ at });
    }
    return d;
}
// A frozen band's spans laid out again from its first wall, `widths` in order.
void Relay (Frozen& f, const std::vector<double>& widths)
{
    double u = f.lo.empty () ? 0 : f.lo.front ();
    for (size_t k = 0; k < widths.size (); ++k) {
        f.lo[k] = u;
        u += widths[k];
        f.hi[k] = u;
    }
}
size_t Index (const Frozen& f, int flat)
{
    return static_cast<size_t> (std::find (f.flats.begin (), f.flats.end (), flat) - f.flats.begin ());
}
constexpr double kDoor = 1.3;
// Where a frozen band meets its circulation (corridor, stair, lobby), along it; lo > hi when it
// meets none.
std::pair<double, double> Reach (const Scheme& s, const Frozen& f)
{
    double dlo = 1e18, dhi = -1e18;
    const Ring& band = s.bands[f.band].shape;
    const auto reach = [&] (const Ring& r) {
        if (!Near (r, band, 0.2) || Touch (band, r, 0.1) < 0.5)
            return;
        for (const auto& p : r) {
            const double u = f.U (p);
            dlo = (std::min) (dlo, u), dhi = (std::max) (dhi, u);
        }
    };
    for (const auto& c : s.corridors)
        reach (c.shape);
    for (const auto& c : s.cores)
        reach (c.shape);
    for (const auto& l : s.lobbies)
        reach (l);
    return { dlo, dhi };
}
// Where a wall may split span `k` of a frozen band: both parts at least `least` wide and each
// reaching the band's circulation by a door's width, as the generator asks of every flat. Empty
// (lo > hi) when the span cannot split.
std::pair<double, double> SplitRange (const Scheme& s, const Frozen& f, size_t k, double least)
{
    const auto [dlo, dhi] = Reach (s, f);
    const double lo = f.lo[k], hi = f.hi[k];
    if (dlo > dhi) // no circulation found: widths alone
        return { lo + least, hi - least };
    return { (std::max) (lo + least, (std::max) (lo, dlo) + kDoor),
             (std::min) (hi - least, (std::min) (hi, dhi) - kDoor) };
}
size_t Nearest (const std::vector<Pins::Core>& pins, Vec p)
{
    size_t best = pins.size ();
    double near = 1e18;
    for (size_t i = 0; i < pins.size (); ++i) {
        const double dd = std::hypot (pins[i].centre.x - p.x, pins[i].centre.y - p.y);
        if (dd < near)
            near = dd, best = i;
    }
    return best;
}
// The unlocked flat that shares the longest wall with `flat`, of its own section first, and not one
// of `taken`; -1 when none does.
int Beside (const Scheme& s, const Design& d, int flat, const std::vector<int>& taken)
{
    const auto& gone = s.flats[flat];
    int best = -1;
    double longest = 0.5;
    bool same = false;
    for (size_t i = 0; i < s.flats.size (); ++i) {
        const int other = static_cast<int> (i);
        if (other == flat || Locked (s, d, other) || std::find (taken.begin (), taken.end (), other) != taken.end () ||
            !Near (gone.shape, s.flats[i].shape, 0.2))
            continue;
        const double shared = Touch (gone.shape, s.flats[i].shape, 0.1);
        const bool mine = s.flats[i].section == gone.section;
        if (shared > 0.5 && ((mine && !same) || (mine == same && shared > longest)))
            best = other, longest = shared, same = mine;
    }
    return best;
}
// A point well inside `r` (an L's vertex mean can lie outside it): of a grid over its bounds, the
// one farthest from its outline.
Vec Within (const Ring& r)
{
    constexpr int kGrid = 24;
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (const auto& p : r)
        x0 = (std::min) (x0, p.x), y0 = (std::min) (y0, p.y), x1 = (std::max) (x1, p.x), y1 = (std::max) (y1, p.y);
    Vec best = Centroid (r);
    double far = -1;
    for (int i = 0; i < kGrid; ++i)
        for (int j = 0; j < kGrid; ++j) {
            const Vec p { x0 + (x1 - x0) * (i + 0.5) / kGrid, y0 + (y1 - y0) * (j + 0.5) / kGrid };
            if (!In (r, p))
                continue;
            double near = 1e18;
            for (size_t k = 0; k < r.size (); ++k)
                near = (std::min) (near, Distance (p, r[k], r[(k + 1) % r.size ()]));
            if (near > far)
                far = near, best = p;
        }
    return best;
}
// The widths of a run of flats laid again with one more: the widest split in two, then every flat
// narrower than it may be widened to that, the others giving in proportion to what they have over
// theirs. A flat is at least `least`; the run's first and last at least `first` and `last` (to
// reach the corridor by a door). Empty when one more does not fit.
std::vector<double> OneMore (std::vector<double> w, double least, double first, double last)
{
    double length = 0;
    for (double x : w)
        length += x;
    const size_t n = w.size () + 1;
    const auto lowest = [&] (size_t k) {
        return (std::max) ({ least, k == 0 ? first : 0.0, k + 1 == n ? last : 0.0 });
    };
    double need = 0;
    for (size_t k = 0; k < n; ++k)
        need += lowest (k);
    if (w.empty () || need > length + 1e-9)
        return {};
    const auto widest = std::max_element (w.begin (), w.end ());
    const double half = *widest / 2;
    *widest = half;
    w.insert (widest + 1, half);
    double deficit = 0, spare = 0;
    for (size_t k = 0; k < n; ++k)
        (w[k] < lowest (k) ? deficit : spare) += std::abs (w[k] - lowest (k));
    if (deficit > 1e-9)
        for (size_t k = 0; k < n; ++k)
            w[k] = w[k] < lowest (k) ? lowest (k) : w[k] - (w[k] - lowest (k)) * deficit / spare;
    return w;
}
} // namespace

int FlatAt (const Scheme& s, Vec p)
{
    for (size_t i = 0; i < s.flats.size (); ++i)
        if (In (s.flats[i].shape, p))
            return static_cast<int> (i);
    return -1;
}
int CoreAt (const Scheme& s, Vec p)
{
    for (size_t i = 0; i < s.cores.size (); ++i)
        if (In (s.cores[i].shape, p))
            return static_cast<int> (i);
    return -1;
}
int WingAt (const Scheme& s, Vec p)
{
    for (size_t i = 0; i < s.wings.size (); ++i)
        if (In (s.wings[i].rect, p))
            return static_cast<int> (i);
    return -1;
}
std::optional<Wall> WallAt (const Scheme& s, Vec p, double tolerance)
{
    std::optional<Wall> best;
    double near = tolerance;
    for (size_t i = 0; i < s.flats.size (); ++i) {
        const auto& f = s.flats[i];
        if (f.band < 0)
            continue;
        const Vec axis = Unit (f.axis);
        for (size_t k = 0; k < f.shape.size (); ++k) {
            const Vec a = f.shape[k], b = f.shape[(k + 1) % f.shape.size ()];
            const Vec d = Unit ({ b.x - a.x, b.y - a.y });
            if (std::abs (Dot (d, axis)) > 0.1 || std::hypot (b.x - a.x, b.y - a.y) < 0.5)
                continue; // only walls across the band
            const double dd = Distance (p, a, b);
            if (dd >= near)
                continue;
            // The flat on the other side of this wall, in the same band.
            const Vec mid { (a.x + b.x) / 2, (a.y + b.y) / 2 };
            const Vec mine = Centroid (f.shape);
            const double side = Dot ({ mid.x - mine.x, mid.y - mine.y }, axis) > 0 ? 1.0 : -1.0;
            const Vec beyond { mid.x + axis.x * 0.15 * side, mid.y + axis.y * 0.15 * side };
            for (size_t j = 0; j < s.flats.size (); ++j)
                if (j != i && s.flats[j].band == f.band && In (s.flats[j].shape, beyond)) {
                    near = dd;
                    best = side > 0 ? Wall { static_cast<int> (i), static_cast<int> (j), a, b, axis }
                                    : Wall { static_cast<int> (j), static_cast<int> (i), a, b, axis };
                }
        }
    }
    return best;
}
std::optional<End> EndAt (const Scheme& s, Vec p, double tolerance)
{
    std::optional<End> best;
    double near = tolerance;
    for (size_t i = 0; i < s.corridors.size (); ++i) {
        const auto& axis = s.corridors[i].axis;
        if (axis.size () < 2)
            continue;
        for (bool last : { false, true }) {
            const Vec at = last ? axis.back () : axis.front ();
            const double dd = std::hypot (at.x - p.x, at.y - p.y);
            if (dd < near)
                near = dd, best = End { static_cast<int> (i), last, at };
        }
    }
    return best;
}

Design MoveStair (const Scheme& s, Design d, int core, Vec to, std::string* why)
{
    if (core < 0 || core >= static_cast<int> (s.cores.size ()))
        return Refuse (std::move (d), why, "There is no stair there to move.");
    const Vec from = s.cores[core].centre;
    d = PinStairs (s, std::move (d), WingAt (s, from));
    const size_t i = Nearest (d.pins.cores, from);
    if (i < d.pins.cores.size ())
        d.pins.cores[i].centre = to;
    else
        d.pins.cores.push_back ({ to });
    return d;
}
Design AddStair (const Scheme& s, Design d, Vec at, std::string* why)
{
    const int wing = WingAt (s, at);
    if (wing < 0)
        return Refuse (std::move (d), why, "A stair goes inside one of the floor's wings: drop it on the floor.");
    int count = 1;
    for (const auto& core : s.cores)
        count += In (s.wings[wing].rect, core.centre);
    return SpaceStairs (s, std::move (d), wing, count, &at);
}
Design RemoveStair (const Scheme& s, Design d, int core, std::string* why)
{
    if (core < 0 || core >= static_cast<int> (s.cores.size ()))
        return Refuse (std::move (d), why, "There is no stair there to remove.");
    const int wing = WingAt (s, s.cores[core].centre);
    if (wing < 0)
        return Refuse (std::move (d), why, "This stair stands in no wing of the floor: it cannot be removed here.");
    int count = -1;
    for (const auto& c : s.cores)
        count += In (s.wings[wing].rect, c.centre);
    if (count < 1)
        return Refuse (std::move (d), why, "This is its wing's only stair: every flat must reach a stair.");
    return SpaceStairs (s, std::move (d), wing, count, nullptr);
}
Design MoveWall (const Scheme& s, Design d, const Wall& wall, Vec to, std::string* why)
{
    if (wall.low < 0 || wall.high < 0)
        return Refuse (std::move (d), why, "Only a wall between two flats of one band moves.");
    auto f = Freeze (s, s.flats[wall.low].band);
    if (!f)
        return Refuse (std::move (d), why, "Only a wall between two flats of one band moves.");
    const auto it = std::find (f->flats.begin (), f->flats.end (), wall.high);
    if (it == f->flats.begin () || it == f->flats.end ())
        return Refuse (std::move (d), why, "Only a wall between two flats of one band moves.");
    const size_t k = static_cast<size_t> (it - f->flats.begin ());
    const double lo = f->lo[k - 1] + kNarrowest, hi = f->hi[k] - kNarrowest;
    if (lo > hi)
        return Refuse (std::move (d), why, "Both flats are at their narrowest (%.1f m): the wall cannot move.",
                       kNarrowest);
    const double u = std::clamp (f->U (to), lo, hi);
    f->hi[k - 1] = f->lo[k] = u;
    return Write (s, std::move (d), *f);
}
Design SplitFlat (const Scheme& s, Design d, int flat, Vec at, std::string* why)
{
    if (flat < 0 || flat >= static_cast<int> (s.flats.size ()))
        return Refuse (std::move (d), why, "There is no flat there to split.");
    if (Locked (s, d, flat))
        return Refuse (std::move (d), why, "The flat is locked: unlock it to split it.");
    auto f = Freeze (s, s.flats[flat].band);
    if (!f)
        return Refuse (std::move (d), why,
                       "Only a flat in a row of flats splits; this one ends a corridor or a section.");
    const auto it = std::find (f->flats.begin (), f->flats.end (), flat);
    if (it == f->flats.end ())
        return Refuse (std::move (d), why,
                       "Only a flat in a row of flats splits; this one ends a corridor or a section.");
    const size_t k = static_cast<size_t> (it - f->flats.begin ());
    const auto [lo, hi] = SplitRange (s, *f, k, kNarrowest + 0.2);
    if (lo > hi)
        return Refuse (std::move (d), why, "Too narrow for two flats of %.1f m that each have a door on the corridor.",
                       kNarrowest + 0.2);
    const double u = std::clamp (f->U (at), lo, hi);
    f->lo.insert (f->lo.begin () + static_cast<std::ptrdiff_t> (k) + 1, u);
    f->hi.insert (f->hi.begin () + static_cast<std::ptrdiff_t> (k), u);
    f->rooms[k] = 0;
    f->rooms.insert (f->rooms.begin () + static_cast<std::ptrdiff_t> (k), 0);
    f->flats.insert (f->flats.begin () + static_cast<std::ptrdiff_t> (k), -1);
    return Write (s, std::move (d), *f);
}
Design RemoveFlat (const Scheme& s, Design d, int flat, std::string* why, double most)
{
    if (flat < 0 || flat >= static_cast<int> (s.flats.size ()))
        return Refuse (std::move (d), why, "There is no flat there to remove.");
    if (Locked (s, d, flat))
        return Refuse (std::move (d), why, "The flat is locked: unlock it to remove it.");
    auto f = Freeze (s, s.flats[flat].band);
    const auto it = f ? std::find (f->flats.begin (), f->flats.end (), flat) : std::vector<int>::iterator {};
    if (!f || f->flats.size () < 2 || it == f->flats.end ()) {
        // No row to close over it (a corridor's end, a section's flat, its row's only flat): it
        // joins the unlocked flat it shares the longest wall with, of its own section first (user,
        // 2026-10-14: "I was not able to delete some units").
        const auto& gone = s.flats[flat];
        const int best = Beside (s, d, flat, {});
        if (best < 0)
            return Refuse (std::move (d), why,
                           "No unlocked flat shares a wall with this one to take its floor: it stays.");
        // Over the largest flat it is still made, as asked, and says so: a corridor's end flat and
        // the row beside it are often sized to the largest already.
        const double joined = gone.net + s.flats[best].net;
        if (most > 0 && joined > most + 0.5)
            Remark (why, "Joined to the flat beside it: %.0f m2, over the largest flat (%.0f m2). Split it or undo.",
                    joined, most);
        std::erase_if (d.pins.rooms, [&] (const Pins::Rooms& r) { return In (gone.shape, r.at); });
        d.pins.joins.push_back ({ Within (gone.shape), Within (s.flats[best].shape) });
        return d;
    }
    const size_t k = static_cast<size_t> (it - f->flats.begin ());
    // It joins its narrower neighbour: the wall between them goes.
    const bool before =
        k + 1 == f->flats.size () || (k > 0 && f->hi[k - 1] - f->lo[k - 1] <= f->hi[k + 1] - f->lo[k + 1]);
    const size_t keep = before ? k - 1 : k; // the first of the two merged
    f->hi[keep] = f->hi[keep + 1];
    f->rooms[keep] = 0;
    f->lo.erase (f->lo.begin () + static_cast<std::ptrdiff_t> (keep) + 1);
    f->hi.erase (f->hi.begin () + static_cast<std::ptrdiff_t> (keep) + 1);
    f->rooms.erase (f->rooms.begin () + static_cast<std::ptrdiff_t> (keep) + 1);
    f->flats.erase (f->flats.begin () + static_cast<std::ptrdiff_t> (keep) + 1);
    return Write (s, std::move (d), *f);
}
Design SetRooms (const Scheme& s, Design d, int flat, double rooms)
{
    if (flat < 0 || flat >= static_cast<int> (s.flats.size ()))
        return d;
    if (auto f = Freeze (s, s.flats[flat].band)) {
        const auto it = std::find (f->flats.begin (), f->flats.end (), flat);
        if (it != f->flats.end ()) {
            f->rooms[static_cast<size_t> (it - f->flats.begin ())] = rooms;
            return Write (s, std::move (d), *f);
        }
    }
    // A flat of its own slot (an end cap, a section's flat): a room-count pin on it.
    const Ring& shape = s.flats[flat].shape;
    std::erase_if (d.pins.rooms, [&] (const Pins::Rooms& r) { return In (shape, r.at); });
    d.pins.rooms.push_back ({ Centroid (shape), rooms });
    return d;
}
Design MoveEnd (Design d, const End& end, Vec to)
{
    std::erase_if (d.pins.ends, [&] (const Pins::End& e) {
        return std::hypot (e.at.x - end.at.x, e.at.y - end.at.y) < kPinTolerance;
    });
    d.pins.ends.push_back ({ to });
    return d;
}
Design SetAccess (const Scheme& s, Design d, int wing, Access access)
{
    if (wing < 0 || wing >= static_cast<int> (s.wings.size ()))
        return d;
    const auto& w = s.wings[wing];
    std::erase_if (d.pins.access, [&] (const Pins::AccessAt& a) { return In (w.rect, a.at); });
    if (access != Access::Auto)
        d.pins.access.push_back ({ { (w.a.x + w.b.x) / 2, (w.a.y + w.b.y) / 2 }, access });
    return d;
}
Design AddRect (Design d, Ring rect)
{
    if (std::abs (Area (rect)) > 0.5)
        d.added.push_back (Counter (std::move (rect)));
    return d;
}
Design CutRect (Design d, Ring rect)
{
    if (std::abs (Area (rect)) > 0.5)
        d.cut.push_back (Counter (std::move (rect)));
    return d;
}
Ring Rectangle (const Scheme& s, Vec a, Vec b, double grid)
{
    Vec u { 1, 0 }, o = a;
    int wing = WingAt (s, a);
    if (wing < 0) { // drawn from outside: the nearest wing's frame
        double near = 1e18;
        for (size_t i = 0; i < s.wings.size (); ++i) {
            const Vec m = Centroid (s.wings[i].rect);
            const double dd = std::hypot (m.x - a.x, m.y - a.y);
            if (dd < near)
                wing = static_cast<int> (i), near = dd;
        }
    }
    if (wing >= 0) {
        const auto& w = s.wings[wing];
        u = Unit ({ w.b.x - w.a.x, w.b.y - w.a.y });
        o = w.rect.front ();
    }
    const Vec v { -u.y, u.x };
    // Corners snap to a line of the outline within 0.5 m (to stay flush with a facade), else to
    // the grid.
    std::vector<double> lu, lv;
    for (const auto& ring : s.outline)
        for (const auto& p : ring)
            lu.push_back (Dot ({ p.x - o.x, p.y - o.y }, u)), lv.push_back (Dot ({ p.x - o.x, p.y - o.y }, v));
    auto snapTo = [&] (double x, const std::vector<double>& lines) {
        double best = grid > 0 ? std::round (x / grid) * grid : x, near = 0.5;
        for (double l : lines)
            if (std::abs (l - x) < near)
                near = std::abs (l - x), best = l;
        return best;
    };
    const double a0 = snapTo (Dot ({ a.x - o.x, a.y - o.y }, u), lu),
                 a1 = snapTo (Dot ({ a.x - o.x, a.y - o.y }, v), lv);
    const double b0 = snapTo (Dot ({ b.x - o.x, b.y - o.y }, u), lu),
                 b1 = snapTo (Dot ({ b.x - o.x, b.y - o.y }, v), lv);
    auto at = [&] (double p, double q) { return Vec { o.x + u.x * p + v.x * q, o.y + u.y * p + v.y * q }; };
    return Counter ({ at (a0, a1), at (b0, a1), at (b0, b1), at (a0, b1) });
}
Design SwapFlats (const Scheme& s, Design d, int a, int b, std::string* why)
{
    const int n = static_cast<int> (s.flats.size ());
    if (a < 0 || b < 0 || a >= n || b >= n || a == b)
        return Refuse (std::move (d), why, "Drop the flat on another flat to change places with it.");
    if (Locked (s, d, a) || Locked (s, d, b))
        return Refuse (std::move (d), why, "A locked flat keeps its place: unlock it first.");
    const auto& fa = s.flats[a];
    const auto& fb = s.flats[b];
    if (fa.band >= 0 && fa.band == fb.band) {
        auto f = Freeze (s, fa.band);
        if (!f)
            return Refuse (std::move (d), why, "These flats cannot change places.");
        const size_t ka = Index (*f, a), kb = Index (*f, b);
        if (ka >= f->flats.size () || kb >= f->flats.size ())
            return Refuse (std::move (d), why, "These flats cannot change places.");
        std::vector<double> widths;
        for (size_t k = 0; k < f->lo.size (); ++k)
            widths.push_back (f->hi[k] - f->lo[k]);
        std::swap (widths[ka], widths[kb]);
        std::swap (f->rooms[ka], f->rooms[kb]);
        std::swap (f->flats[ka], f->flats[kb]);
        Relay (*f, widths);
        return Write (s, std::move (d), *f);
    }
    // Across the corridor, or in another section: each takes the other's room count and its width,
    // a neighbour in its row giving or taking the difference. A room count without its width would
    // leave a wide flat named for few rooms (user, 2026-10-14: "large like 5R but color and name
    // like 1.5R"). Where a width cannot follow -- the flat ends a corridor or a section, or its
    // neighbours are locked or too narrow -- it keeps its own, and the swap stands only if that width
    // suits the new count; else nothing changes and it says why.
    constexpr double kSameWidth = 0.2, kSuits = 0.5;
    const double ra = fa.rooms, rb = fb.rooms, wa = fa.frontage, wb = fb.frontage;
    double misfit = 0, misfitRooms = 0;
    const auto place = [&] (Design next, int flat, double rooms, double width) -> std::optional<Design> {
        const double own = s.flats[flat].frontage;
        const auto keep = [&] () -> std::optional<Design> {
            const Frontage suits = RoomFrontage (rooms);
            if (own < suits.min - kSuits || own > suits.max + kSuits) {
                misfit = own, misfitRooms = rooms;
                return std::nullopt;
            }
            return SetRooms (s, std::move (next), flat, rooms);
        };
        auto f = Freeze (s, s.flats[flat].band);
        const size_t k = f ? Index (*f, flat) : 0;
        if (!f || k >= f->flats.size ()) // its own slot: no width to give or take
            return keep ();
        const double delta = width - (f->hi[k] - f->lo[k]);
        const double least = kNarrowest + 0.2;
        if (std::abs (delta) > kSameWidth) {
            if (k + 1 < f->lo.size () && !Locked (s, next, f->flats[k + 1]) && f->hi[k + 1] - f->hi[k] - delta >= least)
                f->hi[k] += delta, f->lo[k + 1] += delta;
            else if (k > 0 && !Locked (s, next, f->flats[k - 1]) && f->lo[k] - delta - f->lo[k - 1] >= least)
                f->lo[k] -= delta, f->hi[k - 1] -= delta;
            else
                return keep ();
        }
        f->rooms[k] = rooms;
        return Write (s, std::move (next), *f);
    };
    auto first = place (d, a, rb, wb);
    auto both = first ? place (std::move (*first), b, ra, wa) : std::nullopt;
    if (!both)
        return Refuse (std::move (d), why,
                       "These flats cannot change places: a %.1f m wide flat cannot hold %gR, and its neighbours "
                       "cannot give or take the difference (locked, too narrow, or it ends a corridor or a section).",
                       misfit, misfitRooms);
    return std::move (*both);
}
std::optional<Insertion> Insert (const Scheme& s, Vec at, double rooms, std::string* why)
{
    const auto refuse = [why] (const char* text) -> std::optional<Insertion> {
        if (why)
            *why = text;
        return std::nullopt;
    };
    const int flat = FlatAt (s, at);
    if (flat < 0)
        return refuse ("Drop a new flat onto a flat: it splits off from it.");
    if (s.flats[flat].band < 0)
        return refuse (
            "A flat at a corridor's end or in a section does not split: drop the new flat on a row of flats.");
    const int band = s.flats[flat].band;
    const auto f = Freeze (s, band);
    if (!f)
        return refuse ("Drop a new flat onto a flat in a row of flats.");
    const size_t k = Index (*f, flat);
    if (k >= f->flats.size ())
        return refuse ("Drop a new flat onto a flat in a row of flats.");
    const double lo = f->lo[k], hi = f->hi[k];
    // A little over the narrowest: a span right at a minimum reads as too short for a flat.
    const auto [from, to] = SplitRange (s, *f, k, kNarrowest + 0.2);
    if (from > to)
        return refuse ("The flat there is too narrow to give room for another with a door on the corridor.");
    const double w = RoomFrontage (rooms).pref;
    // At the nearer end, but never the band's own end: a corner stays with the larger flat.
    const bool first = k == 0 && f->flats.size () > 1, last = k + 1 == f->flats.size () && f->flats.size () > 1;
    const bool low = first ? false : last ? true : f->U (at) < (lo + hi) / 2;
    const double wall = std::clamp (low ? lo + w : hi - w, from, to), u0 = low ? lo : wall, u1 = low ? wall : hi;
    const double half = s.bands[band].depth / 2;
    const Vec n { -f->dir.y, f->dir.x };
    const auto across = [&] (double u, double t) {
        const Vec c = f->At (u);
        return Vec { c.x + n.x * t, c.y + n.y * t };
    };
    Insertion out;
    out.flat = flat;
    out.a = across (wall, -half), out.b = across (wall, half);
    out.region = Counter ({ across (u0, -half), across (u1, -half), across (u1, half), across (u0, half) });
    return out;
}
Design InsertFlat (const Scheme& s, Design d, Vec at, double rooms, std::string* why)
{
    const auto ins = Insert (s, at, rooms, why);
    if (!ins)
        return d;
    if (Locked (s, d, ins->flat))
        return Refuse (std::move (d), why, "The flat there is locked: unlock it to put a flat in beside it.");
    auto f = Freeze (s, s.flats[ins->flat].band);
    if (!f)
        return Refuse (std::move (d), why, "Drop a new flat onto a flat in a row of flats.");
    const size_t k = Index (*f, ins->flat);
    const double wall = f->U ({ (ins->a.x + ins->b.x) / 2, (ins->a.y + ins->b.y) / 2 });
    const bool low = std::abs (wall - f->lo[k]) < std::abs (wall - f->hi[k]);
    const auto i = static_cast<std::ptrdiff_t> (k);
    // The flat splits at the new wall; the new one takes `rooms`, the rest is the generator's.
    f->lo.insert (f->lo.begin () + i + 1, wall);
    f->hi.insert (f->hi.begin () + i, wall);
    f->rooms[k] = 0;
    f->rooms.insert (f->rooms.begin () + (low ? i : i + 1), rooms);
    f->flats.insert (f->flats.begin () + i + 1, -1);
    return Write (s, std::move (d), *f);
}
double Narrowest (const floorprogramme::Programme& programme)
{
    double fewest = 1e9;
    for (const auto& type : programme.types)
        fewest = (std::min) (fewest, type.rooms);
    return RoomFrontage (fewest < 1e8 ? fewest : 1.5).min;
}
Design SetCount (const Scheme& s, Design d, int total, std::string* why, double narrowest, double most)
{
    const double least = (narrowest > 0 ? narrowest : kNarrowest) + 0.2;
    const double cap = most > 0 ? most + 0.5 : 1e18;
    std::vector<Frozen> bands;
    for (size_t b = 0; b < s.bands.size (); ++b)
        if (auto f = Freeze (s, static_cast<int> (b)))
            bands.push_back (std::move (*f));
    std::vector<char> touched (bands.size (), 0);
    const auto free = [&] (const Frozen& f, size_t k) { return f.flats[k] < 0 || !Locked (s, d, f.flats[k]); };
    // A run of unlocked flats in a row, and how wide its ends must be to reach the corridor by a door.
    struct Run {
        size_t band, a, z;
        double first, last;
    };
    const auto runs = [&] () {
        std::vector<Run> out;
        for (size_t b = 0; b < bands.size (); ++b) {
            const auto& f = bands[b];
            const auto [dlo, dhi] = Reach (s, f);
            const bool known = dlo <= dhi;
            for (size_t a = 0; a < f.lo.size ();) {
                if (!free (f, a)) {
                    ++a;
                    continue;
                }
                size_t z = a;
                while (z + 1 < f.lo.size () && free (f, z + 1))
                    ++z;
                out.push_back ({ b, a, z, known ? dlo + kDoor - f.lo[a] : 0, known ? f.hi[z] - (dhi - kDoor) : 0 });
                a = z + 1;
            }
        }
        return out;
    };
    // Run `r` laid again as `widths`: a flat whose walls stay keeps its rooms, the rest take what
    // their width fits.
    const auto relay = [&] (const Run& r, const std::vector<double>& widths) {
        auto& f = bands[r.band];
        const size_t m = r.z - r.a + 1;
        std::vector<double> lo, hi, rooms;
        std::vector<int> flats;
        double u = f.lo[r.a];
        for (const double width : widths) {
            size_t was = r.a + m;
            for (size_t k = r.a; k <= r.z; ++k)
                if (std::abs (f.lo[k] - u) < 0.05 && std::abs (f.hi[k] - (u + width)) < 0.05)
                    was = k;
            const bool same = was <= r.z;
            lo.push_back (u), hi.push_back (u + width);
            rooms.push_back (same ? f.rooms[was] : 0);
            flats.push_back (same ? f.flats[was] : -1);
            u += width;
        }
        const auto at = static_cast<std::ptrdiff_t> (r.a), to = static_cast<std::ptrdiff_t> (r.z + 1);
        f.lo.erase (f.lo.begin () + at, f.lo.begin () + to), f.lo.insert (f.lo.begin () + at, lo.begin (), lo.end ());
        f.hi.erase (f.hi.begin () + at, f.hi.begin () + to), f.hi.insert (f.hi.begin () + at, hi.begin (), hi.end ());
        f.rooms.erase (f.rooms.begin () + at, f.rooms.begin () + to);
        f.rooms.insert (f.rooms.begin () + at, rooms.begin (), rooms.end ());
        f.flats.erase (f.flats.begin () + at, f.flats.begin () + to);
        f.flats.insert (f.flats.begin () + at, flats.begin (), flats.end ());
        touched[r.band] = 1;
    };
    int now = static_cast<int> (s.flats.size ());
    while (now < total && !d.pins.joins.empty ()) {
        // The latest join undone first: each gives back the flat it took.
        d.pins.joins.pop_back ();
        ++now;
    }
    bool full = false;
    while (now < total) {
        // The run that stays widest laid again with one more.
        const auto all = runs ();
        size_t pick = all.size ();
        std::vector<double> best;
        double widest = 0;
        for (size_t i = 0; i < all.size (); ++i) {
            const auto& f = bands[all[i].band];
            std::vector<double> w;
            for (size_t k = all[i].a; k <= all[i].z; ++k)
                w.push_back (f.hi[k] - f.lo[k]);
            w = OneMore (std::move (w), least, all[i].first, all[i].last);
            const double narrow = w.empty () ? 0 : *std::min_element (w.begin (), w.end ());
            if (narrow > widest)
                widest = narrow, pick = i, best = std::move (w);
        }
        if (pick == all.size ()) {
            Refuse ({}, why,
                    "%d flats is as many as the floor takes now: no row has room for one more flat of %.1f m with a "
                    "door on the corridor.",
                    now, least);
            full = true;
            break;
        }
        relay (all[pick], best);
        ++now;
    }
    // At the most, each row it filled is laid evenly: between its ends, every flat the smallest.
    if (full)
        for (const auto& r : runs ()) {
            if (!touched[r.band])
                continue;
            const auto& f = bands[r.band];
            const size_t n = r.z - r.a + 1;
            std::vector<double> even (n, least);
            even.front () = (std::max) (even.front (), r.first);
            even.back () = (std::max) (even.back (), r.last);
            double slack = f.hi[r.z] - f.lo[r.a];
            for (const double x : even)
                slack -= x;
            if (slack < 0)
                continue;
            for (auto& x : even)
                x += slack / static_cast<double> (n);
            relay (r, even);
        }
    std::vector<Pins::Join> joins;
    // Which flats this call has joined into one: a join within one group would join a flat to itself.
    std::vector<int> group (s.flats.size ());
    for (size_t i = 0; i < group.size (); ++i)
        group[i] = static_cast<int> (i);
    const auto root = [&] (int i) {
        while (group[i] != i)
            i = group[i];
        return i;
    };
    while (now > total) {
        // The narrowest free flat with a free neighbour joins the narrower of them, if the two are
        // no larger than the largest flat.
        size_t bi = bands.size (), ki = 0, ji = 0;
        double narrow = 1e18;
        for (size_t b = 0; b < bands.size (); ++b) {
            const auto& f = bands[b];
            for (size_t k = 0; k < f.lo.size (); ++k) {
                if (!free (f, k))
                    continue;
                const bool before = k > 0 && free (f, k - 1), after = k + 1 < f.lo.size () && free (f, k + 1);
                if (!before && !after)
                    continue;
                const size_t j =
                    !after || (before && f.hi[k - 1] - f.lo[k - 1] <= f.hi[k + 1] - f.lo[k + 1]) ? k - 1 : k + 1;
                const double joined = (std::max) (f.hi[k], f.hi[j]) - (std::min) (f.lo[k], f.lo[j]);
                if (NetArea (joined, s.bands[f.band].depth) <= cap && f.hi[k] - f.lo[k] < narrow)
                    narrow = f.hi[k] - f.lo[k], bi = b, ki = k, ji = j;
            }
        }
        if (bi < bands.size ()) {
            auto& f = bands[bi];
            const size_t keep = (std::min) (ki, ji);
            const auto i = static_cast<std::ptrdiff_t> (keep);
            f.hi[keep] = f.hi[keep + 1];
            f.rooms[keep] = 0;
            f.lo.erase (f.lo.begin () + i + 1);
            f.hi.erase (f.hi.begin () + i + 1);
            f.rooms.erase (f.rooms.begin () + i + 1);
            f.flats.erase (f.flats.begin () + i + 1);
            touched[bi] = 1, --now;
            continue;
        }
        // No row left to close: the smallest flat (with what it has joined) joins a flat beside it.
        std::vector<double> net (s.flats.size (), 0);
        for (size_t i = 0; i < s.flats.size (); ++i)
            net[root (static_cast<int> (i))] += s.flats[i].net;
        int gone = -1, into = -1;
        for (size_t i = 0; i < s.flats.size (); ++i) {
            const int flat = static_cast<int> (i);
            if (Locked (s, d, flat) || (gone >= 0 && net[root (flat)] >= net[root (gone)]))
                continue;
            // A row flat a merge above took is part of its neighbour now.
            const auto row = std::find_if (bands.begin (), bands.end (),
                                           [&] (const Frozen& f) { return f.band == s.flats[i].band; });
            if (row != bands.end () && std::find (row->flats.begin (), row->flats.end (), flat) == row->flats.end ())
                continue;
            std::vector<int> mine;
            for (size_t j = 0; j < s.flats.size (); ++j)
                if (root (static_cast<int> (j)) == root (flat))
                    mine.push_back (static_cast<int> (j));
            if (const int other = Beside (s, d, flat, mine); other >= 0 && net[root (flat)] + net[root (other)] <= cap)
                gone = flat, into = other;
        }
        if (gone < 0) {
            if (most > 0)
                Refuse ({}, why,
                        "%d flats is as few as the floor takes now: two more joined would be over the largest flat "
                        "(%.0f m2).",
                        now, most);
            else
                Refuse ({}, why, "%d flats is as few as the floor takes now: no unlocked flats are left to join.", now);
            break;
        }
        joins.push_back ({ Within (s.flats[gone].shape), Within (s.flats[into].shape) });
        group[root (gone)] = root (into), --now;
    }
    for (size_t b = 0; b < bands.size (); ++b)
        if (touched[b])
            d = Write (s, std::move (d), bands[b]);
    for (const auto& j : joins) {
        const int gone = FlatAt (s, j.from);
        if (gone >= 0)
            std::erase_if (d.pins.rooms, [&] (const Pins::Rooms& r) { return In (s.flats[gone].shape, r.at); });
        d.pins.joins.push_back (j);
    }
    return d;
}
bool Locked (const Scheme& s, const Design& d, int flat)
{
    if (flat < 0 || flat >= static_cast<int> (s.flats.size ()))
        return false;
    return std::any_of (d.locked.begin (), d.locked.end (), [&] (Vec p) { return In (s.flats[flat].shape, p); });
}
Design SetLocked (const Scheme& s, Design d, int flat, bool locked)
{
    if (flat < 0 || flat >= static_cast<int> (s.flats.size ()) || Locked (s, d, flat) == locked)
        return d;
    const Ring& shape = s.flats[flat].shape;
    if (!locked) {
        std::erase_if (d.locked, [&] (Vec p) { return In (shape, p); });
        return d;
    }
    // Its walls and rooms pinned where they are, then marked.
    if (auto f = Freeze (s, s.flats[flat].band))
        d = Write (s, std::move (d), *f);
    else
        d = SetRooms (s, std::move (d), flat, s.flats[flat].rooms);
    d.locked.push_back (Centroid (shape));
    return d;
}
Vec SnapWall (const Scheme& s, const Wall& wall, Vec from, Vec to, const floorprogramme::Programme& programme,
              double step)
{
    const Vec axis = Unit (wall.axis);
    const double t = Dot ({ to.x - from.x, to.y - from.y }, axis);
    double best = std::round (t / step) * step, near = step / 2;
    const int n = static_cast<int> (s.flats.size ());
    for (const auto& [flat, sign] : { std::pair { wall.low, 1.0 }, std::pair { wall.high, -1.0 } }) {
        if (flat < 0 || flat >= n)
            continue;
        const auto& f = s.flats[flat];
        if (f.depth <= floorprogramme::kFacade + 0.5)
            continue;
        for (const auto& type : programme.types) {
            // The frontage at which this flat's net area is the type's middle (NetArea is linear in it).
            const double mid = (type.minM2 + type.maxM2) / 2;
            const double frontage = (mid + kPartyWall * f.depth) / (f.depth - floorprogramme::kFacade);
            const double shift = sign * (frontage - f.frontage);
            if (std::abs (shift - t) < near)
                near = std::abs (shift - t), best = shift;
        }
    }
    return { from.x + axis.x * best, from.y + axis.y * best };
}
Vec Stepped (const Scheme& s, Vec from, Vec to, double step)
{
    Vec u { 1, 0 };
    if (const int wing = WingAt (s, from); wing >= 0)
        u = Unit ({ s.wings[wing].b.x - s.wings[wing].a.x, s.wings[wing].b.y - s.wings[wing].a.y });
    const Vec v { -u.y, u.x }, d { to.x - from.x, to.y - from.y };
    const double du = std::round (Dot (d, u) / step) * step, dv = std::round (Dot (d, v) / step) * step;
    return { from.x + u.x * du + v.x * dv, from.y + u.y * du + v.y * dv };
}
} // namespace geomsrv::archviz::floorscheme::edit
