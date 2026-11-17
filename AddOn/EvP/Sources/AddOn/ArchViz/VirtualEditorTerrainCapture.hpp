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
// #124: the editor's focus request, carried out as Archicad's own zoom (ACAPI_View_Zoom) on the
// main thread: the plan to the model box, 3D to the pixel rectangle. A view change only: no
// element or document is written. `archicadView` is false in the Tapioca viewer, which has no
// such request yet and says so.
void FollowFocus (const std::shared_ptr<overlayhud::State>& state, bool archicadView);
} // namespace geomsrv::archviz
#endif
