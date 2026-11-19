// #201 Diagram Mode in the viewer: the scene's half. The publication and the presentation come
// in a DiagramView (DiagramView.hpp); this file owns only when it is uploaded and where its
// frame is placed.
#include "ArchViz/DiligentSceneImpl.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/DiagramModel.hpp"
#include "ArchViz/DiagramView.hpp"
#include <windows.h>
#include <algorithm>
#include <cmath>

namespace geomsrv {
namespace archviz {

DiligentScene::DiagramFrameStats DiligentScene::DrawDiagram (Diligent::IDeviceContext* context,
                                                             const float viewProj[16], uint32_t colorBufferFormat,
                                                             uint32_t depthBufferFormat,
                                                             const diagram::DiagramView* view)
{
    DiagramFrameStats stats;
    if (impl_ == nullptr || context == nullptr || impl_->device == nullptr || impl_->diagramInitFailed)
        return stats;
    const auto& publication = view ? view->publication : nullptr;
    if (!publication) {
        impl_->diagramShown = Impl::ZeroSpacing (); // re-entry explodes from the assembled building again
        impl_->diagramPlan.reset ();
        impl_->diagramPlanFor.reset ();
        impl_->diagramLastTick = 0;
        return stats;
    }
    if (!impl_->diagram.IsReady ()) {
        std::string error;
        if (!impl_->diagram.Init (impl_->device, colorBufferFormat, depthBufferFormat, error)) {
            impl_->diagramInitFailed = true; // ONCE: a shader that failed fails every frame
            ArchVizLog ("Diligent scene: diagram layer unavailable (" + error + ")");
            return stats;
        }
    }
    // ⚠️ A NEW PUBLICATION IS THE ONLY UPLOAD. Spacing changes translations, not bytes.
    if (publication->generation != impl_->diagramGeneration) {
        std::string error;
        if (!impl_->diagram.Upload (impl_->device, context, publication->geometry, error)) {
            ArchVizLog ("Diligent scene: diagram upload failed (" + error + ")");
            return stats;
        }
        impl_->diagramGeneration = publication->generation;
    }
    // Eased toward the view's spacing: about 90 % of the way in a third of a second.
    const uint64_t now = ::GetTickCount64 ();
    const double seconds = impl_->diagramLastTick ? double (now - impl_->diagramLastTick) / 1000.0 : 0.0;
    impl_->diagramLastTick = now;
    impl_->diagramShown = view->animate ? diagram::Approach (impl_->diagramShown, view->spacing,
                                                             1.0 - std::exp (-7.0 * std::clamp (seconds, 0.0, 0.1)))
                                        : view->spacing;
    if (impl_->diagramPlanFor != publication || !impl_->diagramPlan ||
        !(impl_->diagramPlanSpacing == impl_->diagramShown)) {
        impl_->diagramPlan =
            std::make_shared<const diagram::LayoutPlan> (diagram::Spaced (publication->plan, impl_->diagramShown));
        impl_->diagramPlanFor = publication;
        impl_->diagramPlanSpacing = impl_->diagramShown;
    }
    const diagram::Frame frame = diagram::Evaluate (
        *impl_->diagramPlan,
        view->selected.Empty () ? std::vector<diagram::EntityId> {} : std::vector { view->selected }, 1.0);
    // The viewer draws world metres: the render origin is zero (DiagramGeometry.hpp).
    // Detail picks which uploaded body a storey draws; switching it uploads nothing.
    const auto draws = diagram::DrawsOf (publication->geometry, *impl_->diagramPlan, frame, diagram::Vec3 {},
                                         view->detail, view->shellWireframe);
    const auto lines = diagram::LineDrawsOf (publication->geometry, *impl_->diagramPlan, frame, diagram::Vec3 {},
                                             view->outlines, view->connectors, view->detail, view->shellWireframe);
    DiagramLayer::DrawParams params;
    params.monochrome = view->colouring == diagram::Colouring::White ? 1.0f : 0.0f;
    params.hiddenLines = view->hiddenLines;
    impl_->diagram.Draw (context, viewProj, draws, lines, params);
    stats.uploads = impl_->diagram.UploadCount ();
    stats.uploadedBytes = impl_->diagram.UploadedBytes ();
    stats.draws = impl_->diagram.DrawCount ();
    stats.lineDraws = impl_->diagram.LineDrawCount ();
    stats.shownGap = impl_->diagramShown.storeyGap;
    return stats;
}

void DiligentScene::SetCursorRay (CursorRayFn ray)
{
    if (impl_ != nullptr)
        impl_->cursorRay = std::move (ray);
}

bool DiligentScene::PickDiagram (int32_t px, int32_t py)
{
    if (impl_ == nullptr || !impl_->cursorRay || !diagrammodel::ReadControls ().enabled)
        return false;
    const auto publication = diagrammodel::Read ();
    // The plan on screen, not the commanded one: a click lands on what the user sees.
    const auto plan = impl_->diagramPlan;
    if (!publication || !plan || impl_->diagramPlanFor != publication)
        return false;
    float origin[3], direction[3];
    impl_->cursorRay (px, py, origin, direction);
    const diagram::Frame frame = diagram::Evaluate (*plan, {}, 1.0);
    // Topology selects a storey, Units a flat, Rooms a room: what the click is on.
    const auto hit =
        diagram::PickShown (frame, *plan, publication->geometry, publication->shells, publication->details,
                            diagrammodel::ReadControls ().detail, diagram::Vec3 { origin[0], origin[1], origin[2] },
                            diagram::Vec3 { direction[0], direction[1], direction[2] });
    if (!hit) {
        diagrammodel::Select ({});
        return false; // terrain, neighbours and the sky stay the model's to pick
    }
    diagrammodel::Select (hit->id);
    SetSelection ({}); // one highlight source: the diagram's
    return true;
}

} // namespace archviz
} // namespace geomsrv
