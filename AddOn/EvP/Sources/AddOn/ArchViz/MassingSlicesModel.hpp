#ifndef EVP_ARCHVIZ_MASSINGSLICESMODEL_HPP
#define EVP_ARCHVIZ_MASSINGSLICESMODEL_HPP
#include "ArchViz/MassingSlices.hpp"

namespace geomsrv::archviz::massingslicesmodel {
// Main-thread snapshot facade. Native selection/metadata changes start the
// automatic preview; a timer follows slab edits without ImGui or model writes.
void Changed ();
void Poll ();
std::shared_ptr<const massingslices::Result> Read ();
void Forget ();
void Shutdown ();
} // namespace geomsrv::archviz::massingslicesmodel
#endif
