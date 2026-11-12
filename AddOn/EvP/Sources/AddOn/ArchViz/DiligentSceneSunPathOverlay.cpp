// The sun path (#179) on its own preview channel: depth-tested against the
// model, a constant pixel width per layer, and independent of the preview,
// analysis and obstruction content. The geometry is built by the HUD
// (ArchViz/SunPathScene.hpp); this file only uploads and draws it.

#include "ArchViz/DiligentSceneImpl.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/GhPreviewGeometry.hpp"
#include "ArchViz/SunPathScene.hpp"

#include <string>

namespace geomsrv::archviz {

void DiligentScene::DrawSunPathOverlay (Diligent::IDeviceContext* context, const float viewProj[16],
                                        uint32_t surfaceWidth, uint32_t surfaceHeight, uint32_t colorBufferFormat,
                                        uint32_t depthBufferFormat, const SunPathSceneGeometry* replacement,
                                        uint64_t key, bool modelIsDrawn)
{
    if (impl_ == nullptr || context == nullptr || impl_->device == nullptr || impl_->sunPathInitFailed)
        return;
    if (key == 0 || replacement == nullptr) {
        impl_->sunPathKey = 0;
        return;
    }
    for (GhPreviewLayer& layer : impl_->sunPathLayers) {
        if (layer.IsReady ())
            continue;
        std::string initError;
        if (!layer.Init (impl_->device, colorBufferFormat, depthBufferFormat, initError)) {
            impl_->sunPathInitFailed = true;
            ArchVizLog ("Diligent scene: sun path overlay unavailable (" + initError + ")");
            return;
        }
    }
    if (impl_->sunPathKey != key) {
        const GhPreviewDrawables* sets[3] = { &replacement->fine, &replacement->arcs, &replacement->bold };
        for (int i = 0; i < 3; ++i) {
            std::string uploadError;
            if (!impl_->sunPathLayers[i].Upload (impl_->device, context, *sets[i], uploadError)) {
                impl_->sunPathKey = 0;
                ArchVizLog ("Diligent scene: sun path overlay not uploaded (" + uploadError + ")");
                return;
            }
        }
        impl_->sunPathKey = key;
    }
    // The model's depth first, as every occluded channel does: without it a
    // render mode that did not write this target's depth would let the path
    // paint over the building.
    DrawOcclusionDepth (context, viewProj, true, modelIsDrawn, colorBufferFormat, depthBufferFormat);
    // Thin structure underneath, the analysis over it, the selected slice last.
    constexpr float kWidths[3] = { kSunPathFinePixels, kSunPathArcPixels, kSunPathBoldPixels };
    for (int i = 0; i < 3; ++i) {
        GhPreviewLayer::DrawParams params;
        params.lineWidthPixels = kWidths[i];
        params.ambient = 0.55f;    // only the selected sun has normals; flat geometry is fully lit
        params.squareCaps = false; // dense curve pieces: overlapping caps would tooth every joint
        impl_->sunPathLayers[i].Draw (context, viewProj, surfaceWidth, surfaceHeight, params);
    }
}

} // namespace geomsrv::archviz
