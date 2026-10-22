#ifndef TAPIOCA_NATIVE_VIRTUAL_EDITOR_TERRAIN_HPP
#define TAPIOCA_NATIVE_VIRTUAL_EDITOR_TERRAIN_HPP

#include "ObjectState.hpp"
#include <string>
namespace geomsrv::archviz::edithud {
struct Session;
}
namespace geomsrv {
// Main thread: read one requested Mesh, no selection or model writes.
bool StartNativeTerrainDesign (archviz::edithud::Session& session, const GS::ObjectState& params, std::string& error);
GS::ObjectState VirtualTerrainStatus (archviz::edithud::Session& session);
} // namespace geomsrv
#endif
