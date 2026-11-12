#ifndef TAPIOCA_ARCHVIZ_SUNPATHLABELS_HPP
#define TAPIOCA_ARCHVIZ_SUNPATHLABELS_HPP

// ArchViz/SunPathLabels -- the sunpath's text, hidden by the model (#182).
//
// The HUD publishes world-anchored specs; the frame projects them with the
// CURRENT camera and draws them through SceneTextLayer::DrawProjected, which
// tests each fragment against the scene depth. Projecting at draw time means
// the text never trails the line work by a frame while the camera moves.
// Pure: no device, no ImGui.

#include "ArchViz/TraceAnnotationLayer.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::archviz {

struct SunPathLabelSpec {
    double anchor[3] = {};
    // Non-zero: a world displacement along which the text runs (the compass
    // ring's tangent). The text follows its projection, with no upright flip.
    double tangent[3] = {};
    std::string text;
    uint32_t rgba = 0x505058FFu;
    // No halo by default: this shader adds the halo width straight to the MSDF
    // coverage, so even 1.25 filled each glyph quad as a white block (#182).
    uint32_t haloRgba = 0;
    float sizePoints = 13.0f; // scaled by the DPI at projection
    // Centred on the anchor (compass letters), or left-aligned with its bottom
    // just above it (hour numbers, the tooltip's lines).
    bool centered = true;
    float offsetX = 0.0f, offsetY = 0.0f; // pixels, before DPI scaling
    bool panel = false;                   // a light plate behind the text
};

// Specs behind the camera or outside the clip volume are dropped. Every label
// hides where the scene is in front of its anchor and keeps its own place.
std::vector<ScreenLabel> ProjectSunPathLabels (const std::vector<SunPathLabelSpec>& specs, const float viewProj[16],
                                               uint32_t width, uint32_t height, float dpiScale);

} // namespace geomsrv::archviz

#endif
