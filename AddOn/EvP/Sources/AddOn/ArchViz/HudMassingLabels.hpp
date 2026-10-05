#ifndef EVP_ARCHVIZ_HUDMASSINGLABELS_HPP
#define EVP_ARCHVIZ_HUDMASSINGLABELS_HPP

#include "ArchViz/TraceAnnotationLayer.hpp"
#include <imgui.h>

namespace geomsrv::archviz::hudmassingrules {
struct DiagramLabel {
    ScreenPoint anchor;
    ScreenPoint direction { 1, 0 };
    std::string text;
    uint32_t rgba = 0xFFFFFFFF;
};
// Pure screen-space candidates; accepted labels reserve their rotated bounds.
// No overlap fallback: a crowded label remains accessible through the contour tooltip.
std::optional<ScreenLabel> PlaceDiagramLabel (ProjectedDrawList& occupied, const DiagramLabel& input,
                                              ScreenPoint minimum, ScreenPoint maximum, float fontSize,
                                              const ScreenTextMeasure& measure);
// Uses the current HUD font, not the scene-text renderer. Rotation happens before GPU clipping.
void DrawDiagramLabel (ImDrawList& draw, const ScreenLabel& label);
} // namespace geomsrv::archviz::hudmassingrules
#endif
