#include "ArchViz/HudBuildingPlan.hpp"
#include <clipper2/clipper.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace geomsrv::archviz::buildingplan {
namespace {
namespace cp = Clipper2Lib;
constexpr double kCorridor = 1.8, kModule = 0.3, kMinWidth = 3.4;
constexpr size_t kMaxUnits = 128, kMaxBars = 64, kMaxVertices = 2048;
struct Rect {
    double x0, y0, x1, y1;
};
cp::PathD Polygon (Rect r)
{
    return { { r.x0, r.y0 }, { r.x1, r.y0 }, { r.x1, r.y1 }, { r.x0, r.y1 } };
}
Point Local (const QuickPlan& plan, Point p)
{
    const double x = p.x - plan.origin.x, y = p.y - plan.origin.y;
    return { x * std::cos (plan.angle) + y * std::sin (plan.angle),
             -x * std::sin (plan.angle) + y * std::cos (plan.angle) };
}
Point World (const QuickPlan& plan, Point p)
{
    return { plan.origin.x + p.x * std::cos (plan.angle) - p.y * std::sin (plan.angle),
             plan.origin.y + p.x * std::sin (plan.angle) + p.y * std::cos (plan.angle) };
}
cp::PathsD Paths (const QuickPlan& plan, const std::vector<SliceChain>& rings)
{
    cp::PathsD out;
    for (const auto& ring : rings) {
        cp::PathD path;
        for (size_t i = 0; i < ring.Count (); ++i) {
            const auto p = Local (plan, { ring.xy[i * 2], ring.xy[i * 2 + 1] });
            path.emplace_back (p.x, p.y);
        }
        out.push_back (std::move (path));
    }
    return out;
}
PlanRegion Region (const QuickPlan& plan, const cp::PathsD& paths)
{
    PlanRegion out;
    for (const auto& path : paths) {
        SliceChain ring;
        ring.closed = true;
        for (const auto& p : path) {
            const auto q = World (plan, { p.x, p.y });
            ring.xy.insert (ring.xy.end (), { q.x, q.y });
        }
        out.rings.push_back (std::move (ring));
    }
    return out;
}
std::vector<SliceChain> Counted (const Floor& floor)
{
    if (floor.outlineKnown)
        return floor.outline;
    cp::PathsD parts;
    Point origin;
    if (!floor.contours.empty () && !floor.contours.front ().empty () && floor.contours.front ().front ().Count ())
        origin = { floor.contours.front ().front ().xy[0], floor.contours.front ().front ().xy[1] };
    QuickPlan frame;
    frame.origin = origin;
    for (const auto& source : floor.contours) {
        const auto own = cp::Union (Paths (frame, source), cp::FillRule::EvenOdd, 6);
        parts.insert (parts.end (), own.begin (), own.end ());
    }
    return Region (frame, cp::Union (parts, cp::FillRule::NonZero, 6)).rings;
}
bool Inside (const cp::PathsD& paths, Point p)
{
    int winding = 0;
    for (const auto& path : paths) {
        const auto where = cp::PointInPolygon (cp::PointD (p.x, p.y), path);
        if (where == cp::PointInPolygonResult::IsOn)
            return true;
        if (where == cp::PointInPolygonResult::IsInside)
            winding += cp::Area (path) > 0 ? 1 : -1;
    }
    return winding != 0;
}
bool Hit (const QuickPlan& plan, const PlanRegion& region, Point p)
{
    return Inside (Paths (plan, region.rings), Local (plan, p));
}
double Along (const QuickPlan& plan, const PlanRegion& segment, Point p)
{
    const auto q = Local (plan, p);
    return segment.alongX ? q.x : q.y;
}
Rect Strip (bool alongX, double lo, double hi, double c0, double c1)
{
    return alongX ? Rect { lo, c0, hi, c1 } : Rect { c0, lo, c1, hi };
}
} // namespace

