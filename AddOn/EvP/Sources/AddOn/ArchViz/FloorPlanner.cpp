#include "ArchViz/FloorPlanner.hpp"
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
        floors.push_back (Rings (at->outline));
        const auto draft = drafts.find (name);
        designs.push_back (draft == drafts.end () ? nullptr : FindDesign (plan, draft->second, *at));
    }
    if (floors.empty () || !plans.contains (key)) { // a floor of no known building: on its own
        floors = { Rings (floor.outline) };
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
    // The building's stairs on every floor, so they stack; none of its own: the lead floor's.
    const auto draft = drafts.find (key);
    const auto stairs =
        draft != drafts.end () && !draft->second.cores.empty () ? StairPins (draft->second.cores) : lead;
    in.design.pins.cores = stairs;
    std::ostringstream signature;
    signature << std::setprecision (9) << floorprogramme::Key (programme) << '|' << options.grossFactor << '|'
              << options.north << '|';
    Write (signature, in.owned);
    signature << '|';
    Write (signature, in.party);
    signature << '|';
    fe::Designs one;
    one.floors[0] = in.design;
    signature << fe::ToJson (one) << '|' << in.design.pins.cores.size ();
    in.signature = signature.str ();
    return in;
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
    // Stairs that stack: the building's own, or else where the generator puts them on the lead
    // floor planned free (`#auto`), pinned on every floor alike -- the lead floor too, since
    // pinned stairs set the section boundaries, so identical floors plan identically.
    const auto draft = drafts.find (key);
    std::vector<fs::Pins::Core> stairs;
    if (draft == drafts.end () || draft->second.cores.empty ()) {
        auto free = InputFor (plans, drafts, key, *lead, programme, {}, options);
        planner.Want (FloorId (key, lead->story) + "#auto", std::move (free), true);
        const auto* led = planner.Latest (FloorId (key, lead->story) + "#auto");
        if (!led)
            return nullptr; // the stairs first
        for (const auto& core : led->scheme.cores)
            stairs.push_back ({ core.centre, core.width, core.depth });
    }
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
