// The planting scheme (#278) on its own preview channel: translucent crown
// cylinders and the lines joining each species, depth-tested against the model
// like the sun path. The geometry is built by the HUD
// (ArchViz/PlantingSchemeScene.hpp); this file only uploads and draws it.

#include "ArchViz/DiligentSceneImpl.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/GhPreviewGeometry.hpp"
#include "ArchViz/PlantingSchemeScene.hpp"

#include <string>

namespace geomsrv::archviz {

void DiligentScene::DrawPlantingSchemeOverlay (Diligent::IDeviceContext* context, const float viewProj[16],
                                               uint32_t surfaceWidth, uint32_t surfaceHeight,
                                               uint32_t colorBufferFormat, uint32_t depthBufferFormat,
                                               const planting::PlantingSchemeGeometry* replacement, uint64_t key,
                                               bool modelIsDrawn)
{
    if (impl_ == nullptr || context == nullptr || impl_->device == nullptr || impl_->plantingSchemeInitFailed)
        return;
    if (key == 0 || replacement == nullptr) {
        impl_->plantingSchemeKey = 0;
        return;
    }
    for (GhPreviewLayer& layer : impl_->plantingSchemeLayers) {
        if (layer.IsReady ())
            continue;
        std::string initError;
        if (!layer.Init (impl_->device, colorBufferFormat, depthBufferFormat, initError)) {
            impl_->plantingSchemeInitFailed = true;
            ArchVizLog ("Diligent scene: planting scheme overlay unavailable (" + initError + ")");
            return;
        }
    }
    if (impl_->plantingSchemeKey != key) {
        const GhPreviewDrawables* sets[2] = { &replacement->crowns, &replacement->links };
        for (int i = 0; i < 2; ++i) {
            std::string uploadError;
            if (!impl_->plantingSchemeLayers[i].Upload (impl_->device, context, *sets[i], uploadError)) {
                impl_->plantingSchemeKey = 0;
                ArchVizLog ("Diligent scene: planting scheme overlay not uploaded (" + uploadError + ")");
                return;
            }
        }
        impl_->plantingSchemeKey = key;
    }
    // The model's depth first, as every occluded channel does.
    DrawOcclusionDepth (context, viewProj, true, modelIsDrawn, colorBufferFormat, depthBufferFormat);
    constexpr float kWidths[2] = { 1.25f, planting::kSchemeLinkPixels };
    for (int i = 0; i < 2; ++i) {
        GhPreviewLayer::DrawParams params;
        params.lineWidthPixels = kWidths[i];
        params.ambient = 0.6f; // the cylinders' sides take the light; their tops stay readable
        params.squareCaps = false;
        impl_->plantingSchemeLayers[i].Draw (context, viewProj, surfaceWidth, surfaceHeight, params);
    }
}

} // namespace geomsrv::archviz
