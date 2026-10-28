#ifndef EVP_ARCHVIZ_VIEWPOINTOVERLAYGEOMETRY_HPP
#define EVP_ARCHVIZ_VIEWPOINTOVERLAYGEOMETRY_HPP

// ArchViz/ViewpointOverlayGeometry -- the viewpoint isovist as world-space
// drawables for a depth-tested preview layer (GhPreviewLayer), so the model
// occludes the fill, its edge and the radius guide like any other surface.
// Drawn as ImGui polygons over the frame it showed through walls and floors.
//
// Pure: the layer that draws it, and the camera, are not this file's business.

#include "ArchViz/GhPreviewGeometry.hpp"
#include "SunStudy/ViewpointStudy.hpp"

#include <array>
#include <cstdint>

namespace geomsrv::archviz {

// RGBA8, 0xRRGGBBAA like every other layer.
struct ViewpointOverlayStyle {
    uint32_t fillRgba = 0xF5C44F80u;     // the visible area, warm yellow at 50%
    uint32_t openEdgeRgba = 0xE1A428E6u; // perimeter where a ray reached the radius
    uint32_t blockedRgba = 0xEC6240F0u;  // perimeter on Context, and each run's ends
    uint32_t guideRgba = 0xF5C44F96u;    // the full radius circle
};

// `options` are the current settings: they place the guide (circle, field of
// view arc and edges, or the sphere's band rings). `result` may be null (still
// calculating), and is drawn where IT was computed, never moved: a fan for the
// plan modes, the visibility hull for a sphere.
GhPreviewDrawables BuildViewpointOverlay (const evp::sunstudy::ViewpointOptions& options,
                                          const evp::sunstudy::ViewpointStudyResult* result,
                                          const ViewpointOverlayStyle& style);

} // namespace geomsrv::archviz

#endif