std::string QuickSignature (const Floor& floor, const std::vector<Point>& stairs)
{
    // Floor height/index are NOT template identity. Canonical world rings at micrometre precision
    // share across input order, start vertex and winding, but never across translated buildings.
    std::ostringstream key;
    key << std::fixed << std::setprecision (6);
    if (!floor.outlineKey.empty ()) {
        key << floor.outlineKey;
        for (const auto& p : stairs)
            key << p.x << ',' << p.y << ';';
        return key.str ();
    }
    std::vector<std::string> keys;
    const auto append = [&] (const auto& rings) {
        for (const auto& ring : rings) {
            cp::PathD canonical;
            bool finite = true;
            for (size_t i = 0; i < ring.Count (); ++i) {
                const double x = ring.xy[i * 2], y = ring.xy[i * 2 + 1];
                finite &= std::isfinite (x) && std::isfinite (y) && std::abs (x) <= 1e9 && std::abs (y) <= 1e9;
                canonical.emplace_back (x, y);
            }
            if (finite && canonical.size () >= 3)
                canonical = cp::TrimCollinear (canonical, 6);
            std::vector<std::string> points;
            for (const auto& point : canonical) {
                std::ostringstream p;
                p << std::fixed << std::setprecision (6) << (std::abs (point.x) < 0.5e-6 ? 0 : point.x) << ','
                  << (std::abs (point.y) < 0.5e-6 ? 0 : point.y);
                points.push_back (p.str ());
            }
            if (points.empty ())
                continue;
            const size_t start = size_t (std::min_element (points.begin (), points.end ()) - points.begin ());
            std::string forward, reverse;
            for (size_t i = 0; i < points.size (); ++i) {
                forward += points[(start + i) % points.size ()] + ';';
                reverse += points[(start + points.size () - i) % points.size ()] + ';';
            }
            keys.push_back (std::to_string (ring.closed) + ':' + (std::min) (forward, reverse));
        }
    };
    if (floor.outlineKnown)
        append (floor.outline);
    else
        for (const auto& source : floor.contours)
            append (source);
    std::sort (keys.begin (), keys.end ());
    for (const auto& ring : keys)
        key << '/' << ring;
    key << '#';
    for (const auto& p : stairs)
        key << p.x << ',' << p.y << ';';
    return key.str ();
}

