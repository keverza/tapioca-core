#ifndef TAPIOCA_VIRTUAL_EDITOR_TERRAIN_BAKE_HPP
#define TAPIOCA_VIRTUAL_EDITOR_TERRAIN_BAKE_HPP

#include <memory>

namespace geomsrv::archviz {
namespace overlayhud {
struct State;
}
// tapioca-dev #177. Posts the editor's requested Mesh create to the main thread, once per
// request; the dispatcher owns the undo step. Called on the message loop, never from layout.
void FollowTerrainBake (const std::shared_ptr<overlayhud::State>& state);
} // namespace geomsrv::archviz
#endif
