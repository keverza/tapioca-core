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
#include <vector>
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
// plan modes, the seen surface cells for a sphere (no shell of its own).
GhPreviewDrawables BuildViewpointOverlay (const evp::sunstudy::ViewpointOptions& options,
                                          const evp::sunstudy::ViewpointStudyResult* result,
                                          const ViewpointOverlayStyle& style);

// The view field drawn small at the point, so its shape reads at a glance: a
// wire sphere (horizon and two meridians) with the rings where an elevation
// band is cut off drawn heavier in `bandRgba`.
constexpr double kViewGlyphMetres = 1.0;
// The isovist's glyph is blue whatever the edge colours: the default yellow
// guide barely showed against a lit model.
constexpr uint32_t kViewSphereGlyphRgba = 0x2F7BFFC8u;
constexpr uint32_t kViewSphereBandRgba = 0x2F7BFFFFu;
void AppendViewSphereGlyph (const std::array<double, 3>& centre, double radius, double minElevationDegrees,
                            double maxElevationDegrees, uint32_t rgba, uint32_t bandRgba, GhPreviewBucket& bucket);
// The shared point's Archicad path (AnalysisPath.hpp), teal: apart from the
// point's yellow, the target's blue and the role outlines' violet and magenta.
constexpr uint32_t kAnalysisPathRgba = 0x2EC4A6E6u;
void AppendPolyline (const std::vector<std::array<double, 3>>& points, uint32_t rgba, GhPreviewBucket& bucket);
void AppendGuideSegment (const std::array<double, 3>& a, const std::array<double, 3>& b, uint32_t rgba,
                         GhPreviewBucket& bucket);

// Point visibility's guides, all world-space and depth-tested like the isovist.
// Plain data, so the overlay can key its rebuild on the bytes.
struct PointVisibilityGuide {
    double point[3] = {};
    double direction[3] = { 1.0, 0.0, 0.0 }; // cone: unit, toward the target
    double coneDegrees = 90.0;
    double minElevationDegrees = -90.0, maxElevationDegrees = 90.0; // sphere
    double target[3] = {};
    bool sphere = false;
    bool hasTarget = false;
};
constexpr uint32_t kPointVisibilityGuideRgba = 0x4B8358E6u;
// A cone (a ring 1 m out, four rays from the apex, the line to the target) or a
// sphere glyph at the point.
GhPreviewDrawables BuildPointVisibilityGuides (const PointVisibilityGuide& guide);

} // namespace geomsrv::archviz

#endif
