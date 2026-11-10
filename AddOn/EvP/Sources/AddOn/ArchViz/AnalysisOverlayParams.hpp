#ifndef EVP_ARCHVIZ_ANALYSISOVERLAYPARAMS_HPP
#define EVP_ARCHVIZ_ANALYSISOVERLAYPARAMS_HPP

#include "ArchViz/GhPreviewLayer.hpp"

namespace geomsrv::archviz {

// Analysis geometry is measured against the model's depth. Ordinary meshes must
// not be pulled toward the camera: constant and slope-scaled rasterizer bias can
// carry an isovist across a real occluder, especially at grazing angles. Only
// the separate spherical surface-decal bucket gets a small depth-only offset.
// Lines keep the preview layer's established pull and hidden, dashed pass.
inline GhPreviewLayer::DrawParams AnalysisOverlayDrawParams (float nearClip, float farClip, bool perspective)
{
    GhPreviewLayer::DrawParams params;
    params.nearClipMetres = nearClip;
    params.farClipMetres = farClip;
    params.perspective = perspective;
    params.lineWidthPixels = 2.0f;
    params.ambient = 1.0f;           // flat: analysis is a measurement, not a lit surface
    params.hiddenDashPixels = 10.0f; // guides behind the model stay, dashed
    params.pullMeshes = false;
    return params;
}

} // namespace geomsrv::archviz

#endif // EVP_ARCHVIZ_ANALYSISOVERLAYPARAMS_HPP
