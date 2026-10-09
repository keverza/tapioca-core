#include "ArchViz/HudFloorPlanFrame.hpp"
#include <clipper2/clipper.triangulation.h>
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace geomsrv::archviz::buildingplan {
namespace frame {
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
    cp::PathsD triangles;
    if (cp::Triangulate (paths, 6, triangles, false) == cp::TriangulateResult::success)
        for (const auto& triangle : triangles)
            for (const auto& p : triangle)
                out.triangles.push_back (World (plan, { p.x, p.y }));
    return out;
}
} // namespace frame
namespace {
using namespace frame;
constexpr size_t kMaxBars = 64, kMaxVertices = 2048;
std::vector<SliceChain> Counted (const Floor& floor)
{
    if (floor.outlineKnown)
        return floor.outline;
    cp::PathsD parts;
    Point origin;
    if (!floor.contours.empty () && !floor.contours.front ().empty () && floor.contours.front ().front ().Count ())
        origin = { floor.contours.front ().front ().xy[0], floor.contours.front ().front ().xy[1] };
    QuickPlan local;
    local.origin = origin;
    for (const auto& source : floor.contours) {
        const auto own = cp::Union (Paths (local, source), cp::FillRule::EvenOdd, 6);
        parts.insert (parts.end (), own.begin (), own.end ());
    }
    return Region (local, cp::Union (parts, cp::FillRule::NonZero, 6)).rings;
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
double Contact (const cp::PathsD& piece, const cp::PathsD& other)
{
    // Shared boundary length: the overlap of a 5 cm band around `piece` with `other`.
    constexpr double band = 0.05;
    const auto grown = cp::InflatePaths (piece, band, cp::JoinType::Miter, cp::EndType::Polygon);
    return std::abs (cp::Area (cp::Intersect (cp::Difference (grown, piece, cp::FillRule::NonZero, 6), other,
                                              cp::FillRule::NonZero, 6))) /
           band;
}
void Components (const cp::PolyPathD& node, std::vector<cp::PathsD>& out)
{
    for (size_t i = 0; i < node.Count (); ++i) {
        const auto* outer = node.Child (i);
        cp::PathsD component { outer->Polygon () };
        for (size_t h = 0; h < outer->Count (); ++h) {
            component.push_back (outer->Child (h)->Polygon ());
            Components (*outer->Child (h), out); // islands inside the hole
        }
        out.push_back (std::move (component));
    }
}
// Boundary pieces of one segment along its axis, at most one module long.
void Edges (PlanRegion& segment, const cp::PathsD& piece, const cp::PathsD& outline, const cp::PathsD& circulation)
{
    segment.facade.clear ();
    segment.access.clear ();
    for (const auto& path : piece)
        for (size_t i = 0; i < path.size (); ++i) {
            const auto a = path[i], b = path[(i + 1) % path.size ()];
            const double length = std::hypot (b.x - a.x, b.y - a.y);
            if (length < 1e-6)
                continue;
            Point normal { (b.y - a.y) / length, (a.x - b.x) / length };
            const Point middle { (a.x + b.x) / 2, (a.y + b.y) / 2 };
            if (Inside (piece, { middle.x + normal.x * 0.02, middle.y + normal.y * 0.02 }))
                normal = { -normal.x, -normal.y };
            const uint8_t side =
                std::abs (normal.x) >= std::abs (normal.y) ? (normal.x > 0 ? 0 : 2) : (normal.y > 0 ? 1 : 3);
            const size_t count = (std::max) (size_t (1), size_t (std::ceil (length / kModule - 1e-9)));
            for (size_t k = 0; k < count; ++k) {
                const Point p0 { a.x + (b.x - a.x) * double (k) / count, a.y + (b.y - a.y) * double (k) / count };
                const Point p1 { a.x + (b.x - a.x) * double (k + 1) / count,
                                 a.y + (b.y - a.y) * double (k + 1) / count };
                const Point m { (p0.x + p1.x) / 2, (p0.y + p1.y) / 2 };
                const Point out { m.x + normal.x * 0.05, m.y + normal.y * 0.05 };
                const bool facade = !Inside (outline, out);
                if (!facade && !Inside (circulation, out))
                    continue;
                const double a0 = segment.alongX ? (std::min) (p0.x, p1.x) : (std::min) (p0.y, p1.y);
                const double a1 = segment.alongX ? (std::max) (p0.x, p1.x) : (std::max) (p0.y, p1.y);
                PlanRegion::Edge edge { a0, a1, length / count, segment.alongX ? m.y : m.x, side };
                (facade ? segment.facade : segment.access).push_back (edge);
            }
        }
}
} // namespace

std::string QuickSignature (const Floor& floor, const std::vector<Core>& stairs,
                            const floorprogramme::Programme* programme)
{
    // Floor height/index are NOT template identity. Canonical world rings at micrometre precision
    // share across input order, start vertex and winding, but never across translated buildings.
    std::ostringstream key;
    key << std::fixed << std::setprecision (6);
    const auto cores = [&] {
        for (const auto& core : stairs)
            key << core.center.x << ',' << core.center.y << ',' << core.width << 'x' << core.depth << ';';
        if (programme)
            key << floorprogramme::Key (*programme);
    };
    if (!floor.outlineKey.empty ()) {
        key << floor.outlineKey;
        cores ();
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
    cores ();
    return key.str ();
}

QuickPlan GenerateQuick (const Floor& floor, const std::vector<Core>& stairs,
                         const floorprogramme::Programme& programme, double angle)
{
    QuickPlan plan;
    plan.programme = floorprogramme::Valid (programme) ? programme : floorprogramme::Default ();
    plan.newType = size_t (std::max_element (plan.programme.types.begin (), plan.programme.types.end (),
                                             [] (const auto& a, const auto& b) { return a.share < b.share; }) -
                           plan.programme.types.begin ());
    plan.cores = stairs;
    plan.signature = QuickSignature (floor, stairs, &plan.programme);
    plan.outlineSignature = QuickSignature (floor, {});
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
    if (std::isfinite (angle))
        plan.angle = angle; // the building's frame, shared by all its floors and cores
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
    for (const auto& core : stairs) {
        if (!Fits (floor, core, plan.angle)) {
            plan.note = "Quick plan refused: every proposed stair footprint must fit this floor.";
            return plan;
        }
        const auto rect = Polygon (CoreRect (plan, core));
        if (std::abs (cp::Area (cp::Intersect (cores, { rect }, cp::FillRule::NonZero, 6))) > 1e-6) {
            plan.note = "Quick plan refused: proposed stair footprints overlap.";
            return plan;
        }
        cores.push_back (rect);
    }
    cp::PathsD circulation;
    std::vector<Rect> spines;
    std::vector<Rect> spineBars;
    std::vector<double> crossOf (bars.size (), std::numeric_limits<double>::quiet_NaN ());
    for (const auto& b : bars) {
        const bool axis = b.x1 - b.x0 >= b.y1 - b.y0;
        const double lo = axis ? b.x0 : b.y0, hi = axis ? b.x1 : b.y1;
        const double c0 = axis ? b.y0 : b.x0, c1 = axis ? b.y1 : b.x1;
        if (c1 - c0 < kCorridor + kMinWidth + 0.6 || hi - lo <= 2 * kMinWidth)
            continue;
        double cross = (c0 + c1) / 2;
        if (c1 - c0 < 2 * 5.4 + kCorridor)
            cross = c0 + 0.6 + kCorridor / 2; // Shallow bar: keep circulation off the facade.
        const auto spine = Strip (axis, lo + kMinWidth, hi - kMinWidth, cross - kCorridor / 2, cross + kCorridor / 2);
        crossOf[size_t (&b - bars.data ())] = cross;
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
    // Connect each core to the nearest spine from the middle of its facing side; clipping
    // alone is NOT connectivity validation.
    for (const auto& core : stairs) {
        const auto q = Local (plan, core.center);
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
    // Both external facades and courtyard boundaries retain a positive apartment-side buffer.
    const auto interior = cp::InflatePaths (outline, -0.3, cp::JoinType::Miter, cp::EndType::Polygon);
    circulation = cp::Intersect (cp::Union (circulation, cp::FillRule::NonZero, 6), interior, cp::FillRule::NonZero, 6);
    const auto reserved = cp::Union (circulation, cores, cp::FillRule::NonZero, 6);
    // Reject a corridor component that has no core. No apparently served apartments across a courtyard/island.
    for (const auto& component : reserved)
        if (cp::Area (component) > 0 &&
            std::abs (cp::Area (cp::Intersect ({ component }, cores, cp::FillRule::NonZero, 6))) < 1e-6) {
            plan.note = "Quick S4 refused: a corridor/island is disconnected from all proposed stairs.";
            return plan;
        }
    auto corridors = cp::Difference (circulation, cores, cp::FillRule::NonZero, 6);
    std::vector<cp::PathsD> pieces, loose;
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
            if (cuts[c] - cuts[c - 1] < kMinWidth - 1e-5)
                continue; // a narrow strip joins a neighbouring flat below
            const auto parts = cp::Intersect (
                remaining,
                { Polygon (Strip (axis, cuts[c - 1], cuts[c], axis ? rect.y0 : rect.x0, axis ? rect.y1 : rect.x1)) },
                cp::FillRule::NonZero, 6);
            for (const auto& path : parts) {
                if (cp::Area (path) <= 1e-6)
                    continue;
                // Require actual shared corridor boundary, not just proximity or a stair marker.
                const auto expanded =
                    cp::InflatePaths (cp::PathsD { path }, 0.01, cp::JoinType::Miter, cp::EndType::Polygon);
                if (std::abs (cp::Area (cp::Intersect (expanded, circulation, cp::FillRule::NonZero, 6))) < 1e-6)
                    continue;
                PlanRegion segment;
                segment.alongX = axis;
                segment.lo = cuts[c - 1];
                segment.hi = cuts[c];
                double a = 1e300, z = -1e300;
                for (const auto& p : path) {
                    a = (std::min) (a, axis ? p.y : p.x);
                    z = (std::max) (z, axis ? p.y : p.x);
                }
                const auto touchesEnd = [&] (double at) {
                    return std::abs (cp::Area (cp::Intersect (
                               expanded,
                               cp::Intersect (circulation, { Polygon (Strip (axis, at - 0.02, at + 0.02, a, z)) },
                                              cp::FillRule::NonZero, 6),
                               cp::FillRule::NonZero, 6))) > 1e-6;
                };
                const bool low = touchesEnd (segment.lo), high = touchesEnd (segment.hi);
                const auto middle = cp::Intersect (
                    circulation, { Polygon (Strip (axis, segment.lo + 0.03, segment.hi - 0.03, a - 0.02, z + 0.02)) },
                    cp::FillRule::NonZero, 6);
                const bool sideAccess =
                    std::abs (cp::Area (cp::Intersect (expanded, middle, cp::FillRule::NonZero, 6))) > 1e-6;
                if (low != high && !sideAccess)
                    segment.endAccess = high ? 1 : -1;
                const double cross = crossOf[b];
                if (segment.endAccess && std::isfinite (cross) && a + 1.0 < cross - kCorridor / 2 &&
                    cross + kCorridor / 2 < z - 1.0) {
                    // A bar end across its double-loaded corridor: each half joins its band as
                    // the corner flat (private generator band caps), not one through-flat.
                    for (const auto& half : { Strip (axis, segment.lo, segment.hi, a, cross),
                                              Strip (axis, segment.lo, segment.hi, cross, z) }) {
                        const auto part = cp::Intersect ({ path }, { Polygon (half) }, cp::FillRule::NonZero, 6);
                        if (std::abs (cp::Area (part)) > 1e-6)
                            loose.push_back (part);
                    }
                    continue;
                }
                plan.segments.push_back (std::move (segment));
                pieces.push_back ({ path });
            }
        }
    }
    if (plan.segments.size () > kMaxUnits) {
        plan.note = "Quick apartment division exceeds 128 segments; no partial scheme shown.";
        plan.segments.clear ();
        return plan;
    }
    // No empty floor: every leftover joins the flat band it shares most boundary with; strips
    // reached only by circulation join the circulation; anything else is reported as empty.
    {
        const auto reach = cp::Union (circulation, cores, cp::FillRule::NonZero, 6);
        cp::PathsD used = reach;
        for (const auto* group : { &pieces, &loose })
            for (const auto& piece : *group)
                used.insert (used.end (), piece.begin (), piece.end ());
        used = cp::Union (used, cp::FillRule::NonZero, 6);
        cp::PolyTreeD tree;
        cp::BooleanOp (cp::ClipType::Difference, cp::FillRule::NonZero, outline, used, tree, 6);
        std::vector<cp::PathsD> leftovers = loose;
        Components (tree, leftovers);
        // Several passes: a strip may reach a band only through a piece attached before it.
        cp::PathsD common, lost;
        std::vector<bool> placed (leftovers.size (), false);
        for (bool progress = true; progress;) {
            progress = false;
            for (size_t l = 0; l < leftovers.size (); ++l) {
                if (placed[l] || std::abs (cp::Area (leftovers[l])) < 1e-4)
                    continue;
                size_t best = pieces.size ();
                double contact = 0.3;
                for (size_t s = 0; s < pieces.size (); ++s) {
                    const double c = Contact (leftovers[l], pieces[s]);
                    if (c > contact + 1e-9) {
                        contact = c;
                        best = s;
                    }
                }
                if (best == pieces.size ())
                    continue;
                pieces[best] = cp::Union (pieces[best], leftovers[l], cp::FillRule::NonZero, 6);
                auto& segment = plan.segments[best];
                for (const auto& path : leftovers[l])
                    for (const auto& p : path) {
                        segment.lo = (std::min) (segment.lo, segment.alongX ? p.x : p.y);
                        segment.hi = (std::max) (segment.hi, segment.alongX ? p.x : p.y);
                    }
                placed[l] = progress = true;
            }
        }
        for (size_t l = 0; l < leftovers.size (); ++l) {
            if (placed[l] || std::abs (cp::Area (leftovers[l])) < 1e-4)
                continue;
            auto& target = Contact (leftovers[l], reach) >= 0.3 ? common : lost;
            target.insert (target.end (), leftovers[l].begin (), leftovers[l].end ());
        }
        if (!common.empty ())
            corridors = cp::Union (corridors, common, cp::FillRule::NonZero, 6);
        if (!lost.empty ())
            plan.unassigned.push_back (Region (plan, lost));
    }
    plan.corridors.push_back (Region (plan, corridors));
    const auto served = cp::Union (corridors, cores, cp::FillRule::NonZero, 6);
    for (size_t s = 0; s < plan.segments.size (); ++s) {
        auto& segment = plan.segments[s];
        const auto region = Region (plan, pieces[s]);
        segment.rings = region.rings;
        segment.triangles = region.triangles;
        double a = 1e300, z = -1e300;
        for (const auto& path : pieces[s])
            for (const auto& p : path) {
                a = (std::min) (a, segment.alongX ? p.y : p.x);
                z = (std::max) (z, segment.alongX ? p.y : p.x);
            }
        segment.across = (a + z) / 2;
        segment.center = World (plan, segment.alongX ? Point { (segment.lo + segment.hi) / 2, segment.across }
                                                     : Point { segment.across, (segment.lo + segment.hi) / 2 });
        Edges (segment, pieces[s], outline, served);
        BuildAreaProfile (plan, segment);
    }
    plan.ready = !plan.segments.empty ();
    if (!plan.ready) {
        plan.note = "No corridor-served segments wide enough for quick apartments.";
        return plan;
    }
    Fill (plan);
    if (plan.seeds.empty ()) {
        plan.ready = false;
        plan.note = "No programme flat fits the corridor-served segments.";
        return plan;
    }
    plan.egress = AnalyseEgress (plan);
    plan.score = Score (plan, plan.seeds);
    OptimiseUnits (plan, 300);
    plan.note = "Programme fill on the building frame: net areas against the programme ranges, larger flats on "
                "corners. Optimise may retype, add or remove unlocked flats.";
    return plan;
}

Point UnitCenter (const QuickPlan& plan, const UnitSeed& seed)
{
    const auto& s = plan.segments.at (seed.segment);
    std::vector<double> crossings;
    for (const auto& path : Paths (plan, s.rings))
        for (size_t i = 0; i < path.size (); ++i) {
            auto a = path[i], b = path[(i + 1) % path.size ()];
            if (!s.alongX) {
                std::swap (a.x, a.y);
                std::swap (b.x, b.y);
            }
            if ((a.x < seed.along && b.x > seed.along) || (b.x < seed.along && a.x > seed.along))
                crossings.push_back (a.y + (b.y - a.y) * (seed.along - a.x) / (b.x - a.x));
        }
    std::sort (crossings.begin (), crossings.end ());
    double across = s.across, width = -1;
    for (size_t i = 1; i < crossings.size (); i += 2)
        if (crossings[i] - crossings[i - 1] > width) {
            width = crossings[i] - crossings[i - 1];
            across = (crossings[i] + crossings[i - 1]) / 2;
        }
    return World (plan, s.alongX ? Point { seed.along, across } : Point { across, seed.along });
}
double TargetArea (const QuickPlan& plan, const UnitSeed& seed)
{
    return seed.type < plan.programme.types.size () ? floorprogramme::Target (plan.programme.types[seed.type])
                                                    : seed.target;
}
double Rooms (const QuickPlan& plan, const UnitSeed& seed)
{
    return seed.type < plan.programme.types.size () ? plan.programme.types[seed.type].rooms : 0;
}
std::string TypeName (const QuickPlan& plan, const UnitSeed& seed)
{
    return floorprogramme::Name (plan.programme, seed.type);
}
double UnitArea (const PlanRegion& unit)
{
    if (unit.rings.empty () || !unit.rings.front ().Count ())
        return 0;
    QuickPlan local;
    local.origin = { unit.rings.front ().xy[0], unit.rings.front ().xy[1] };
    return std::abs (cp::Area (Paths (local, unit.rings)));
}
uint32_t UnitColour (double rooms)
{
    return floorprogramme::Colour (rooms);
}
bool SetUnitType (QuickPlan& plan, size_t seed, size_t type)
{
    if (seed >= plan.seeds.size () || plan.dragging || type >= plan.programme.types.size () ||
        plan.seeds[seed].type == type)
        return false;
    const auto original = plan.seeds;
    plan.seeds[seed].type = type;
    plan.seeds[seed].target = floorprogramme::Target (plan.programme.types[type]);
    if (RelaxUnits (plan, int (seed), true))
        return true;
    plan.seeds = original;
    return false;
}
void PartitionUnits (QuickPlan& plan)
{
    RelaxUnits (plan);
}
void RebuildUnits (QuickPlan& plan)
{
    ++plan.revision;
    plan.units.clear ();
    for (const auto& seed : plan.seeds) {
        const auto& s = plan.segments.at (seed.segment);
        const double lo = seed.lo, hi = seed.hi;
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
    plan.score = Score (plan, plan.seeds);
}
bool MoveUnit (QuickPlan& plan, size_t seed, Point point)
{
    if (seed >= plan.seeds.size () || !std::isfinite (point.x) || !std::isfinite (point.y))
        return false;
    auto& selected = plan.seeds[seed];
    const auto& s = plan.segments[selected.segment];
    const double at = std::clamp (std::round (Along (plan, s, point) / kModule) * kModule, s.lo + 0.3, s.hi - 0.3);
    if (at == selected.along)
        return false;
    auto candidate = selected;
    candidate.along = at;
    if (!Hit (plan, s, UnitCenter (plan, candidate)))
        return false;
    if (plan.dragging && plan.dragSeeds.empty ()) {
        plan.dragSeeds = plan.seeds;
        plan.dragSeeds[seed].along = plan.dragOriginal;
        plan.dragUnits = plan.units;
    }
    const auto original = plan.seeds;
    selected.along = at;
    if (RelaxUnits (plan, int (seed), false, true))
        return true;
    plan.seeds = original;
    return false;
}
bool AddUnit (QuickPlan& plan, Point point)
{
    if (!plan.ready || plan.seeds.size () >= kMaxUnits || plan.newType >= plan.programme.types.size () ||
        !std::isfinite (point.x) || !std::isfinite (point.y))
        return false;
    for (size_t i = 0; i < plan.segments.size (); ++i) {
        const auto& s = plan.segments[i];
        if (!Hit (plan, s, point))
            continue;
        if (s.endAccess && std::any_of (plan.seeds.begin (), plan.seeds.end (),
                                        [&] (const UnitSeed& seed) { return seed.segment == i; })) {
            plan.solveNote = "Cannot split an end-access apartment along the band without losing corridor access.";
            return false;
        }
        const double at = std::clamp (std::round (Along (plan, s, point) / kModule) * kModule, s.lo + 0.3, s.hi - 0.3);
        for (const auto& seed : plan.seeds)
            if (seed.segment == i && std::abs (seed.along - at) < 0.6)
                return false;
        UnitSeed added { plan.nextId, i, at, plan.newType,
                         floorprogramme::Target (plan.programme.types[plan.newType]) };
        if (!Hit (plan, s, UnitCenter (plan, added)))
            return false;
        const auto original = plan.seeds;
        plan.seeds.push_back (added);
        if (!RelaxUnits (plan, int (plan.seeds.size () - 1), true)) {
            plan.seeds = original;
            return false;
        }
        ++plan.nextId;
        plan.selected = int (plan.seeds.size () - 1);
        plan.adding = false;
        return true;
    }
    return false;
}
bool RemoveUnit (QuickPlan& plan, size_t seed)
{
    if (seed >= plan.seeds.size () || plan.dragging)
        return false;
    const size_t segment = plan.seeds[seed].segment;
    if (std::count_if (plan.seeds.begin (), plan.seeds.end (),
                       [&] (const UnitSeed& other) { return other.segment == segment; }) <= 1) {
        plan.solveNote = "A band keeps at least one flat (no empty floor): change its type instead.";
        return false;
    }
    const auto original = plan.seeds;
    plan.seeds.erase (plan.seeds.begin () + std::ptrdiff_t (seed));
    if (!RelaxUnits (plan)) {
        plan.seeds = original;
        return false;
    }
    plan.selected = -1;
    return true;
}
void CancelUnits (QuickPlan& plan)
{
    if (plan.dragging && plan.selected >= 0 && size_t (plan.selected) < plan.seeds.size ()) {
        if (!plan.dragSeeds.empty ()) {
            plan.seeds = std::move (plan.dragSeeds);
            plan.units = std::move (plan.dragUnits);
            ++plan.revision;
        }
        else {
            plan.seeds[size_t (plan.selected)].along = plan.dragOriginal;
            PartitionUnits (plan);
        }
    }
    plan.dragging = plan.adding = false;
    plan.owner = 0;
    plan.dragSeeds.clear ();
    plan.dragUnits.clear ();
}
} // namespace geomsrv::archviz::buildingplan
