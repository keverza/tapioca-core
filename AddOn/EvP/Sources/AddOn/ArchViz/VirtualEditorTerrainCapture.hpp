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
// The same for adding the Archicad selection's drafting (Tapioca.VirtualEditor import): queued
// once per request, checked again at dispatch, its outcome the session's message.
void FollowDraftingImport (const std::shared_ptr<overlayhud::State>& state);
} // namespace geomsrv::archviz
#endif
