// Exact single-day obstruction attribution on its own preview channel. The
// analysis overlay and Grasshopper preview keep their independent content.

#include "ArchViz/DiligentSceneImpl.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/GhPreviewGeometry.hpp"

#include <string>

namespace geomsrv::archviz {

void DiligentScene::DrawObstructionOverlay (Diligent::IDeviceContext* context, const float viewProj[16],
                                            uint32_t surfaceWidth, uint32_t surfaceHeight, uint32_t colorBufferFormat,
                                            uint32_t depthBufferFormat, const GhPreviewDrawables* replacement,
                                            uint64_t key, bool modelIsDrawn)
{
    if (impl_ == nullptr || context == nullptr || impl_->device == nullptr || impl_->obstructionOverlayInitFailed)
        return;
    if (key == 0) {
        impl_->obstructionOverlayKey = 0;
        return;
    }
    if (!impl_->obstructionOverlay.IsReady ()) {
        std::string initError;
        if (!impl_->obstructionOverlay.Init (impl_->device, colorBufferFormat, depthBufferFormat, initError)) {
            impl_->obstructionOverlayInitFailed = true;
            ArchVizLog ("Diligent scene: obstruction overlay unavailable (" + initError + ")");
            return;
        }
    }
    if (replacement != nullptr && impl_->obstructionOverlayKey != key) {
        std::string uploadError;
        if (!impl_->obstructionOverlay.Upload (impl_->device, context, *replacement, uploadError)) {
            impl_->obstructionOverlayKey = 0;
            ArchVizLog ("Diligent scene: obstruction overlay not uploaded (" + uploadError + ")");
            return;
        }
        impl_->obstructionOverlayKey = key;
    }
    if (impl_->obstructionOverlayKey != key)
        return;
    DrawOcclusionDepth (context, viewProj, true, modelIsDrawn, colorBufferFormat, depthBufferFormat);
    GhPreviewLayer::DrawParams params;
    params.lineWidthPixels = 2.0f;
    params.ambient = 1.0f;
    params.hiddenDashPixels = 10.0f; // whole blockers remain readable behind the model
    params.pullMeshes = true;        // exact hit triangles lie on the model
    impl_->obstructionOverlay.Draw (context, viewProj, surfaceWidth, surfaceHeight, params);
}

} // namespace geomsrv::archviz
