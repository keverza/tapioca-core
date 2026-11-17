// #201 Diagram Mode in the viewer: the scene's half. The publication is DiagramModel's;
// this file owns only when it is uploaded and how its frame is placed.
#include "ArchViz/DiligentSceneImpl.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/DiagramModel.hpp"
#include <windows.h>
#include <algorithm>

namespace geomsrv {
namespace archviz {

void DiligentScene::DrawDiagram (Diligent::IDeviceContext* context, const float viewProj[16],
                                 uint32_t colorBufferFormat, uint32_t depthBufferFormat)
{
    if (impl_ == nullptr || context == nullptr || impl_->device == nullptr || impl_->diagramInitFailed)
        return;
    const auto controls = diagrammodel::ReadControls ();
    const auto publication = diagrammodel::Read ();
    if (!controls.enabled || !publication) {
        impl_->diagramT = 0.0; // re-entry explodes from the assembled building again
        impl_->diagramLastTick = 0;
        return;
    }
    if (!impl_->diagram.IsReady ()) {
        std::string error;
        if (!impl_->diagram.Init (impl_->device, colorBufferFormat, depthBufferFormat, error)) {
            impl_->diagramInitFailed = true; // ONCE: a shader that failed fails every frame
            ArchVizLog ("Diligent scene: diagram layer unavailable (" + error + ")");
            return;
        }
    }
    // ⚠️ A NEW PUBLICATION IS THE ONLY UPLOAD. Animation changes translations, not bytes.
    if (publication->generation != impl_->diagramGeneration) {
        std::string error;
        if (!impl_->diagram.Upload (impl_->device, context, publication->geometry, error)) {
            ArchVizLog ("Diligent scene: diagram upload failed (" + error + ")");
            return;
        }
        impl_->diagramGeneration = publication->generation;
    }
    // A full travel takes about two thirds of a second; Evaluate's easing shapes it.
    const uint64_t now = ::GetTickCount64 ();
    const double seconds = impl_->diagramLastTick ? double (now - impl_->diagramLastTick) / 1000.0 : 0.0;
    impl_->diagramLastTick = now;
    const double step = std::clamp (seconds, 0.0, 0.1) * 1.5;
    const double target = controls.explode;
    impl_->diagramT = impl_->diagramT < target ? (std::min) (target, impl_->diagramT + step)
                                               : (std::max) (target, impl_->diagramT - step);
    const diagram::Frame frame = diagram::Evaluate (publication->plan, {}, impl_->diagramT);
    // The viewer draws world metres: the render origin is zero (DiagramGeometry.hpp).
    const auto draws = diagram::DrawsOf (publication->geometry, publication->plan, frame, diagram::Vec3 {});
    impl_->diagram.Draw (context, viewProj, draws, DiagramLayer::DrawParams {});
    diagrammodel::ReportDraw (impl_->diagram.UploadCount (), impl_->diagram.UploadedBytes (),
                              impl_->diagram.DrawCount (), impl_->diagramT);
}

} // namespace archviz
} // namespace geomsrv
