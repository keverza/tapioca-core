#include "ArchViz/HudBuildingPlan.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace geomsrv::archviz::buildingplan {
bool TransferUnits (const QuickPlan& old, QuickPlan& next, int anchor)
{
    if (!next.ready || next.segments.empty ())
        return false;
    next.seeds = old.seeds;
    next.selected = old.selected;
    next.stage = old.stage;
    next.nextId = old.nextId;
    next.newRooms = old.newRooms;
    for (auto& seed : next.seeds) {
        const auto world = UnitCenter (old, seed);
        const double x = world.x - next.origin.x, y = world.y - next.origin.y;
        const Point local { x * std::cos (next.angle) + y * std::sin (next.angle),
                            -x * std::sin (next.angle) + y * std::cos (next.angle) };
        double best = (std::numeric_limits<double>::max) ();
        size_t chosen = 0;
        double along = 0;
        for (size_t i = 0; i < next.segments.size (); ++i) {
            const auto& s = next.segments[i];
            const double at = std::clamp (s.alongX ? local.x : local.y, s.lo + 1e-5, s.hi - 1e-5);
            const double across = s.alongX ? local.y : local.x;
            const double distance = std::hypot (at - (s.alongX ? local.x : local.y), across - s.across);
            if (distance < best) {
                best = distance;
                chosen = i;
                along = at;
            }
        }
        seed.segment = chosen;
        seed.along = along;
    }
    return RelaxUnits (next, anchor, anchor >= 0);
}
bool ChangeUnitTarget (const Plan& plan, Draft& draft, const Floor& floor, size_t seed, double rooms, bool locked)
{
    auto& quick = QuickFor (plan, draft, floor);
    if (Conflict (plan, draft) || draft.dragging || quick.dragging || !quick.ready || seed >= quick.seeds.size () ||
        UnitTargetArea (rooms) <= 0)
        return false;
    const auto before = quick;
    quick.seeds[seed].rooms = rooms;
    quick.seeds[seed].locked = locked;
    if (RelaxUnits (quick, int (seed), true))
        return true;
    const auto requested = quick;
    quick = before;
    // A bounded local core search runs only on infeasible room edits, never per animation frame.
    // Publish all floors and core points together, or retain the complete previous draft.
    constexpr double steps[] = { 0.6, 1.2, 2.4 };
    constexpr Point directions[] = { { 1, 0 },
                                     { -1, 0 },
                                     { 0, 1 },
                                     { 0, -1 },
                                     { 0.7071, 0.7071 },
                                     { -0.7071, 0.7071 },
                                     { 0.7071, -0.7071 },
                                     { -0.7071, -0.7071 } };
    size_t attempts = 0;
    for (double step : steps)
        for (size_t core = 0; core < draft.points.size (); ++core)
            for (const auto direction : directions) {
                if (++attempts > 48)
                    break;
                auto points = draft.points;
                points[core].x += step * direction.x;
                points[core].y += step * direction.y;
                if (std::any_of (plan.floors.begin (), plan.floors.end (),
                                 [&] (const Floor& f) { return !Fits (f, points[core]); }))
                    continue;
                auto next = GenerateQuick (floor, points);
                if (!TransferUnits (requested, next, int (seed)))
                    continue;
                auto designs = draft.quickPlans;
                bool feasible = true;
                for (const auto& f : plan.floors) {
                    auto& current = QuickFor (plan, draft, f);
                    if (&current == &quick)
                        continue;
                    auto replacement = GenerateQuick (f, points);
                    if (!TransferUnits (current, replacement)) {
                        feasible = false;
                        break;
                    }
                    for (const auto& [story, value] : draft.quickPlans)
                        if (&value == &current) {
                            replacement.revision = current.revision + 1;
                            designs[story] = std::move (replacement);
                            break;
                        }
                }
                if (!feasible)
                    continue;
                for (const auto& [story, value] : draft.quickPlans)
                    if (&value == &quick) {
                        next.revision = before.revision + 1;
                        next.solveNote =
                            "Target met by moving a proposed core. Save stairwells to persist core changes.";
                        designs[story] = std::move (next);
                        break;
                    }
                for (auto& [story, design] : designs)
                    draft.quickPlans[story] = std::move (design);
                draft.points = std::move (points);
                draft.changed = true;
                return true;
            }
    quick.solveNote = "Target cannot fit with current locks and a bounded core relocation. No changes applied.";
    return false;
}
} // namespace geomsrv::archviz::buildingplan
