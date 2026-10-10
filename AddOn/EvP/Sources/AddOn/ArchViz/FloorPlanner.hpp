#ifndef EVP_ARCHVIZ_FLOORPLANNER_HPP
#define EVP_ARCHVIZ_FLOORPLANNER_HPP

// Every building floor's typology scheme (FloorScheme.hpp), planned off the UI thread and kept:
// the Plan view's canvas shows one, the overlay draws them all. A floor is planned as its
// building's own silhouette among the other buildings at its elevation (user, 2026-10-10: "show
// the user only the building, but behind the scenes the whole floor exists as context"): what
// it owns of the massing floor (floorscheme::edit::Owned), the walls against the others blind,
// its design, and the building's stairs pinned on every floor so they stack.
//
// MAIN THREAD for the Planner's calls; the generator runs on worker threads with copies.
#include "ArchViz/HudBuildingPlan.hpp"
#include <chrono>
#include <deque>
#include <future>
#include <optional>

namespace geomsrv::archviz::buildingplan {
constexpr double kElevationTolerance = 0.05; // m: floors of different buildings at one elevation

// The floor whose design `floor` uses: its own when unique or when no lower floor has the same
// outline, else that floor's.
int DesignStory (const Plan& plan, const Draft& draft, const Floor& floor);
const floorscheme::edit::Design* FindDesign (const Plan& plan, const Draft& draft, const Floor& floor);
std::vector<floorscheme::Ring> Rings (const std::vector<SliceChain>& chains);
// A floor's counted boundary: its outline, or the union of its sources' contours when it has none.
std::vector<floorscheme::Ring> FloorRings (const Floor& floor);
// The building's stairs as generator pins.
std::vector<floorscheme::Pins::Core> StairPins (const std::vector<Core>& cores);

// What one building floor is planned from.
struct FloorInput {
    std::vector<floorscheme::Ring> owned, party; // its silhouette; the other buildings' at the elevation
    floorscheme::edit::Design design;            // with the building's stairs pinned
    floorprogramme::Programme programme = floorprogramme::Default ();
    floorscheme::Options options;
    std::string signature; // everything above: equal signatures plan equal schemes
};
// `key`'s `floor` among every building at its elevation (`plans` in key order: an overlap of
// floors is the earlier building's). `lead`: stairs to pin when the building has none of its own
// (the largest floor's, so the floors' stairs stack).
FloorInput InputFor (const std::map<std::string, Plan>& plans, const std::map<std::string, Draft>& drafts,
                     const std::string& key, const Floor& floor, const floorprogramme::Programme& programme,
                     const std::vector<floorscheme::Pins::Core>& lead = {}, const floorscheme::Options& options = {});
// `input.signature` from the rest of it, after a change (a gesture's design on screen).
void Sign (FloorInput& input);
// The id a floor's scheme is kept under.
std::string FloorId (const std::string& key, int story);

class Planner {
  public:
    struct Planned {
        std::string signature;
        floorscheme::Scheme scheme;
        double ms = 0;         // how long the generator took
        uint64_t revision = 0; // the planner's revision when it arrived
    };
    // The newest scheme kept for `id` (it may be of an older input), asking for `input` when its
    // signature is new; `first` puts it ahead of the others waiting (the floor on screen).
    const Planned* Want (const std::string& id, FloorInput input, bool first = false);
    const Planned* Latest (const std::string& id) const;
    // Takes finished schemes and starts waiting ones; true when a scheme arrived.
    bool Poll ();
    bool Busy () const;                                                       // generating or waiting
    bool Pending (const std::string& id, const std::string& signature) const; // asked, not yet planned
    uint64_t Revision () const
    {
        return revision_;
    }

  private:
    struct Job {
        std::future<Planned> running;
        std::string runningSignature;
        std::optional<FloorInput> waiting;
        std::chrono::steady_clock::time_point started;
    };
    size_t Running () const;
    void Start ();
    std::map<std::string, Planned> done_;
    std::map<std::string, Job> jobs_;
    std::deque<std::string> queue_;
    uint64_t revision_ = 0;
};

// The building's largest floor: its stairs lead the others' while the building has none saved.
const Floor* LeadFloor (const Plan& plan);
// Every floor of `key` asked of `planner`, the shown story first; with no stairs saved, the lead
// floor is planned free first and its stairs pinned on every floor. The newest scheme of the
// shown story, or null while it has none.
const Planner::Planned* WantFloors (Planner& planner, const std::map<std::string, Plan>& plans,
                                    const std::map<std::string, Draft>& drafts, const std::string& key,
                                    const floorprogramme::Programme& programme, int shownStory,
                                    const floorscheme::Options& options = {});
} // namespace geomsrv::archviz::buildingplan
#endif
