#ifndef EVP_ARCHVIZ_MASSINGSLICESMODEL_HPP
#define EVP_ARCHVIZ_MASSINGSLICESMODEL_HPP
#include "ArchViz/MassingSlices.hpp"

namespace geomsrv::archviz::massingslicesmodel {
// Main-thread snapshot facade. Native selection/metadata changes start the
// automatic preview; a timer follows slab edits without ImGui or model writes.
void Changed ();
void Poll ();
std::shared_ptr<const massingslices::Result> Read ();
std::vector<std::string> SelectedGuids ();
void Display (bool shown, const storysliceoverlay::Controls& controls);
void HoverFunction (const std::string& function);
void CollapseZone (bool shown);
std::string CollapseNote ();
std::shared_ptr<const std::vector<SliceChain>> CollapseContours ();
bool Shown ();
storysliceoverlay::Controls Controls ();
void Forget ();
void Shutdown ();

// ANY THREAD. How often a result was built and published, and how often a body pass
// brought back the geometry already shown and rebuilt nothing (#235). On an idle model
// `builds` stays flat.
struct Counts {
    uint64_t builds = 0, sameBodies = 0;
};
Counts ReadCounts ();
} // namespace geomsrv::archviz::massingslicesmodel
#endif
