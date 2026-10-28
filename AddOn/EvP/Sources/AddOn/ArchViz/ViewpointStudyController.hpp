#ifndef EVP_ARCHVIZ_VIEWPOINTSTUDYCONTROLLER_HPP
#define EVP_ARCHVIZ_VIEWPOINTSTUDYCONTROLLER_HPP

#include "SunStudy/ViewpointStudy.hpp"

namespace geomsrv::archviz::viewpointstudy {

struct Settings {
    uint64_t revision = 0;
    bool enabled = false;
    bool pointValid = true;
    // Every captured element stops rays -- the viewer's default. False: only
    // `context` does, and an empty `context` gives the full radius circle.
    // The Analysis tabs share this Context with surface and point visibility.
    bool contextAll = true;
    std::vector<std::string> context;
    evp::sunstudy::ViewpointOptions options;
};

struct Status {
    bool calculating = false;
    std::string error;
    std::shared_ptr<const evp::sunstudy::ViewpointStudyResult> result;
};

// Immutable settings cross the command/render seam. One coalescing worker owns
// BVH admission and rays; moving the gumball never waits for them.
void Configure (Settings settings);
std::shared_ptr<const Settings> GetSettings ();
void Tick (std::shared_ptr<const Snapshot> snapshot);
Status GetStatus ();
void Shutdown ();

} // namespace geomsrv::archviz::viewpointstudy
#endif
