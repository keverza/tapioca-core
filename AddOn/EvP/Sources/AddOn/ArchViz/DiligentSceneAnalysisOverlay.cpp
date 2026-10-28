// The analysis overlay: the viewpoint isovist (ArchViz/ViewpointOverlayGeometry)
// drawn through a second GhPreviewLayer, so the model occludes its fill, edge and
// radius guide. Drawn as ImGui polygons over the frame it showed through walls
// and floors, which read as "visible" exactly where the view is blocked.
//
// ⚠️ ITS OWN LAYER, NOT THE PREVIEW'S. The preview layer's content is rebuilt
// from the preview stores on their generation; sharing it would make one
// producer's upload erase the other's.

#include "ArchViz/DiligentSceneImpl.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/GhPreviewGeometry.hpp"

#include <string>

namespace geomsrv {
namespace archviz {

uint64_t DiligentScene::AnalysisOverlayKey () const
{
    return impl_ != nullptr ? impl_->analysisOverlayKey : 0;
}

void DiligentScene::DrawAnalysisOverlay (Diligent::IDeviceContext* context, const float viewProj[16],
                                         uint32_t surfaceWidth, uint32_t surfaceHeight, uint32_t colorBufferFormat,
                                         uint32_t depthBufferFormat, const GhPreviewDrawables* replacement,
                                         uint64_t key, bool modelIsDrawn)
{
    if (impl_ == nullptr || context == nullptr || impl_->device == nullptr || impl_->analysisOverlayInitFailed)
        return;
    if (key == 0) {
        // Hidden: draw nothing, and forget the upload so showing it rebuilds.
        impl_->analysisOverlayKey = 0;
        return;
    }
    // ⚠️ ONCE, like the preview layer: shaders that will not compile will not
    // compile next frame either.
    if (!impl_->analysisOverlay.IsReady ()) {
        std::string initError;
        if (!impl_->analysisOverlay.Init (impl_->device, colorBufferFormat, depthBufferFormat, initError)) {
            impl_->analysisOverlayInitFailed = true;
            ArchVizLog ("Diligent scene: analysis overlay layer unavailable (" + initError + ")");
            return;
        }
    }
    if (replacement != nullptr) {
        std::string uploadError;
        if (!impl_->analysisOverlay.Upload (impl_->device, context, *replacement, uploadError)) {
            impl_->analysisOverlayKey = 0;
            ArchVizLog ("Diligent scene: analysis overlay not uploaded (" + uploadError + ")");
            return;
        }
        impl_->analysisOverlayKey = key;
    }
    if (impl_->analysisOverlayKey != key)
        return;
    // The model's depth, where the shaded pass has not already left it.
    DrawOcclusionDepth (context, viewProj, true, modelIsDrawn, colorBufferFormat, depthBufferFormat);
    GhPreviewLayer::DrawParams params;
    params.lineWidthPixels = 2.0f;
    params.ambient = 1.0f; // flat: the isovist is a measurement, not a lit surface
    impl_->analysisOverlay.Draw (context, viewProj, surfaceWidth, surfaceHeight, params);
}

} // namespace archviz
} // namespace geomsrv
