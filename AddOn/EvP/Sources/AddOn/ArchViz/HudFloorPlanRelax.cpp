#include "ArchViz/HudFloorPlanFrame.hpp"
#include <algorithm>

namespace geomsrv::archviz::buildingplan {
namespace {
constexpr double kAspect = 1.2; // facade length that counts as an aspect (one window)
constexpr int kNetIterations = 8;
// The facade or entrance length of `edge` that belongs to the flat between `lo` and `hi`.
double Overlap (const PlanRegion::Edge& edge, bool alongX, double lo, double hi)
{
    if (edge.a1 - edge.a0 > 1e-6) {
        const double overlap = (std::min) (edge.a1, hi) - (std::max) (edge.a0, lo);
        return overlap > 0 ? overlap * edge.length / (edge.a1 - edge.a0) : 0;
    }
    // A piece across the axis belongs to the flat on its inner side.
    const uint8_t forward = alongX ? 0 : 1, backward = alongX ? 2 : 3;
    const double at = edge.a0;
    if (edge.side == forward)
        return at > lo + 1e-6 && at <= hi + 1e-6 ? edge.length : 0;
    if (edge.side == backward)
        return at >= lo - 1e-6 && at < hi - 1e-6 ? edge.length : 0;
    return at >= lo - 1e-6 && at <= hi + 1e-6 ? edge.length : 0;
}
// Net = gross - one wall between flats across the band - the facade wall along the outline.
double Deduction (const PlanRegion& segment, double lo, double hi)
{
    const double gross = AreaBefore (segment, hi) - AreaBefore (segment, lo);
    const double depth = hi - lo > 1e-6 ? gross / (hi - lo) : 0;
    return floorprogramme::kWall * depth + floorprogramme::kFacade * frame::FacadeLength (segment, lo, hi);
}
} // namespace
double frame::FacadeLength (const PlanRegion& segment, double lo, double hi)
{
    double length = 0;
    for (const auto& edge : segment.facade)
        length += Overlap (edge, segment.alongX, lo, hi);
    return length;
}
void BuildAreaProfile (const QuickPlan& plan, PlanRegion& segment)
{
    std::vector<std::vector<Point>> rings;
    std::vector<double> cuts { segment.lo, segment.hi };
    for (const auto& ring : segment.rings) {
        std::vector<Point> points;
        for (size_t i = 0; i < ring.Count (); ++i) {
            auto p = frame::Local (plan, { ring.xy[i * 2], ring.xy[i * 2 + 1] });
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
bool SolveCuts (const QuickPlan& plan, std::vector<UnitSeed>& seeds, std::string& note, int anchor, bool exactTarget,
                bool dragging)
{
    auto solved = seeds;
    for (size_t s = 0; s < plan.segments.size (); ++s) {
        const auto& segment = plan.segments[s];
        const double total = AreaBefore (segment, segment.hi);
        std::vector<size_t> order;
        for (size_t i = 0; i < solved.size (); ++i)
            if (solved[i].segment == s)
                order.push_back (i);
        if (order.empty ())
            continue;
        std::sort (order.begin (), order.end (), [&] (size_t a, size_t b) {
            return solved[a].along == solved[b].along ? solved[a].id < solved[b].id : solved[a].along < solved[b].along;
        });
        if (segment.endAccess && order.size () > 1) {
            note = "Target would create an end-access apartment without corridor access. No changes applied.";
            return false;
        }
        const size_t n = order.size ();
        // First estimate of each flat's span: halfway to its neighbours' centres. While a
        // point is held, those spans are also the free flats' spring rest areas.
        std::vector<double> lo (n), hi (n), spring (n);
        for (size_t k = 0; k < n; ++k) {
            lo[k] = k ? (solved[order[k - 1]].along + solved[order[k]].along) / 2 : segment.lo;
            hi[k] = k + 1 < n ? (solved[order[k]].along + solved[order[k + 1]].along) / 2 : segment.hi;
            spring[k] = (std::max) (1.0, AreaBefore (segment, hi[k]) - AreaBefore (segment, lo[k]));
        }
        // Net targets become gross lengths through the walls of the current spans; the
        // fixed point converges in a few rounds (the facade wall is a small share of depth).
        for (int round = 0; round < kNetIterations; ++round) {
            std::vector<double> areas (n);
            std::vector<bool> fixed (n);
            double hard = 0, soft = 0;
            size_t free = 0;
            for (size_t k = 0; k < n; ++k) {
                const size_t i = order[k];
                const bool lock = solved[i].locked || (exactTarget && int (i) == anchor);
                // Overdamped spring rest lengths while dragging: the held point pulls adjacent
                // cuts; other points are free to settle. Bounded work, not a frame-time optimiser.
                const double desire =
                    dragging && !lock ? spring[k] : TargetArea (plan, solved[i]) + Deduction (segment, lo[k], hi[k]);
                areas[k] = desire;
                fixed[k] = lock;
                if (lock)
                    hard += desire;
                else {
                    soft += desire;
                    ++free;
                }
            }
            double minimum = 0;
            std::vector<double> floors (n, 0);
            for (size_t k = 0; k < n; ++k)
                if (!fixed[k]) {
                    floors[k] = kMinUnitArea + 0.001 + Deduction (segment, lo[k], hi[k]);
                    minimum += floors[k];
                }
            if (hard > total + 1e-4 || (free && total - hard < minimum - 0.01) ||
                (!free && std::abs (total - hard) > 0.05)) {
                note = "Room target cannot fit while preserving locked sizes. No changes applied.";
                return false;
            }
            // Water-fill free lengths above the 25 m2 net floor; exact locks never scale.
            double left = total - hard;
            for (size_t pass = 0; pass <= n && soft > 1e-9; ++pass) {
                bool clamped = false;
                for (size_t k = 0; k < n; ++k)
                    if (!fixed[k] && left * areas[k] / soft < floors[k]) {
                        soft -= areas[k];
                        areas[k] = floors[k];
                        left -= areas[k];
                        fixed[k] = true;
                        clamped = true;
                    }
                if (!clamped) {
                    for (size_t k = 0; k < n; ++k)
                        if (!fixed[k])
                            areas[k] *= left / soft;
                    break;
                }
            }
            double cursor = 0;
            for (size_t k = 0; k < n; ++k) {
                auto& seed = solved[order[k]];
                lo[k] = seed.lo = AlongAtArea (segment, cursor);
                hi[k] = seed.hi = AlongAtArea (segment, cursor + areas[k]);
                seed.along = AlongAtArea (segment, cursor + areas[k] / 2);
                cursor += areas[k];
            }
        }
    }
    for (const auto& seed : solved) {
        const auto traits = Traits (plan, seed);
        if (traits.net < kMinUnitArea - 1e-4 || traits.depth < kMinUnitDepth - 0.01 || seed.hi - seed.lo < 2.6 - 0.01 ||
            traits.facade < 1.2 - 0.01 || traits.access < 0.9 - 0.01) {
            note = "Unit must have at least 25 m2 net, habitable depth/width, a facade and corridor access. No changes "
                   "applied.";
            return false;
        }
    }
    seeds = std::move (solved);
    return true;
}
bool RelaxUnits (QuickPlan& plan, int anchor, bool exactTarget, bool dragging)
{
    std::string note;
    if (!SolveCuts (plan, plan.seeds, note, anchor, exactTarget, dragging)) {
        plan.solveNote = note;
        return false;
    }
    plan.solveNote.clear ();
    RebuildUnits (plan);
    return true;
}
UnitTraits Traits (const QuickPlan& plan, const UnitSeed& seed)
{
    UnitTraits out;
    if (seed.segment >= plan.segments.size ())
        return out;
    const auto& segment = plan.segments[seed.segment];
    const double lo = seed.lo, hi = (std::max) (seed.lo, seed.hi);
    out.gross = AreaBefore (segment, hi) - AreaBefore (segment, lo);
    out.depth = hi - lo > 1e-6 ? out.gross / (hi - lo) : 0;
    double sides[4] = {};
    double line[4] = {};
    bool lined[4] = {}, straight = true;
    for (const auto& edge : segment.facade) {
        const double length = Overlap (edge, segment.alongX, lo, hi);
        if (length <= 1e-9)
            continue;
        out.facade += length;
        sides[edge.side] += length;
        const double key = edge.a1 - edge.a0 > 1e-6 ? edge.across : edge.a0;
        if (!lined[edge.side]) {
            lined[edge.side] = true;
            line[edge.side] = key;
        }
        else if (std::abs (line[edge.side] - key) > 0.05)
            straight = false;
    }
    for (const auto& edge : segment.access)
        out.access += Overlap (edge, segment.alongX, lo, hi);
    out.net = (std::max) (0.0, out.gross - floorprogramme::kWall * out.depth - floorprogramme::kFacade * out.facade);
    for (uint8_t side = 0; side < 4; ++side)
        if (sides[side] >= kAspect)
            out.sides |= uint8_t (1u << side);
    const uint8_t along = segment.alongX ? 0b1010 : 0b0101; // long sides: facades parallel to the axis
    const uint8_t ends = uint8_t (~along & 0b1111);
    const bool corner = (out.sides & along) && (out.sides & ends);
    const bool through = (out.sides & along) == along;
    if (corner)
        out.traits |= kCorner;
    if (corner || through)
        out.traits |= kDualAspect;
    int faces = 0;
    for (uint8_t side = 0; side < 4; ++side)
        faces += sides[side] > 0.3;
    if (straight && faces == 1 && out.facade >= kAspect)
        out.traits |= kStraightFacade;
    return out;
}
bool SetUnitLocked (QuickPlan& plan, size_t seed, bool locked)
{
    if (seed >= plan.seeds.size () || plan.dragging || plan.seeds[seed].locked == locked)
        return false;
    const auto original = plan.seeds;
    plan.seeds[seed].locked = locked;
    plan.seeds[seed].keep = locked ? Traits (plan, plan.seeds[seed]).traits : uint8_t (0);
    if (RelaxUnits (plan, int (seed), locked))
        return true;
    plan.seeds = original;
    return false;
}
bool SetUnitKeep (QuickPlan& plan, size_t seed, uint8_t keep)
{
    if (seed >= plan.seeds.size () || !plan.seeds[seed].locked || plan.seeds[seed].keep == keep)
        return false;
    plan.seeds[seed].keep = keep & (kCorner | kDualAspect | kStraightFacade);
    plan.score = Score (plan, plan.seeds);
    ++plan.revision;
    return true;
}
} // namespace geomsrv::archviz::buildingplan
