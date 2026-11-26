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
                                               const planting::PlantingSchemeGeometry* envelope, uint64_t envelopeKey,
                                               bool modelIsDrawn)
{
    if (impl_ == nullptr || context == nullptr || impl_->device == nullptr || impl_->plantingSchemeInitFailed)
        return;
    if (key == 0 || replacement == nullptr) {
        impl_->plantingSchemeKey = impl_->plantingEnvelopeKey = 0;
        return;
    }
    if (envelope == nullptr)
        envelopeKey = 0;
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
    const auto upload = [&] (const planting::PlantingSchemeGeometry& g, int first, uint64_t& kept, uint64_t wanted) {
        if (kept == wanted)
            return true;
        const GhPreviewDrawables* sets[2] = { &g.crowns, &g.links };
        for (int i = 0; i < 2; ++i) {
            std::string uploadError;
            if (!impl_->plantingSchemeLayers[first + i].Upload (impl_->device, context, *sets[i], uploadError)) {
                kept = 0;
                ArchVizLog ("Diligent scene: planting scheme overlay not uploaded (" + uploadError + ")");
                return false;
            }
        }
        kept = wanted;
        return true;
    };
    if (!upload (*replacement, 0, impl_->plantingSchemeKey, key))
        return;
    if (envelopeKey != 0 && !upload (*envelope, 2, impl_->plantingEnvelopeKey, envelopeKey))
        envelopeKey = 0;
    // The model's depth first, as every occluded channel does.
    DrawOcclusionDepth (context, viewProj, true, modelIsDrawn, colorBufferFormat, depthBufferFormat);
    constexpr float kWidths[4] = { 1.25f, planting::kSchemeLinkPixels, 1.25f, planting::kSchemeLinkPixels };
    for (int i = 0; i < (envelopeKey != 0 ? 4 : 2); ++i) {
        GhPreviewLayer::DrawParams params;
        params.lineWidthPixels = kWidths[i];
        params.ambient = 0.6f; // the cylinders' sides take the light; their tops stay readable
        params.squareCaps = false;
        impl_->plantingSchemeLayers[i].Draw (context, viewProj, surfaceWidth, surfaceHeight, params);
    }
}

} // namespace geomsrv::archviz
