#include "ArchViz/FloorSchemeEdit.hpp"
#include "ArchViz/FloorSchemeDetail.hpp"
#include <algorithm>

namespace geomsrv::archviz::floorscheme::edit {
namespace {
using namespace detail;
constexpr size_t kUndo = 200;
// A flat no narrower than the smallest flat type's rooms (a 1.5R: living room and alcove).
const double kNarrowest = RoomFrontage (1.5).min;

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
cp::PathsD Clean (const std::vector<Ring>& rings)
{
    return cp::Union (ToPaths (rings), cp::FillRule::NonZero, kPrecision);
}
// Where a wall may split span `k` of a frozen band: both parts at least `least` wide and each
// reaching the band's circulation (corridor, stair, lobby) by a door's width, as the generator
// asks of every flat. Empty (lo > hi) when the span cannot split.
std::pair<double, double> SplitRange (const Scheme& s, const Frozen& f, size_t k, double least)
{
    constexpr double kDoor = 1.3;
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
} // namespace

std::vector<Ring> Outline (const std::vector<Ring>& floor, const Design& design)
{
    cp::PathsD all = ToPaths (floor);
    for (const auto& r : design.added)
        all.push_back (ToPath (Counter (r)));
    auto joined = cp::Union (all, cp::FillRule::NonZero, kPrecision);
    if (!design.cut.empty ()) {
        cp::PathsD cut;
        for (const auto& r : design.cut)
            cut.push_back (ToPath (Counter (r)));
        joined = cp::Difference (joined, cut, cp::FillRule::NonZero, kPrecision);
    }
    return FromPaths (joined);
}

std::vector<std::vector<Ring>> Owned (const std::vector<std::vector<Ring>>& floors,
                                      const std::vector<const Design*>& designs)
{
    const size_t n = floors.size ();
    std::vector<cp::PathsD> base (n), added (n);
    cp::PathsD before;
    for (size_t i = 0; i < n; ++i) {
        const auto floor = Clean (floors[i]);
        base[i] = before.empty () ? floor : cp::Difference (floor, before, cp::FillRule::NonZero, kPrecision);
        before = cp::Union (before, floor, cp::FillRule::NonZero, kPrecision);
        if (i < designs.size () && designs[i])
            added[i] = Clean (designs[i]->added);
    }
    std::vector<std::vector<Ring>> out (n);
    for (size_t i = 0; i < n; ++i) {
        auto own = added[i].empty () ? base[i] : cp::Union (base[i], added[i], cp::FillRule::NonZero, kPrecision);
        if (i < designs.size () && designs[i] && !designs[i]->cut.empty ())
            own = cp::Difference (own, Clean (designs[i]->cut), cp::FillRule::NonZero, kPrecision);
        cp::PathsD others;
        for (size_t j = 0; j < n; ++j)
            if (j != i)
                others.insert (others.end (), added[j].begin (), added[j].end ());
        if (!others.empty ())
            own = cp::Difference (own, cp::Union (others, cp::FillRule::NonZero, kPrecision), cp::FillRule::NonZero,
                                  kPrecision);
        out[i] = FromPaths (own);
    }
    return out;
}
Scheme RunOwned (const std::vector<Ring>& owned, const std::vector<Ring>& party,
                 const floorprogramme::Programme& programme, const Design& design, const Options& options)
{
    Options o = options;
    o.shallow = design.shallow;
    o.party = party;
    return Generate (owned, programme, design.pins, o);
}
std::vector<Ring> PartyStrip (const std::array<Vec, 2>& wall, double distance, const std::vector<Ring>& from)
{
    if (std::abs (distance) < 0.05 || from.empty ())
        return {};
    const Vec a = wall[0], b = wall[1];
    const Vec d = Unit ({ b.x - a.x, b.y - a.y }), n { d.y, -d.x }; // outward: right of a counter-clockwise outline
    const Ring strip = Counter (
        { a, b, { b.x + n.x * distance, b.y + n.y * distance }, { a.x + n.x * distance, a.y + n.y * distance } });
    std::vector<Ring> out;
    for (auto& r : FromPaths (cp::Intersect ({ ToPath (strip) }, Clean (from), cp::FillRule::NonZero, kPrecision)))
        if (std::abs (Area (r)) > 0.05)
            out.push_back (std::move (r));
    return out;
}
void Take (Design& gainer, Design& loser, const std::vector<Ring>& strip)
{
    if (strip.empty ())
        return;
    auto added = ToPaths (gainer.added);
    for (const auto& r : strip)
        added.push_back (ToPath (Counter (r)));
    gainer.added = FromPaths (cp::Union (added, cp::FillRule::NonZero, kPrecision));
    const auto paths = Clean (strip);
    if (!gainer.cut.empty ())
        gainer.cut = FromPaths (cp::Difference (Clean (gainer.cut), paths, cp::FillRule::NonZero, kPrecision));
    if (!loser.added.empty ())
        loser.added = FromPaths (cp::Difference (Clean (loser.added), paths, cp::FillRule::NonZero, kPrecision));
}

Scheme Run (const std::vector<Ring>& floor, const floorprogramme::Programme& programme, const Design& design,
            const Options& options)
{
    Options o = options;
    o.shallow = design.shallow;
    return Generate (Outline (floor, design), programme, design.pins, o);
}

Access Chosen (const Scheme& scheme, const Options& options)
{
    Access chosen = Access::Auto;
    for (const auto& w : scheme.wings)
        if (w.depth < options.centreDepth - 1e-6 && (w.access == Access::OneSide || w.access == Access::CoreOnly))
            chosen = chosen == Access::Auto || chosen == w.access ? w.access : chosen;
    return chosen;
}

bool Session::Commit (Design next)
{
    if (next == design)
        return false;
    undo.push_back (std::move (design));
    if (undo.size () > kUndo)
        undo.erase (undo.begin ());
    design = std::move (next);
    redo.clear ();
    return true;
}
bool Session::Undo ()
{
    if (undo.empty ())
        return false;
    redo.push_back (std::move (design));
    design = std::move (undo.back ());
    undo.pop_back ();
    return true;
}
bool Session::Redo ()
{
    if (redo.empty ())
        return false;
    undo.push_back (std::move (design));
    design = std::move (redo.back ());
    redo.pop_back ();
    return true;
}
void Session::Reset ()
{
    Commit ({});
}

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

Design MoveStair (const Scheme& s, Design d, int core, Vec to)
{
    if (core < 0 || core >= static_cast<int> (s.cores.size ()))
        return d;
    const Vec from = s.cores[core].centre;
    d = PinStairs (s, std::move (d), WingAt (s, from));
    const size_t i = Nearest (d.pins.cores, from);
    if (i < d.pins.cores.size ())
        d.pins.cores[i].centre = to;
    else
        d.pins.cores.push_back ({ to });
    return d;
}
Design AddStair (const Scheme& s, Design d, Vec at)
{
    const int wing = WingAt (s, at);
    if (wing < 0)
        return d;
    int count = 1;
    for (const auto& core : s.cores)
        count += In (s.wings[wing].rect, core.centre);
    return SpaceStairs (s, std::move (d), wing, count, &at);
}
Design RemoveStair (const Scheme& s, Design d, int core)
{
    if (core < 0 || core >= static_cast<int> (s.cores.size ()))
        return d;
    const int wing = WingAt (s, s.cores[core].centre);
    if (wing < 0)
        return d;
    int count = -1;
    for (const auto& c : s.cores)
        count += In (s.wings[wing].rect, c.centre);
    return count < 1 ? d : SpaceStairs (s, std::move (d), wing, count, nullptr);
}
Design MoveWall (const Scheme& s, Design d, const Wall& wall, Vec to)
{
    if (wall.low < 0 || wall.high < 0)
        return d;
    auto f = Freeze (s, s.flats[wall.low].band);
    if (!f)
        return d;
    const auto it = std::find (f->flats.begin (), f->flats.end (), wall.high);
    if (it == f->flats.begin () || it == f->flats.end ())
        return d;
    const size_t k = static_cast<size_t> (it - f->flats.begin ());
    const double lo = f->lo[k - 1] + kNarrowest, hi = f->hi[k] - kNarrowest;
    if (lo > hi)
        return d;
    const double u = std::clamp (f->U (to), lo, hi);
    f->hi[k - 1] = f->lo[k] = u;
    return Write (s, std::move (d), *f);
}
Design SplitFlat (const Scheme& s, Design d, int flat, Vec at)
{
    if (flat < 0 || flat >= static_cast<int> (s.flats.size ()) || Locked (s, d, flat))
        return d;
    auto f = Freeze (s, s.flats[flat].band);
    if (!f)
        return d;
    const auto it = std::find (f->flats.begin (), f->flats.end (), flat);
    if (it == f->flats.end ())
        return d;
    const size_t k = static_cast<size_t> (it - f->flats.begin ());
    const auto [lo, hi] = SplitRange (s, *f, k, kNarrowest + 0.2);
    if (lo > hi)
        return d; // too narrow for two flats that each reach the corridor
    const double u = std::clamp (f->U (at), lo, hi);
    f->lo.insert (f->lo.begin () + static_cast<std::ptrdiff_t> (k) + 1, u);
    f->hi.insert (f->hi.begin () + static_cast<std::ptrdiff_t> (k), u);
    f->rooms[k] = 0;
    f->rooms.insert (f->rooms.begin () + static_cast<std::ptrdiff_t> (k), 0);
    f->flats.insert (f->flats.begin () + static_cast<std::ptrdiff_t> (k), -1);
    return Write (s, std::move (d), *f);
}
Design RemoveFlat (const Scheme& s, Design d, int flat)
{
    if (flat < 0 || flat >= static_cast<int> (s.flats.size ()) || Locked (s, d, flat))
        return d;
    auto f = Freeze (s, s.flats[flat].band);
    if (!f || f->flats.size () < 2)
        return d; // the band's only flat stays
    const auto it = std::find (f->flats.begin (), f->flats.end (), flat);
    if (it == f->flats.end ())
        return d;
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
Design SwapFlats (const Scheme& s, Design d, int a, int b)
{
    const int n = static_cast<int> (s.flats.size ());
    if (a < 0 || b < 0 || a >= n || b >= n || a == b || Locked (s, d, a) || Locked (s, d, b))
        return d;
    const auto& fa = s.flats[a];
    const auto& fb = s.flats[b];
    if (fa.band >= 0 && fa.band == fb.band) {
        auto f = Freeze (s, fa.band);
        if (!f)
            return d;
        const size_t ka = Index (*f, a), kb = Index (*f, b);
        if (ka >= f->flats.size () || kb >= f->flats.size ())
            return d;
        std::vector<double> widths;
        for (size_t k = 0; k < f->lo.size (); ++k)
            widths.push_back (f->hi[k] - f->lo[k]);
        std::swap (widths[ka], widths[kb]);
        std::swap (f->rooms[ka], f->rooms[kb]);
        std::swap (f->flats[ka], f->flats[kb]);
        Relay (*f, widths);
        return Write (s, std::move (d), *f);
    }
    // Across the corridor, or in another section: each takes the other's room count and, where a
    // neighbour in its band can give or take the difference, its width.
    const double ra = fa.rooms, rb = fb.rooms, wa = fa.frontage, wb = fb.frontage;
    const auto place = [&] (Design next, int flat, double rooms, double width) {
        auto f = Freeze (s, s.flats[flat].band);
        if (!f)
            return SetRooms (s, std::move (next), flat, rooms);
        const size_t k = Index (*f, flat);
        if (k >= f->flats.size ())
            return SetRooms (s, std::move (next), flat, rooms);
        const double delta = width - (f->hi[k] - f->lo[k]);
        const double least = kNarrowest + 0.2;
        if (k + 1 < f->lo.size () && !Locked (s, next, f->flats[k + 1]) && f->hi[k + 1] - f->hi[k] - delta >= least)
            f->hi[k] += delta, f->lo[k + 1] += delta;
        else if (k > 0 && !Locked (s, next, f->flats[k - 1]) && f->lo[k] - delta - f->lo[k - 1] >= least)
            f->lo[k] -= delta, f->hi[k - 1] -= delta;
        f->rooms[k] = rooms;
        return Write (s, std::move (next), *f);
    };
    if (fa.band >= 0 && fb.band >= 0)
        return place (place (std::move (d), a, rb, wb), b, ra, wa);
    d = SetRooms (s, std::move (d), a, rb);
    return SetRooms (s, std::move (d), b, ra);
}
std::optional<Insertion> Insert (const Scheme& s, Vec at, double rooms)
{
    const int flat = FlatAt (s, at);
    if (flat < 0 || s.flats[flat].band < 0)
        return std::nullopt;
    const int band = s.flats[flat].band;
    const auto f = Freeze (s, band);
    if (!f)
        return std::nullopt;
    const size_t k = Index (*f, flat);
    if (k >= f->flats.size ())
        return std::nullopt;
    const double lo = f->lo[k], hi = f->hi[k];
    // A little over the narrowest: a span right at a minimum reads as too short for a flat.
    const auto [from, to] = SplitRange (s, *f, k, kNarrowest + 0.2);
    if (from > to)
        return std::nullopt;
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
Design InsertFlat (const Scheme& s, Design d, Vec at, double rooms)
{
    const auto ins = Insert (s, at, rooms);
    if (!ins || Locked (s, d, ins->flat))
        return d;
    auto f = Freeze (s, s.flats[ins->flat].band);
    if (!f)
        return d;
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
Design SetCount (const Scheme& s, Design d, int total)
{
    std::vector<Frozen> bands;
    for (size_t b = 0; b < s.bands.size (); ++b)
        if (auto f = Freeze (s, static_cast<int> (b)))
            bands.push_back (std::move (*f));
    std::vector<char> touched (bands.size (), 0);
    const auto free = [&] (const Frozen& f, size_t k) { return f.flats[k] < 0 || !Locked (s, d, f.flats[k]); };
    int now = static_cast<int> (s.flats.size ());
    while (now < total) {
        size_t bi = bands.size (), ki = 0;
        double widest = 0, mid = 0;
        for (size_t b = 0; b < bands.size (); ++b)
            for (size_t k = 0; k < bands[b].lo.size (); ++k) {
                const auto [lo, hi] = SplitRange (s, bands[b], k, kNarrowest + 0.2);
                if (free (bands[b], k) && lo <= hi && bands[b].hi[k] - bands[b].lo[k] > widest)
                    widest = bands[b].hi[k] - bands[b].lo[k], bi = b, ki = k,
                    mid = std::clamp ((bands[b].lo[k] + bands[b].hi[k]) / 2, lo, hi);
            }
        if (bi == bands.size ())
            break; // nothing wide enough to split
        auto& f = bands[bi];
        const auto i = static_cast<std::ptrdiff_t> (ki);
        f.lo.insert (f.lo.begin () + i + 1, mid);
        f.hi.insert (f.hi.begin () + i, mid);
        f.rooms[ki] = 0;
        f.rooms.insert (f.rooms.begin () + i, 0);
        f.flats.insert (f.flats.begin () + i + 1, -1);
        touched[bi] = 1, ++now;
    }
    while (now > total) {
        // The narrowest free flat with a free neighbour joins the narrower of them.
        size_t bi = bands.size (), ki = 0, ji = 0;
        double narrowest = 1e18;
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
                if (f.hi[k] - f.lo[k] < narrowest)
                    narrowest = f.hi[k] - f.lo[k], bi = b, ki = k, ji = j;
            }
        }
        if (bi == bands.size ())
            break;
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
    }
    for (size_t b = 0; b < bands.size (); ++b)
        if (touched[b])
            d = Write (s, std::move (d), bands[b]);
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
