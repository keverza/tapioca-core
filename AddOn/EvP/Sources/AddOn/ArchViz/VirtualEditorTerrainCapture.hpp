#ifndef TAPIOCA_ARCHVIZ_VIRTUAL_EDITOR_TERRAIN_CAPTURE_HPP
#define TAPIOCA_ARCHVIZ_VIRTUAL_EDITOR_TERRAIN_CAPTURE_HPP
#include <memory>
namespace geomsrv::archviz::overlayhud {
struct State;
}
namespace geomsrv::archviz {
// Main-thread controller, not layout/Present. Dispatches the existing native
// capture action after session/revision checks; no native model/selection writes.
void FollowTerrainCapture (const std::shared_ptr<overlayhud::State>& state);
} // namespace geomsrv::archviz
#endif
