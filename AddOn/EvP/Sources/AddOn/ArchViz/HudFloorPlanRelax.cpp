#include "ArchViz/HudBuildingPlan.hpp"
#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::buildingplan {
void BuildAreaProfile (const QuickPlan& plan, PlanRegion& segment)
{
    std::vector<std::vector<Point>> rings;
    std::vector<double> cuts { segment.lo, segment.hi };
    for (const auto& ring : segment.rings) {
        std::vector<Point> points;
        for (size_t i = 0; i < ring.Count (); ++i) {
            const double x = ring.xy[i * 2] - plan.origin.x, y = ring.xy[i * 2 + 1] - plan.origin.y;
            Point p { x * std::cos (plan.angle) + y * std::sin (plan.angle),
                      -x * std::sin (plan.angle) + y * std::cos (plan.angle) };
            if (!segment.alongX)
                std::swap (p.x, p.y);
            points.push_back (p);
            cuts.push_back (p.x);
        }
        rings.push_back (std::move (points));
    }
    std::sort (cuts.begin (), cuts.end ());
    cuts.erase (std::unique (cuts.begin (), cuts.end (), [] (double a, double b) { return std::abs (a - b) < 1e-7; }),
                cuts.end ());
    const auto width = [&] (double x) {
        std::vector<double> crossings;
        for (const auto& ring : rings)
            for (size_t i = 0; i < ring.size (); ++i) {
                const auto a = ring[i], b = ring[(i + 1) % ring.size ()];
                if ((a.x < x && b.x > x) || (b.x < x && a.x > x))
                    crossings.push_back (a.y + (b.y - a.y) * (x - a.x) / (b.x - a.x));
            }
        std::sort (crossings.begin (), crossings.end ());
        double sum = 0;
        for (size_t i = 1; i < crossings.size (); i += 2)
            sum += crossings[i] - crossings[i - 1];
        return sum;
    };
    segment.areaProfile.clear ();
    double before = 0;
    for (size_t i = 1; i < cuts.size (); ++i) {
        const double lo = cuts[i - 1], hi = cuts[i], d = hi - lo;
        const double a = width (lo + d / 3), b = width (lo + 2 * d / 3);
        const double slope = 3 * (b - a) / d, w = (std::max) (0.0, a - slope * d / 3);
        segment.areaProfile.push_back ({ lo, hi, w, slope, before });
        before += (std::max) (0.0, w * d + slope * d * d / 2);
    }
}
double AreaBefore (const PlanRegion& segment, double along)
{
    double area = 0;
    for (const auto& span : segment.areaProfile) {
        if (along < span.lo)
            break;
        const double d = std::clamp (along - span.lo, 0.0, span.hi - span.lo);
        area = span.before + span.width * d + span.slope * d * d / 2;
        if (along <= span.hi)
            break;
    }
    return area;
}
double AlongAtArea (const PlanRegion& segment, double area)
{
    if (area <= 0)
        return segment.lo;
    for (const auto& span : segment.areaProfile) {
        const double length = span.hi - span.lo;
        const double capacity = span.width * length + span.slope * length * length / 2;
        if (capacity <= 0 || area > span.before + capacity + 1e-8)
            continue;
        const double delta = std::clamp (area - span.before, 0.0, capacity);
        const double root = std::sqrt ((std::max) (0.0, span.width * span.width + 2 * span.slope * delta));
        const double denominator = span.width + root;
        const double d = denominator > 1e-12 ? 2 * delta / denominator : 0;
        return span.lo + std::clamp (d, 0.0, length);
    }
    return segment.hi;
}
bool RelaxUnits (QuickPlan& plan, int anchor, bool exactTarget, bool dragging)
{
    auto solved = plan.seeds;
    bool remainder = false;
    for (size_t s = 0; s < plan.segments.size (); ++s) {
        auto& segment = plan.segments[s];
        if (segment.areaProfile.empty ())
            BuildAreaProfile (plan, segment);
        const double total = AreaBefore (segment, segment.hi);
        std::vector<size_t> order;
        for (size_t i = 0; i < solved.size (); ++i)
            if (solved[i].segment == s)
                order.push_back (i);
        std::sort (order.begin (), order.end (), [&] (size_t a, size_t b) {
            return solved[a].along == solved[b].along ? solved[a].id < solved[b].id : solved[a].along < solved[b].along;
        });
        if (segment.endAccess && order.size () > 1) {
            plan.solveNote = "Target would create an end-access apartment without corridor access. No changes applied.";
            return false;
        }
        std::vector<double> areas;
        std::vector<bool> fixed;
        double hard = 0, soft = 0;
        size_t free = 0;
        for (size_t n = 0; n < order.size (); ++n) {
            const size_t i = order[n];
            const bool lock = solved[i].locked || (exactTarget && int (i) == anchor);
            double desire = UnitTargetArea (solved[i].rooms);
            if (dragging && !lock) {
                // Overdamped spring rest lengths: the held point pulls adjacent cuts;
                // other points are free to settle. Work is bounded, not a frame-time optimiser.
                const double lo = n ? (solved[order[n - 1]].along + solved[i].along) / 2 : segment.lo;
                const double hi =
                    n + 1 < order.size () ? (solved[i].along + solved[order[n + 1]].along) / 2 : segment.hi;
                desire = (std::max) (1.0, AreaBefore (segment, hi) - AreaBefore (segment, lo));
            }
            areas.push_back (desire);
            fixed.push_back (lock);
            if (lock)
                hard += desire;
            else {
                soft += desire;
                ++free;
            }
        }
        if (hard > total + 1e-4 || (free && total - hard < free * 1.0)) {
            plan.solveNote = "Room target cannot fit while preserving locked sizes. No changes applied.";
            return false;
        }
        // Positive spring lengths share the leftover exactly. Locked lengths never scale.
        for (size_t n = 0; n < order.size (); ++n)
            if (!fixed[n])
                areas[n] = (total - hard) * areas[n] / soft;
        double cursor = free ? 0 : (std::max) (0.0, (total - hard) / 2);
        if (!free && segment.endAccess)
            cursor = segment.endAccess > 0 ? (std::max) (0.0, total - hard) : 0;
        if (dragging && !free && anchor >= 0 && !segment.endAccess) {
            double prior = 0;
            for (size_t n = 0; n < order.size (); ++n) {
                if (int (order[n]) == anchor)
                    cursor = std::clamp (AreaBefore (segment, solved[order[n]].along) - prior - areas[n] / 2, 0.0,
                                         (std::max) (0.0, total - hard));
                prior += areas[n];
            }
        }
        remainder |= !free && total - hard > 1e-3 && !order.empty ();
        for (size_t n = 0; n < order.size (); ++n) {
            auto& seed = solved[order[n]];
            seed.lo = AlongAtArea (segment, cursor);
            seed.hi = AlongAtArea (segment, cursor + areas[n]);
            seed.along = AlongAtArea (segment, cursor + areas[n] / 2);
            cursor += areas[n];
        }
    }
    plan.seeds = std::move (solved);
    plan.solveNote = remainder ? "Locked sizes retained; remaining area is unassigned." : "";
    RebuildUnits (plan);
    return true;
}
bool SetUnitLocked (QuickPlan& plan, size_t seed, bool locked)
{
    if (seed >= plan.seeds.size () || plan.dragging || plan.seeds[seed].locked == locked)
        return false;
    const auto original = plan.seeds;
    plan.seeds[seed].locked = locked;
    if (RelaxUnits (plan, int (seed), locked))
        return true;
    plan.seeds = original;
    return false;
}
} // namespace geomsrv::archviz::buildingplan
