#include "ArchViz/HudFloorPlanFrame.hpp"
#include <algorithm>
#include <limits>

namespace geomsrv::archviz::buildingplan {
namespace {
// The segment of `next` nearest the world point, and the point's position along it.
size_t Nearest (const QuickPlan& next, Point world, double& along)
{
    const auto local = frame::Local (next, world);
    double best = (std::numeric_limits<double>::max) ();
    size_t chosen = 0;
    for (size_t i = 0; i < next.segments.size (); ++i) {
        const auto& s = next.segments[i];
        const double at = std::clamp (s.alongX ? local.x : local.y, s.lo + 1e-5, s.hi - 1e-5);
        const double across = s.alongX ? local.y : local.x;
        const bool inside = frame::Inside (frame::Paths (next, s.rings), local);
        const double distance = inside ? -1 : std::hypot (at - (s.alongX ? local.x : local.y), across - s.across);
        if (distance < best) {
            best = distance;
            chosen = i;
            along = at;
        }
    }
    return chosen;
}
// A seed's type in `next`'s programme: the same type when the programme is unchanged.
void Remap (const QuickPlan& old, const QuickPlan& next, UnitSeed& seed)
{
    if (old.programme == next.programme && seed.type < next.programme.types.size ())
        return;
    seed.type = floorprogramme::Nearest (next.programme, seed.target);
    seed.target = floorprogramme::Target (next.programme.types[seed.type]);
}
// Moves of one core: four directions of the building frame at four distances.
std::vector<Point> Steps (double angle)
{
    std::vector<Point> out;
    const Point u { std::cos (angle), std::sin (angle) }, v { -std::sin (angle), std::cos (angle) };
    for (double step : { 1.2, 2.4, 4.8, 9.6 })
        for (const auto& d : { u, Point { -u.x, -u.y }, v, Point { -v.x, -v.y } })
            out.push_back ({ d.x * step, d.y * step });
    return out;
}
} // namespace
bool TransferUnits (const QuickPlan& old, QuickPlan& next, int anchor)
{
    if (!next.ready || next.segments.empty ())
        return false;
    next.seeds = old.seeds;
    next.selected = old.selected;
    next.stage = old.stage;
    next.nextId = (std::max) (next.nextId, old.nextId);
    next.newType = (std::min) (old.newType, next.programme.types.size () - 1);
    for (auto& seed : next.seeds) {
        double along = 0;
        seed.segment = Nearest (next, UnitCenter (old, seed), along);
        seed.along = along;
        Remap (old, next, seed);
    }
    return RelaxUnits (next, anchor, anchor >= 0);
}
bool KeepLocked (const QuickPlan& old, QuickPlan& next)
{
    if (!next.ready || next.segments.empty ())
        return false;
    std::vector<UnitSeed> locked;
    for (const auto& seed : old.seeds)
        if (seed.locked && seed.segment < old.segments.size ())
            locked.push_back (seed);
    if (locked.empty ())
        return true;
    uint32_t nextId = (std::max) (next.nextId, old.nextId);
    auto seeds = next.seeds;
    for (auto& seed : seeds)
        seed.id = nextId++; // fresh flats never reuse a kept flat's id
    for (auto seed : locked) {
        double along = 0;
        const size_t segment = Nearest (next, UnitCenter (old, seed), along);
        // The fresh flat the locked one lands on gives way.
        const auto replaced = std::find_if (seeds.begin (), seeds.end (), [&] (const UnitSeed& other) {
            return other.segment == segment && !other.locked && other.lo - 1e-6 <= along && along <= other.hi + 1e-6;
        });
        if (replaced != seeds.end ())
            seeds.erase (replaced);
        seed.segment = segment;
        seed.along = along;
        Remap (old, next, seed);
        seeds.push_back (seed);
    }
    const auto fresh = next.seeds;
    next.seeds = std::move (seeds);
    next.nextId = nextId;
    if (RelaxUnits (next))
        return true;
    next.seeds = fresh;
    return false;
}
bool ChangeUnitTarget (const Plan& plan, Draft& draft, const Floor& floor, size_t seed, size_t type, bool locked)
{
    auto& quick = QuickFor (plan, draft, floor);
    if (Conflict (plan, draft) || draft.dragging || quick.dragging || !quick.ready || seed >= quick.seeds.size () ||
        type >= quick.programme.types.size ())
        return false;
    const auto before = quick;
    quick.seeds[seed].type = type;
    quick.seeds[seed].target = floorprogramme::Target (quick.programme.types[type]);
    quick.seeds[seed].locked = locked;
    if (locked && !before.seeds[seed].locked)
        quick.seeds[seed].keep = Traits (quick, before.seeds[seed]).traits;
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
        for (size_t core = 0; core < draft.cores.size (); ++core)
            for (const auto direction : directions) {
                if (++attempts > 48)
                    break;
                auto cores = draft.cores;
                cores[core].center.x += step * direction.x;
                cores[core].center.y += step * direction.y;
                if (std::any_of (plan.floors.begin (), plan.floors.end (),
                                 [&] (const Floor& f) { return !Fits (f, cores[core], plan.angle); }))
                    continue;
                auto next = GenerateQuick (floor, cores, draft.programme, plan.angle);
                if (!TransferUnits (requested, next, int (seed)))
                    continue;
                auto designs = draft.quickPlans;
                bool feasible = true;
                for (const auto& f : plan.floors) {
                    auto& current = QuickFor (plan, draft, f);
                    if (&current == &quick)
                        continue;
                    auto replacement = GenerateQuick (f, cores, draft.programme, plan.angle);
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
                draft.cores = std::move (cores);
                draft.changed = true;
                return true;
            }
    quick.solveNote = "Target cannot fit with current locks and a bounded core relocation. No changes applied.";
    return false;
}
bool Regenerate (const Plan& plan, Draft& draft, const Floor& floor)
{
    auto& quick = QuickFor (plan, draft, floor);
    if (Conflict (plan, draft) || draft.dragging || quick.dragging)
        return false;
    auto fresh = GenerateQuick (floor, draft.cores, draft.programme, plan.angle);
    if (!fresh.ready) {
        quick.solveNote = fresh.note;
        return false;
    }
    if (!KeepLocked (quick, fresh)) {
        quick.solveNote = "Locked flats do not fit a fresh fill: unlock one or move a core. No changes applied.";
        return false;
    }
    OptimiseUnits (fresh, 800);
    const size_t kept = size_t (
        std::count_if (fresh.seeds.begin (), fresh.seeds.end (), [] (const UnitSeed& seed) { return seed.locked; }));
    fresh.stage = quick.stage;
    fresh.newType = (std::min) (quick.newType, fresh.programme.types.size () - 1);
    fresh.revision = quick.revision + 1;
    fresh.solveNote = "Regenerated from the programme around " + std::to_string (kept) + " locked flat(s).";
    quick = std::move (fresh);
    return true;
}
bool Optimise (const Plan& plan, Draft& draft, const Floor& floor)
{
    const auto& shown = QuickFor (plan, draft, floor);
    if (Conflict (plan, draft) || draft.dragging || shown.dragging || !shown.ready)
        return false;
    // Every shared design, weighted by the floors that use it.
    std::map<int, const Floor*> templates;
    std::map<int, double> weights;
    for (const auto& f : plan.floors) {
        const int story = frame::TemplateStory (plan, draft, f);
        templates.emplace (story, &f);
        weights[story] += 1;
    }
    std::map<int, QuickPlan> designs;
    double baseline = 0;
    bool ready = true;
    for (const auto& [story, f] : templates) {
        auto design = QuickFor (plan, draft, *f);
        if (!design.ready) {
            ready = false;
            continue;
        }
        OptimiseUnits (design, 800);
        baseline += design.score * weights[story];
        designs[story] = std::move (design);
    }
    std::string note = "Optimised unlocked flats on the current cores.";
    if (draft.moveCores && ready && !draft.cores.empty ()) {
        // Hill climb over single core moves valid on every floor; each candidate regenerates
        // the shared designs around their locked flats.
        auto bestCores = draft.cores;
        double best = baseline;
        std::map<int, QuickPlan> bestDesigns;
        for (int round = 0; round < 4; ++round) {
            const auto start = bestCores;
            bool moved = false;
            for (size_t c = 0; c < start.size (); ++c)
                for (const auto& step : Steps (plan.angle)) {
                    auto cores = start;
                    cores[c].center = { cores[c].center.x + step.x, cores[c].center.y + step.y };
                    cores[c].center = Snap (floor, cores[c], plan.angle);
                    if (std::any_of (plan.floors.begin (), plan.floors.end (),
                                     [&] (const Floor& f) { return !Fits (f, cores[c], plan.angle); }))
                        continue;
                    std::map<int, QuickPlan> candidate;
                    double total = 0;
                    bool feasible = true;
                    for (const auto& [story, f] : templates) {
                        auto next = GenerateQuick (*f, cores, draft.programme, plan.angle);
                        if (!next.ready || !KeepLocked (designs.at (story), next)) {
                            feasible = false;
                            break;
                        }
                        OptimiseUnits (next, 200);
                        total += next.score * weights[story];
                        candidate[story] = std::move (next);
                    }
                    if (feasible && total < best - 0.5) {
                        best = total;
                        bestCores = std::move (cores);
                        bestDesigns = std::move (candidate);
                        moved = true;
                    }
                }
            if (!moved)
                break;
        }
        if (bestCores != draft.cores) {
            for (auto& [story, design] : bestDesigns)
                OptimiseUnits (design, 600);
            designs = std::move (bestDesigns);
            draft.cores = std::move (bestCores);
            draft.changed = true;
            note = "Optimise moved proposed cores (score " + std::to_string (int (std::round (baseline))) + " -> " +
                   std::to_string (int (std::round (best))) + "). Save stairwells to keep them.";
        }
    }
    for (auto& [story, design] : designs) {
        auto& current = draft.quickPlans[story];
        design.stage = current.stage;
        design.revision = current.revision + 1;
        design.selected = -1;
        design.solveNote = note;
        current = std::move (design);
    }
    return !designs.empty ();
}
} // namespace geomsrv::archviz::buildingplan
