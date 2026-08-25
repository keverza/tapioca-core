#ifndef EVP_ARCHVIZ_TRACEANNOTATIONLAYER_HPP
#define EVP_ARCHVIZ_TRACEANNOTATIONLAYER_HPP

#include "Annotation/DrawList.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::archviz {

struct ScreenPoint {
    float x = 0.0f;
    float y = 0.0f;
};

struct ScreenLine {
    ScreenPoint from;
    ScreenPoint to;
    uint32_t rgba = 0xFFFFFFFFu;
    float width = 2.0f;
};

struct ScreenTriangle {
    ScreenPoint points[3];
    uint32_t rgba = 0xFFFFFFFFu;
};

struct ScreenLabel {
    ScreenPoint anchor;
    std::string text;
    uint32_t rgba = 0xFFFFFFFFu;
    float fontSize = 0.0f;
    bool centered = false;
};

struct ProjectedDrawList {
    std::vector<ScreenLine> lines;
    std::vector<ScreenTriangle> triangles;
    std::vector<ScreenLabel> labels;
};

// Adds a uniform screen-space fit to a world projection using this frame only.
// The source matrix and output both use ArchViz's row-vector convention.
bool FitFrameProjection (const annotation::Frame& frame, const float viewProj[16], uint32_t width, uint32_t height,
                         float marginPixels, float fittedViewProj[16]);

// Projects one retained watch frame using ArchViz's row-vector view-projection.
// D3D clip depth is [0,w], and returned screen y grows down from the top edge.
ProjectedDrawList BuildTraceAnnotations (const annotation::Frame& frame, const float viewProj[16], uint32_t width,
                                         uint32_t height, float dpiScale = 1.0f, bool fitSelectedFrame = false);

} // namespace geomsrv::archviz

#endif
