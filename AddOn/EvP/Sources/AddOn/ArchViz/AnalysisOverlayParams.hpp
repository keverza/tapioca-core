#ifndef EVP_ARCHVIZ_ANALYSISOVERLAYPARAMS_HPP
#define EVP_ARCHVIZ_ANALYSISOVERLAYPARAMS_HPP

#include "ArchViz/GhPreviewLayer.hpp"

namespace geomsrv::archviz {

// Analysis geometry is measured against the model's depth. Meshes must not be
// pulled toward the camera: constant and slope-scaled depth bias can carry an
// isovist or a seen-surface patch across a real occluder, especially at grazing
// angles. Spherical seen surfaces keep their geometric lift instead. Lines keep
// the preview layer's established pull and their separate hidden, dashed pass.
inline GhPreviewLayer::DrawParams AnalysisOverlayDrawParams ()
{
    GhPreviewLayer::DrawParams params;
    params.lineWidthPixels = 2.0f;
    params.ambient = 1.0f;           // flat: analysis is a measurement, not a lit surface
    params.hiddenDashPixels = 10.0f; // guides behind the model stay, dashed
    params.pullMeshes = false;
    return params;
}

} // namespace geomsrv::archviz

#endif // EVP_ARCHVIZ_ANALYSISOVERLAYPARAMS_HPP
