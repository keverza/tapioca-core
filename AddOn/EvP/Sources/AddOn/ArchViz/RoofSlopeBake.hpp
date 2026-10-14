#ifndef EVP_ARCHVIZ_ROOFSLOPEBAKE_HPP
#define EVP_ARCHVIZ_ROOFSLOPEBAKE_HPP
#include "ArchViz/RoofSlope.hpp"
#include "NodeGraph/Json.hpp"
#include <memory>
namespace geomsrv::archviz::overlayhud {
struct State;
}
namespace geomsrv::archviz::roofslopebake {
// Pure, counted recipe: merge render triangles back to planar roof polygons,
// retain holes, then bind associative dimensions to newly created roof GUIDs.
bool Transaction (const roofslope::Data& data, int floor, double symbolLength, const std::string& inheritSymbol,
                  evp::nodegraph::json::JsonValue& request, size_t& roofs, size_t& levels, std::string& error);
// Main-thread controller facade. Posts model work out of ImGui/Present; stale
// sessions are rejected before dispatch, and the dispatcher owns the sole Undo.
void Follow (const std::shared_ptr<overlayhud::State>& state);
} // namespace geomsrv::archviz::roofslopebake
#endif
