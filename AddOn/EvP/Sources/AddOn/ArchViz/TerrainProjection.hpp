#ifndef EVP_ARCHVIZ_TERRAINPROJECTION_HPP
#define EVP_ARCHVIZ_TERRAINPROJECTION_HPP
#include "ArchViz/StorySliceGeometry.hpp"
#include "ArchViz/OverlayLayers.hpp"
#include "Geometry/Mesh.hpp"

namespace geomsrv::archviz::terrainprojection {
struct Style {
    std::string name, category;
    uint32_t fillRgba = 0xAA446528, hatchRgba = 0xAA4465C0;
    float hatchWidthPixels = 0.7f;
    double spacing = 0.7;
};
// Model-XY 45-degree strokes clipped to contours, including holes. Phase uses
// x+y-originSum in metres so plan and terrain projection remain identical.
bool HatchPlan (const std::vector<SliceChain>& chains, double originSum, double z, const Style& style,
                overlaylayers::Layer& layer, std::string& error);
// Upward-facing terrain only; gaps/outside remain undrawn. Alpha-zero fill emits
// strokes only, not a transparent solid highlight or a guessed elevation plane.
bool Project (const std::vector<SliceChain>& chains, double originSum, const Mesh& terrain, const Style& style,
              overlaylayers::Layer& layer, std::string& error);
} // namespace geomsrv::archviz::terrainprojection
#endif
