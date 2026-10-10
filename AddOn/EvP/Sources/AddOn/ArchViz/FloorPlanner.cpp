#include "ArchViz/FloorPlanner.hpp"
#include <clipper2/clipper.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <thread>

namespace geomsrv::archviz::buildingplan {
namespace {
namespace fs = floorscheme;
namespace fe = floorscheme::edit;

// Generations at once: the cores but one, at most four (a floor takes tens of milliseconds).
size_t Workers ()
{
    const unsigned cores = std::thread::hardware_concurrency ();
    return std::clamp<size_t> (cores > 1 ? cores - 1 : 1, 1, 4);
}
void Write (std::ostringstream& out, const std::vector<fs::Ring>& rings)
{
    for (const auto& ring : rings) {
        out << '[';
        for (const auto& p : ring)
            out << std::llround (p.x * 1000) << ',' << std::llround (p.y * 1000) << ';';
        out << ']';
    }
}
} // namespace

std::vector<fs::Ring> Rings (const std::vector<SliceChain>& chains)
{
    std::vector<fs::Ring> rings;
    for (const auto& chain : chains) {
        fs::Ring ring;
        for (size_t i = 0; i < chain.Count (); ++i)
            ring.push_back ({ chain.xy[i * 2], chain.xy[i * 2 + 1] });
        if (ring.size () >= 3)
            rings.push_back (std::move (ring));
    }
    return rings;
}
std::vector<fs::Ring> FloorRings (const Floor& floor)
{
    if (floor.outlineKnown && !floor.outline.empty ())
        return Rings (floor.outline);
    // Each source's holes with its own rings (even-odd), then the sources joined (non-zero).
    namespace cp = Clipper2Lib;
    const auto paths = [] (const std::vector<fs::Ring>& rings) {
        cp::PathsD out;
        for (const auto& r : rings) {
            cp::PathD path;
            for (const auto& p : r)
                path.emplace_back (p.x, p.y);
            out.push_back (std::move (path));
        }
        return out;
    };
    cp::PathsD all;
    for (const auto& source : floor.contours) {
        const auto own = cp::Union (paths (Rings (source)), cp::FillRule::EvenOdd, 6);
        all.insert (all.end (), own.begin (), own.end ());
    }
    std::vector<fs::Ring> out;
    for (const auto& path : cp::Union (all, cp::FillRule::NonZero, 6)) {
        fs::Ring ring;
        for (const auto& p : path)
            ring.push_back ({ p.x, p.y });
        out.push_back (std::move (ring));
    }
    return out;
}
std::vector<fs::Pins::Core> StairPins (const std::vector<Core>& cores)
{
    std::vector<fs::Pins::Core> pins;
    for (const auto& core : cores)
        pins.push_back ({ { core.center.x, core.center.y }, core.width, core.depth });
    return pins;
}
int DesignStory (const Plan& plan, const Draft& draft, const Floor& floor)
{
    if (draft.designs.unique.contains (floor.story))
        return floor.story;
    for (const auto& other : plan.floors)
        if (!draft.designs.unique.contains (other.story) && other.outlineKey == floor.outlineKey)
            return other.story;
    return floor.story;
}
const fe::Design* FindDesign (const Plan& plan, const Draft& draft, const Floor& floor)
{
    const auto it = draft.designs.floors.find (DesignStory (plan, draft, floor));
    return it == draft.designs.floors.end () ? nullptr : &it->second;
}
std::string FloorId (const std::string& key, int story)
{
    return key + '#' + std::to_string (story);
}

FloorInput InputFor (const std::map<std::string, Plan>& plans, const std::map<std::string, Draft>& drafts,
                     const std::string& key, const Floor& floor, const floorprogramme::Programme& programme,
                     const std::vector<fs::Pins::Core>& lead, const fs::Options& options)
{
    FloorInput in;
    in.programme = programme;
    in.options = options;
    // Every building's floor at this elevation, in key order; this building's among them.
    std::vector<std::vector<fs::Ring>> floors;
    std::vector<const fe::Design*> designs;
    size_t mine = 0;
    for (const auto& [name, plan] : plans) {
        const Floor* at = name == key ? &floor : nullptr;
        if (!at)
            for (const auto& other : plan.floors)
                if (std::abs (other.z - floor.z) <= kElevationTolerance)
                    at = &other;
        if (!at)
            continue;
        if (name == key)
            mine = floors.size ();
        floors.push_back (FloorRings (*at));
        const auto draft = drafts.find (name);
        designs.push_back (draft == drafts.end () ? nullptr : FindDesign (plan, draft->second, *at));
    }
    if (floors.empty () || !plans.contains (key)) { // a floor of no known building: on its own
        floors = { FloorRings (floor) };
        designs = { nullptr };
        mine = 0;
    }
    auto owned = fe::Owned (floors, designs);
    in.owned = std::move (owned[mine]);
    for (size_t i = 0; i < owned.size (); ++i)
        if (i != mine)
            in.party.insert (in.party.end (), owned[i].begin (), owned[i].end ());
    if (designs[mine])
        in.design = *designs[mine];
    // The stack on every floor, so the stairs stand at one place; without one, the building's
    // saved stairs (the floor the stack is planned on).
    const auto draft = drafts.find (key);
    const auto stairs = !lead.empty () || draft == drafts.end () ? lead : StairPins (draft->second.cores);
    in.design.pins.cores = stairs;
    in.options.holdCores = !stairs.empty (); // the stack stands on every floor, never moved
    Sign (in);
    return in;
}
void Sign (FloorInput& in)
{
    std::ostringstream signature;
    signature << std::setprecision (9) << floorprogramme::Key (in.programme) << '|' << in.options.grossFactor << '|'
              << in.options.north << '|' << in.options.holdCores << '|';
    Write (signature, in.owned);
    signature << '|';
    Write (signature, in.party);
    signature << '|';
    fe::Designs one;
    one.floors[0] = in.design;
    signature << fe::ToJson (one) << '|' << in.design.pins.cores.size ();
    in.signature = signature.str ();
}

const Planner::Planned* Planner::Latest (const std::string& id) const
{
    const auto it = done_.find (id);
    return it == done_.end () ? nullptr : &it->second;
}
bool Planner::Pending (const std::string& id, const std::string& signature) const
{
    const auto job = jobs_.find (id);
    if (job == jobs_.end ())
        return false;
    return (job->second.running.valid () && job->second.runningSignature == signature) ||
           (job->second.waiting && job->second.waiting->signature == signature);
}
const Planner::Planned* Planner::Want (const std::string& id, FloorInput input, bool first)
{
    const auto* latest = Latest (id);
    if ((latest && latest->signature == input.signature) || Pending (id, input.signature))
        return latest;
    auto& job = jobs_[id];
    job.waiting = std::move (input);
    std::erase (queue_, id);
    if (first)
        queue_.push_front (id);
    else
        queue_.push_back (id);
    Start ();
    return latest;
}
size_t Planner::Running () const
{
    return static_cast<size_t> (
        std::count_if (jobs_.begin (), jobs_.end (), [] (const auto& job) { return job.second.running.valid (); }));
}
void Planner::Start ()
{
    for (auto it = queue_.begin (); it != queue_.end () && Running () < Workers ();) {
        auto& job = jobs_[*it];
        if (job.running.valid () || !job.waiting) {
            it = job.waiting ? std::next (it) : queue_.erase (it);
            continue;
        }
        auto input = std::move (*job.waiting);
        job.waiting.reset ();
        job.runningSignature = input.signature;
        job.started = std::chrono::steady_clock::now ();
        job.running = std::async (std::launch::async, [input = std::move (input)] {
            const auto start = std::chrono::steady_clock::now ();
            Planned planned;
            planned.signature = input.signature;
            planned.scheme = fe::RunOwned (input.owned, input.party, input.programme, input.design, input.options);
            planned.ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - start).count ();
            return planned;
        });
        it = queue_.erase (it);
    }
}
bool Planner::Poll ()
{
    bool arrived = false;
    for (auto& [id, job] : jobs_)
        if (job.running.valid () && job.running.wait_for (std::chrono::seconds (0)) == std::future_status::ready) {
            auto planned = job.running.get ();
            planned.revision = ++revision_;
            done_[id] = std::move (planned);
            arrived = true;
            if (job.waiting && std::find (queue_.begin (), queue_.end (), id) == queue_.end ())
                queue_.push_back (id);
        }
    Start ();
    return arrived;
}
bool Planner::Busy () const
{
    return Running () > 0 ||
           std::any_of (jobs_.begin (), jobs_.end (), [] (const auto& job) { return job.second.waiting.has_value (); });
}
std::vector<fs::Ring> Common (const Plan& plan, const std::vector<fs::Ring>& owned)
{
    namespace cp = Clipper2Lib;
    const auto paths = [] (const std::vector<fs::Ring>& rings) {
        cp::PathsD out;
        for (const auto& r : rings) {
            cp::PathD path;
            for (const auto& p : r)
                path.emplace_back (p.x, p.y);
            out.push_back (std::move (path));
        }
        return out;
    };
    auto common = cp::Union (paths (owned), cp::FillRule::NonZero, 6);
    for (const auto& floor : plan.floors)
        common = cp::Intersect (common, cp::Union (paths (FloorRings (floor)), cp::FillRule::NonZero, 6),
                                cp::FillRule::NonZero, 6);
    // Too little floor in common to plan a stair: the lead floor's own (CheckStack reports the
    // floors the stack misses).
    if (std::abs (cp::Area (common)) < kMinCommon)
        return owned;
    std::vector<fs::Ring> out;
    for (const auto& path : common) {
        fs::Ring ring;
        for (const auto& p : path)
            ring.push_back ({ p.x, p.y });
        out.push_back (std::move (ring));
    }
    return out;
}
std::string StackId (const std::string& key, const Plan& plan)
{
    const Floor* lead = LeadFloor (plan);
    return FloorId (key, lead ? lead->story : 0) + "#auto";
}
bool Stack (const Planner& planner, const std::string& key, const Plan& plan, std::vector<fs::Pins::Core>& out)
{
    out.clear ();
    const auto* planned = planner.Latest (StackId (key, plan));
    if (!planned)
        return false;
    for (const auto& core : planned->scheme.cores)
        out.push_back ({ core.centre, core.width, core.depth });
    return true;
}
const Floor* LeadFloor (const Plan& plan)
{
    const Floor* lead = nullptr;
    for (const auto& floor : plan.floors)
        if (!lead || floor.areaM2 > lead->areaM2 + 1e-6)
            lead = &floor;
    return lead;
}
const Planner::Planned* WantFloors (Planner& planner, const std::map<std::string, Plan>& plans,
                                    const std::map<std::string, Draft>& drafts, const std::string& key,
                                    const floorprogramme::Programme& programme, int shownStory,
                                    const fs::Options& options)
{
    const auto plan = plans.find (key);
    if (plan == plans.end ())
        return nullptr;
    const Floor* lead = LeadFloor (plan->second);
    if (!lead)
        return nullptr;
    // The stack: the floor every floor shares, planned with the building's saved stairs as
    // ordinary pins (they may move to fit, here only); the stairs it gets are held on every
    // floor, the lead floor too, so identical floors plan identically.
    auto reference = InputFor (plans, drafts, key, *lead, programme, {}, options);
    reference.owned = Common (plan->second, reference.owned);
    reference.options.holdCores = false;
    Sign (reference);
    planner.Want (StackId (key, plan->second), std::move (reference), true);
    std::vector<fs::Pins::Core> stairs;
    if (!Stack (planner, key, plan->second, stairs))
        return nullptr; // the stairs first
    const Planner::Planned* shown = nullptr;
    for (const auto& floor : plan->second.floors) {
        const auto* got =
            planner.Want (FloorId (key, floor.story), InputFor (plans, drafts, key, floor, programme, stairs, options),
                          floor.story == shownStory);
        if (floor.story == shownStory)
            shown = got;
    }
    return shown;
}
} // namespace geomsrv::archviz::buildingplan