QuickPlan GenerateQuick (const Floor& floor, const std::vector<Point>& stairs)
{
    QuickPlan plan;
    plan.signature = QuickSignature (floor, stairs);
    plan.note = "Place a stairwell to generate a quick floor scheme.";
    const auto valid = [] (const auto& values) {
        size_t count = 0;
        for (const auto& ring : values) {
            count += ring.Count ();
            if (!ring.closed || ring.xy.size () % 2 || ring.Count () < 3 || count > kMaxVertices ||
                std::any_of (ring.xy.begin (), ring.xy.end (),
                             [] (double x) { return !std::isfinite (x) || std::abs (x) > 1e9; }))
                return false;
        }
        return true;
    };
    if ((floor.outlineKnown && !valid (floor.outline)) ||
        (!floor.outlineKnown && std::any_of (floor.contours.begin (), floor.contours.end (),
                                             [&] (const auto& source) { return !valid (source); }))) {
        plan.note = "Quick plan refused: invalid outline or vertex budget exceeded.";
        return plan;
    }
    const auto rings = Counted (floor);
    size_t count = 0;
    double longest = 0;
    for (const auto& ring : rings) {
        if (!ring.closed || ring.xy.size () % 2 || ring.Count () < 3) {
            plan.note = "Quick plan refused: invalid counted outline.";
            return plan;
        }
        count += ring.Count ();
        for (size_t i = 0; i < ring.Count (); ++i) {
            const size_t j = (i + 1) % ring.Count ();
            const double x = ring.xy[j * 2] - ring.xy[i * 2], y = ring.xy[j * 2 + 1] - ring.xy[i * 2 + 1];
            if (!std::isfinite (x) || !std::isfinite (y)) {
                plan.note = "Quick plan refused: non-finite outline.";
                return plan;
            }
            if (std::hypot (x, y) > longest) {
                longest = std::hypot (x, y);
                plan.angle = std::atan2 (y, x);
                plan.origin = { ring.xy[i * 2], ring.xy[i * 2 + 1] };
            }
        }
    }
    if (!count || count > kMaxVertices || stairs.size () > kMaxStairs) {
        plan.note = "Quick plan refused: empty outline or preview budget exceeded.";
        return plan;
    }
    const auto outline = Paths (plan, rings);
    std::vector<double> xs, ys;
    for (const auto& path : outline)
        for (size_t i = 0; i < path.size (); ++i) {
            const auto& a = path[i];
            const auto& b = path[(i + 1) % path.size ()];
            if (std::abs (a.x - b.x) > 1e-5 && std::abs (a.y - b.y) > 1e-5) {
                plan.note = "Quick S2 currently requires an orthogonal outline in its local frame; no box substitute.";
                return plan;
            }
            xs.push_back (a.x);
            ys.push_back (a.y);
        }
    const auto unique = [] (std::vector<double>& values) {
        std::sort (values.begin (), values.end ());
        values.erase (
            std::unique (values.begin (), values.end (), [] (double a, double b) { return std::abs (a - b) < 1e-5; }),
            values.end ());
    };
    unique (xs);
    unique (ys);
    if (xs.size () * ys.size () > 16384) {
        plan.note = "Quick S2 decomposition exceeds the preview budget.";
        return plan;
    }
    // Orthogonal sweep retains holes and islands; join equal across spans, never a bounding box.
    std::vector<Rect> bars;
    for (size_t x = 1; x < xs.size (); ++x) {
        size_t y = 1;
        while (y < ys.size ()) {
            if (!Inside (outline, { (xs[x - 1] + xs[x]) / 2, (ys[y - 1] + ys[y]) / 2 })) {
                ++y;
                continue;
            }
            const double bottom = ys[y - 1];
            while (y + 1 < ys.size () && Inside (outline, { (xs[x - 1] + xs[x]) / 2, (ys[y] + ys[y + 1]) / 2 }))
                ++y;
            Rect rect { xs[x - 1], bottom, xs[x], ys[y] };
            auto prior = std::find_if (bars.begin (), bars.end (), [&] (const Rect& b) {
                return std::abs (b.x1 - rect.x0) < 1e-5 && std::abs (b.y0 - rect.y0) < 1e-5 &&
                       std::abs (b.y1 - rect.y1) < 1e-5;
            });
            if (prior == bars.end ())
                bars.push_back (rect);
            else
                prior->x1 = rect.x1;
            ++y;
        }
    }
    if (bars.size () > kMaxBars) {
        plan.note = "Quick S2 decomposition exceeds 64 bars.";
        return plan;
    }
    for (const auto& b : bars)
        plan.bars.push_back (Region (plan, { Polygon (b) }));
    if (stairs.empty ())
        return plan;
    cp::PathsD cores;
    for (const auto& p : stairs) {
        if (!Fits (floor, p)) {
            plan.note = "Quick plan refused: every proposed stair footprint must fit this floor.";
            return plan;
        }
        cp::PathD core;
        for (const auto& v :
             Polygon ({ p.x - kStairWidth / 2, p.y - kStairDepth / 2, p.x + kStairWidth / 2, p.y + kStairDepth / 2 })) {
            const auto q = Local (plan, { v.x, v.y });
            core.emplace_back (q.x, q.y);
        }
        if (std::abs (cp::Area (cp::Intersect (cores, { core }, cp::FillRule::NonZero, 6))) > 1e-6) {
            plan.note = "Quick plan refused: proposed stair footprints overlap.";
            return plan;
        }
        cores.push_back (std::move (core));
    }
    cp::PathsD circulation;
    std::vector<Rect> spines;
    std::vector<Rect> spineBars;
    for (const auto& b : bars) {
        const bool axis = b.x1 - b.x0 >= b.y1 - b.y0;
        const double lo = axis ? b.x0 : b.y0, hi = axis ? b.x1 : b.y1;
        const double c0 = axis ? b.y0 : b.x0, c1 = axis ? b.y1 : b.x1;
        if (c1 - c0 < kCorridor + kMinWidth)
            continue;
        double cross = (c0 + c1) / 2;
        if (c1 - c0 < 2 * 5.4 + kCorridor)
            cross = c0 + kCorridor / 2; // Shallow bar: single-loaded, not two unusable bands.
        const auto spine = Strip (axis, lo, hi, cross - kCorridor / 2, cross + kCorridor / 2);
        circulation.push_back (Polygon (spine));
        spines.push_back (spine);
        spineBars.push_back (b);
    }
    // Join touching bars THROUGH their shared edge, never straight across a courtyard.
    // The later component check still refuses islands or clipped-off connections.
    for (size_t i = 0; i < spines.size (); ++i)
        for (size_t j = i + 1; j < spines.size (); ++j) {
            const auto& a = spineBars[i];
            const auto& b = spineBars[j];
            const double x0 = (std::max) (a.x0, b.x0), x1 = (std::min) (a.x1, b.x1);
            const double y0 = (std::max) (a.y0, b.y0), y1 = (std::min) (a.y1, b.y1);
            const bool vertical = std::abs (x1 - x0) < 1e-5 && y1 - y0 >= kCorridor;
            const bool horizontal = std::abs (y1 - y0) < 1e-5 && x1 - x0 >= kCorridor;
            if (!vertical && !horizontal)
                continue;
            const Point shared { (x0 + x1) / 2, (y0 + y1) / 2 };
            for (size_t index : { i, j }) {
                const auto& spine = spines[index];
                const Point p { std::clamp (shared.x, spine.x0, spine.x1), std::clamp (shared.y, spine.y0, spine.y1) };
                const double w = kCorridor / 2;
                const Point bend = vertical ? Point { p.x, shared.y } : Point { shared.x, p.y };
                circulation.push_back (Polygon ({ (std::min) (p.x, bend.x) - w, (std::min) (p.y, bend.y) - w,
                                                  (std::max) (p.x, bend.x) + w, (std::max) (p.y, bend.y) + w }));
                circulation.push_back (
                    Polygon ({ (std::min) (shared.x, bend.x) - w, (std::min) (shared.y, bend.y) - w,
                               (std::max) (shared.x, bend.x) + w, (std::max) (shared.y, bend.y) + w }));
            }
        }
    // Connect each core to the nearest spine; clipping alone is NOT connectivity validation.
    for (const auto& p : stairs) {
        const auto q = Local (plan, p);
        double best = 1e300;
        Point target;
        for (const auto& spine : spines) {
            const Point t { std::clamp (q.x, spine.x0, spine.x1), std::clamp (q.y, spine.y0, spine.y1) };
            const double d = std::hypot (q.x - t.x, q.y - t.y);
            if (d < best) {
                best = d;
                target = t;
            }
        }
        if (best < 1e299) {
            const double w = kCorridor / 2;
            circulation.push_back (
                Polygon ({ (std::min) (q.x, target.x) - w, q.y - w, (std::max) (q.x, target.x) + w, q.y + w }));
            circulation.push_back (Polygon (
                { target.x - w, (std::min) (q.y, target.y) - w, target.x + w, (std::max) (q.y, target.y) + w }));
        }
    }
    circulation = cp::Intersect (cp::Union (circulation, cp::FillRule::NonZero, 6), outline, cp::FillRule::NonZero, 6);
    const auto reserved = cp::Union (circulation, cores, cp::FillRule::NonZero, 6);
    // Reject a corridor component that has no core. No apparently served apartments across a courtyard/island.
    for (const auto& component : reserved)
        if (cp::Area (component) > 0 &&
            std::abs (cp::Area (cp::Intersect ({ component }, cores, cp::FillRule::NonZero, 6))) < 1e-6) {
            plan.note = "Quick S4 refused: a corridor/island is disconnected from all proposed stairs.";
            return plan;
        }
    plan.corridors.push_back (Region (plan, cp::Difference (circulation, cores, cp::FillRule::NonZero, 6)));
    for (size_t b = 0; b < bars.size (); ++b) {
        const auto& rect = bars[b];
        const bool axis = rect.x1 - rect.x0 >= rect.y1 - rect.y0;
        const auto available = cp::Difference ({ Polygon (rect) }, circulation, cp::FillRule::NonZero, 6);
        if (!available.empty ())
            plan.bands.push_back (Region (plan, available));
        // S5: sweep cuts at every circulation/core vertex; merge across spans to obtain segments.
        const auto remaining = cp::Difference (available, cores, cp::FillRule::NonZero, 6);
        std::vector<double> cuts { axis ? rect.x0 : rect.y0, axis ? rect.x1 : rect.y1 };
        for (const auto& path : remaining)
            for (const auto& p : path)
                cuts.push_back (axis ? p.x : p.y);
        unique (cuts);
        for (size_t c = 1; c < cuts.size (); ++c) {
            if (cuts[c] - cuts[c - 1] < kMinWidth)
                continue;
            const auto pieces = cp::Intersect (
                remaining,
                { Polygon (Strip (axis, cuts[c - 1], cuts[c], axis ? rect.y0 : rect.x0, axis ? rect.y1 : rect.x1)) },
                cp::FillRule::NonZero, 6);
            for (const auto& path : pieces) {
                if (cp::Area (path) <= 1e-6)
                    continue;
                double a = 1e300, z = -1e300;
                for (const auto& p : path) {
                    a = (std::min) (a, axis ? p.y : p.x);
                    z = (std::max) (z, axis ? p.y : p.x);
                }
                auto segment = Region (plan, { path });
                segment.alongX = axis;
                segment.lo = cuts[c - 1];
                segment.hi = cuts[c];
                segment.across = (a + z) / 2;
                segment.center = World (plan, axis ? Point { (segment.lo + segment.hi) / 2, segment.across }
                                                   : Point { segment.across, (segment.lo + segment.hi) / 2 });
                if (!Hit (plan, segment, segment.center))
                    continue;
                // Require actual shared corridor boundary, not just proximity or a stair marker.
                const auto expanded =
                    cp::InflatePaths (cp::PathsD { path }, 0.01, cp::JoinType::Miter, cp::EndType::Polygon);
                if (std::abs (cp::Area (cp::Intersect (expanded, circulation, cp::FillRule::NonZero, 6))) < 1e-6)
                    continue;
                const size_t index = plan.segments.size ();
                plan.segments.push_back (std::move (segment));
                const int units =
                    (std::max) (1, int (std::floor ((cuts[c] - cuts[c - 1]) / (std::max) (kMinWidth, 55.0 / (z - a)))));
                for (int n = 0; n < units; ++n) {
                    if (plan.seeds.size () >= kMaxUnits) {
                        plan.note = "Quick apartment division exceeds 128 units; no partial scheme shown.";
                        plan.segments.clear ();
                        plan.seeds.clear ();
                        return plan;
                    }
                    plan.seeds.push_back (
                        { plan.nextId++, index, cuts[c - 1] + (n + 0.5) * (cuts[c] - cuts[c - 1]) / units });
                }
            }
        }
    }
    plan.ready = !plan.seeds.empty ();
    plan.note =
        plan.ready
            ? "Quick heuristic: 0.3 m edits, 1.8 m corridors, ~55 m2 initial units. No daylight/egress validation."
            : "No corridor-served segments wide enough for quick apartments.";
    PartitionUnits (plan);
    return plan;
}

