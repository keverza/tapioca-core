#ifndef EVP_ARCHVIZ_TRACEANNOTATIONLAYER_HPP
#define EVP_ARCHVIZ_TRACEANNOTATIONLAYER_HPP

#include "Annotation/DrawList.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace geomsrv::archviz {

struct ScreenPoint {
    float x = 0.0f;
    float y = 0.0f;
};

struct ScreenTextExtent {
    float width = 0.0f;
    float height = 0.0f;
};

using ScreenTextMeasure = std::function<bool (std::string_view text, float fontSize, ScreenTextExtent& extent)>;
using AnnotationPrimitiveFilter = std::function<bool (std::size_t index, const annotation::Primitive& primitive)>;

struct ScreenLine {
    ScreenPoint from;
    ScreenPoint to;
    uint32_t rgba = 0xFFFFFFFFu;
    float width = 2.0f;
    bool collisionObstacle = true;
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
    uint32_t haloRgba = 0;
    float haloWidthPixels = 0.0f;
    bool backgroundPanel = true;
    float rotationRadians = 0.0f;
    std::size_t ownLineBegin = 0;
    std::size_t ownLineEnd = 0;
    std::size_t sourcePrimitive = 0;
    ScreenPoint depthAnchor;
    float anchorDepth = 1.0f;
    bool fadeWhenOccluded = false;
};

struct ProjectedDrawList {
    std::vector<ScreenLine> lines;
    std::vector<ScreenTriangle> triangles;
    std::vector<ScreenLabel> labels;
};

struct AnnotationPlacementHistory {
    std::shared_ptr<const annotation::DrawList> source;
    std::size_t nodeIndex = 0;
    std::size_t frameIndex = 0;
    std::unordered_map<std::size_t, uint8_t> candidateByPrimitive;
};

struct DimensionHoverState {
    std::shared_ptr<const annotation::DrawList> source;
    std::size_t nodeIndex = 0;
    std::size_t frameIndex = 0;
    std::optional<std::size_t> candidate;
    std::optional<std::size_t> visible;
    std::chrono::steady_clock::time_point startedAt;
};

// Adds a uniform screen-space fit to a world projection using this frame only.
// The source matrix and output both use ArchViz's row-vector convention.
bool FitFrameProjection (const annotation::Frame& frame, const float viewProj[16], uint32_t width, uint32_t height,
                         float marginPixels, float fittedViewProj[16]);

std::optional<std::size_t> HitTestTraceDimension (const annotation::Frame& frame, const float viewProj[16],
                                                  uint32_t width, uint32_t height, float dpiScale,
                                                  bool fitSelectedFrame, const ScreenPoint& cursor,
                                                  float textHeightMetres = 0.18f, float hideBelowPixels = 10.0f,
                                                  float capAbovePixels = 36.0f);

std::optional<std::size_t> UpdateDimensionHover (DimensionHoverState& state,
                                                 const std::shared_ptr<const annotation::DrawList>& source,
                                                 std::size_t nodeIndex, std::size_t frameIndex,
                                                 std::optional<std::size_t> hit, bool eligible,
                                                 std::chrono::steady_clock::time_point now,
                                                 std::chrono::milliseconds delay = std::chrono::milliseconds (500));

// Projects one retained watch frame using ArchViz's row-vector view-projection.
// D3D clip depth is [0,w], and returned screen y grows down from the top edge.
// Annotation furniture follows textHeightMetres in model space, disappears below
// hideBelowPixels, and stops growing above capAbovePixels.
ProjectedDrawList BuildTraceAnnotations (const annotation::Frame& frame, const float viewProj[16], uint32_t width,
                                         uint32_t height, float dpiScale = 1.0f, bool fitSelectedFrame = false,
                                         const ScreenTextMeasure& measureText = {},
                                         AnnotationPlacementHistory* placementHistory = nullptr,
                                         float textHeightMetres = 0.18f, float hideBelowPixels = 10.0f,
                                         float capAbovePixels = 36.0f,
                                         const AnnotationPrimitiveFilter& primitiveFilter = {});

} // namespace geomsrv::archviz

#endif