Point UnitCenter (const QuickPlan& plan, const UnitSeed& seed)
{
    const auto& s = plan.segments.at (seed.segment);
    return World (plan, s.alongX ? Point { seed.along, s.across } : Point { s.across, seed.along });
}
double UnitTargetArea (double rooms)
{
    // Python's default programme targets, with 1.5R interpolated between 1R and 2R.
    if (rooms == 1)
        return 34;
    if (rooms == 1.5)
        return 41;
    if (rooms == 2)
        return 48;
    if (rooms == 3)
        return 65;
    if (rooms == 4)
        return 82;
    return 0;
}
double UnitArea (const PlanRegion& unit)
{
    if (unit.rings.empty () || !unit.rings.front ().Count ())
        return 0;
    QuickPlan frame;
    frame.origin = { unit.rings.front ().xy[0], unit.rings.front ().xy[1] };
    return std::abs (cp::Area (Paths (frame, unit.rings)));
}
uint32_t UnitColour (double rooms)
{
    return rooms == 1     ? 0x9AD1E6FFu
           : rooms == 1.5 ? 0xA7DADFFFu
           : rooms == 2   ? 0x7FC8A9FFu
           : rooms == 3   ? 0xF2D06BFFu
                          : 0xF0A35EFFu;
}
bool SetUnitRooms (QuickPlan& plan, size_t seed, double rooms)
{
    if (seed >= plan.seeds.size () || plan.dragging || UnitTargetArea (rooms) <= 0 || plan.seeds[seed].rooms == rooms)
        return false;
    plan.seeds[seed].rooms = rooms;
    PartitionUnits (plan);
    return true;
}
void PartitionUnits (QuickPlan& plan)
{
    ++plan.revision;
    plan.units.clear ();
    for (const auto& seed : plan.seeds) {
        const auto& s = plan.segments.at (seed.segment);
        double lo = s.lo, hi = s.hi;
        const UnitSeed* left = nullptr;
        const UnitSeed* right = nullptr;
        for (const auto& other : plan.seeds)
            if (other.segment == seed.segment && other.id != seed.id) {
                if (other.along < seed.along && (!left || other.along > left->along))
                    left = &other;
                else if (other.along > seed.along && (!right || other.along < right->along))
                    right = &other;
            }
        const auto cut = [] (const UnitSeed& a, const UnitSeed& b) {
            const double wa = UnitTargetArea (a.rooms), wb = UnitTargetArea (b.rooms);
            return a.along + (b.along - a.along) * wa / (wa + wb);
        };
        if (left)
            lo = cut (*left, seed);
        if (right)
            hi = cut (seed, *right);
        const auto paths = Paths (plan, s.rings);
        double c0 = 1e300, c1 = -1e300;
        for (const auto& path : paths)
            for (const auto& p : path) {
                c0 = (std::min) (c0, s.alongX ? p.y : p.x);
                c1 = (std::max) (c1, s.alongX ? p.y : p.x);
            }
        auto unit = Region (
            plan, cp::Intersect (paths, { Polygon (Strip (s.alongX, lo, hi, c0, c1)) }, cp::FillRule::NonZero, 6));
        unit.center = UnitCenter (plan, seed);
        plan.units.push_back (std::move (unit));
    }
}
bool MoveUnit (QuickPlan& plan, size_t seed, Point point)
{
    if (seed >= plan.seeds.size () || !std::isfinite (point.x) || !std::isfinite (point.y))
        return false;
    auto& selected = plan.seeds[seed];
    const auto& s = plan.segments[selected.segment];
    const double at = std::clamp (std::round (Along (plan, s, point) / kModule) * kModule, s.lo + 0.3, s.hi - 0.3);
    for (const auto& other : plan.seeds)
        if (other.segment == selected.segment && other.id != selected.id &&
            (std::abs (other.along - at) < 0.6 || (other.along - at) * (other.along - selected.along) <= 0))
            return false; // No coincidence or exchanging IDs by dragging through a neighbour.
    if (at == selected.along)
        return false;
    auto candidate = selected;
    candidate.along = at;
    if (!Hit (plan, s, UnitCenter (plan, candidate)))
        return false;
    selected.along = at;
    PartitionUnits (plan);
    return true;
}
bool AddUnit (QuickPlan& plan, Point point)
{
    if (!plan.ready || plan.seeds.size () >= kMaxUnits || UnitTargetArea (plan.newRooms) <= 0 ||
        !std::isfinite (point.x) || !std::isfinite (point.y))
        return false;
    for (size_t i = 0; i < plan.segments.size (); ++i) {
        const auto& s = plan.segments[i];
        if (!Hit (plan, s, point))
            continue;
        const double at = std::clamp (std::round (Along (plan, s, point) / kModule) * kModule, s.lo + 0.3, s.hi - 0.3);
        for (const auto& seed : plan.seeds)
            if (seed.segment == i && std::abs (seed.along - at) < 0.6)
                return false;
        if (!Hit (plan, s, UnitCenter (plan, UnitSeed { 0, i, at })))
            return false;
        plan.seeds.push_back ({ plan.nextId++, i, at, plan.newRooms });
        plan.selected = int (plan.seeds.size () - 1);
        plan.adding = false;
        PartitionUnits (plan);
        return true;
    }
    return false;
}
bool RemoveUnit (QuickPlan& plan, size_t seed)
{
    if (seed >= plan.seeds.size () || plan.dragging)
        return false;
    plan.seeds.erase (plan.seeds.begin () + seed);
    plan.selected = -1;
    PartitionUnits (plan);
    return true;
}
void CancelUnits (QuickPlan& plan)
{
    if (plan.dragging && plan.selected >= 0 && size_t (plan.selected) < plan.seeds.size ()) {
        plan.seeds[size_t (plan.selected)].along = plan.dragOriginal;
        PartitionUnits (plan);
    }
    plan.dragging = plan.adding = false;
    plan.owner = 0;
}
namespace {
int TemplateFloor (const Plan& plan, const Draft& draft, const Floor& floor)
{
    if (draft.uniqueFloors.contains (floor.story))
        return floor.story;
    const auto shape = QuickSignature (floor, {});
    for (const auto& other : plan.floors)
        if (!draft.uniqueFloors.contains (other.story) && QuickSignature (other, {}) == shape)
            return other.story;
    return floor.story;
}
} // namespace
QuickPlan& QuickFor (const Plan& plan, Draft& draft, const Floor& floor)
{
    const int story = TemplateFloor (plan, draft, floor);
    auto& quick = draft.quickPlans[story];
    auto committed = draft.points;
    if (draft.dragging && draft.selected >= 0 && size_t (draft.selected) < committed.size ())
        committed[size_t (draft.selected)] = draft.dragOriginal;
    const auto signature = QuickSignature (floor, committed);
    if (quick.signature != signature) {
        const int stage = quick.stage;
        const bool replaced = !quick.signature.empty ();
        quick = GenerateQuick (floor, committed);
        quick.stage = stage;
        if (replaced)
            quick.note += " Source outline or cores changed: local unit edits reset.";
    }
    return quick;
}
void MakeUnique (const Plan& plan, Draft& draft, const Floor& floor)
{
    if (draft.uniqueFloors.contains (floor.story))
        return;
    const int shared = TemplateFloor (plan, draft, floor);
    auto copy = QuickFor (plan, draft, floor);
    CancelUnits (copy);
    draft.uniqueFloors.insert (floor.story);
    if (shared == floor.story)
        for (const auto& other : plan.floors)
            if (!draft.uniqueFloors.contains (other.story) &&
                QuickSignature (other, {}) == QuickSignature (floor, {})) {
                draft.quickPlans[TemplateFloor (plan, draft, other)] = copy;
                break;
            }
    draft.quickPlans[floor.story] = std::move (copy);
}
void ResetQuick (const Plan& plan, Draft& draft, const Floor& floor)
{
    // Reset a unique floor rejoins its shared outline; reset a shared floor regenerates that template.
    QuickPlan shared;
    bool hasShared = false;
    if (draft.uniqueFloors.contains (floor.story))
        for (const auto& other : plan.floors)
            if (!draft.uniqueFloors.contains (other.story) &&
                QuickSignature (other, {}) == QuickSignature (floor, {})) {
                shared = QuickFor (plan, draft, other);
                hasShared = true;
                break;
            }
    const bool unique = draft.uniqueFloors.erase (floor.story) != 0;
    draft.quickPlans.erase (floor.story);
    if (!unique)
        draft.quickPlans.erase (TemplateFloor (plan, draft, floor));
    else if (hasShared)
        draft.quickPlans[TemplateFloor (plan, draft, floor)] = std::move (shared);
    QuickFor (plan, draft, floor);
}
void PreviewLayers (const Plan& plan, Draft& draft, overlaylayers::Layer& stairs, overlaylayers::Layer& units,
                    bool includeUnits)
{
    if (Conflict (plan, draft))
        return;
    const auto line = [] (overlaylayers::Layer& layer, const SliceChain& ring, double z, uint32_t rgba) {
        overlaylayers::Polyline poly;
        poly.closed = true;
        poly.rgba = rgba;
        poly.behind = overlaylayers::Behind::Show;
        for (size_t i = 0; i < ring.Count (); ++i)
            poly.points.insert (poly.points.end (), { ring.xy[i * 2], ring.xy[i * 2 + 1], z });
        layer.polylines.push_back (std::move (poly));
    };
    for (const auto& floor : plan.floors) {
        for (const auto& center : draft.points) {
            SliceChain core;
            core.closed = true;
            for (const auto& p : Polygon ({ center.x - kStairWidth / 2, center.y - kStairDepth / 2,
                                            center.x + kStairWidth / 2, center.y + kStairDepth / 2 }))
                core.xy.insert (core.xy.end (), { p.x, p.y });
            const uint32_t colour = Fits (floor, center) ? 0xFFBA00FFu : 0xE5484DFFu;
            line (stairs, core, floor.z, colour);
            if (floor.height > 0) {
                line (stairs, core, floor.z + floor.height, colour);
                for (size_t i = 0; i < core.Count (); ++i) {
                    overlaylayers::Polyline edge;
                    edge.rgba = colour;
                    edge.behind = overlaylayers::Behind::Show;
                    edge.points = { core.xy[i * 2], core.xy[i * 2 + 1], floor.z,
                                    core.xy[i * 2], core.xy[i * 2 + 1], floor.z + floor.height };
                    stairs.polylines.push_back (std::move (edge));
                }
            }
        }
        if (!includeUnits)
            continue;
        const auto& quick = QuickFor (plan, draft, floor);
        if (!quick.ready)
            continue;
        for (size_t i = 0; i < quick.units.size (); ++i) {
            const uint32_t colour = UnitColour (quick.seeds[i].rooms);
            for (const auto& ring : quick.units[i].rings)
                line (units, ring, floor.z + 0.02, colour);
        }
        for (const auto& corridor : quick.corridors)
            for (const auto& ring : corridor.rings)
                line (units, ring, floor.z + 0.02, 0xD6C49AFFu);
    }
}
} // namespace geomsrv::archviz::buildingplan
