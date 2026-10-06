#ifndef EVP_ARCHVIZ_HUDMASSINGDIAGRAM_HPP
#define EVP_ARCHVIZ_HUDMASSINGDIAGRAM_HPP
#include "ArchViz/TraceAnnotationLayer.hpp"
#include <imgui.h>

namespace geomsrv::archviz::hudmassingrules {
// Property boundary: long dash, gap, round dot, gap; arc persists across segments.
void DrawDiagramProperty (ImDrawList& draw, ScreenPoint from, ScreenPoint to, float width, float fontSize, double& arc,
                          ProjectedDrawList* occupied = nullptr);
// Closed inset path; dash phase continues across corners. Collision reservations
// cover the full path, not only its visible dashes.
void DrawDiagramOffset (ImDrawList& draw, const std::vector<ScreenPoint>& points, float fontSize,
                        ProjectedDrawList* occupied = nullptr);
// zero[i] applies to points[i] -> points[i+1]. The warm band is clipped to the
// parcel's exterior, independent of winding; the caller draws red edges on top.
void DrawDiagramZeroOffset (ImDrawList& draw, const std::vector<ScreenPoint>& points, const std::vector<bool>& zero,
                            float fontSize);
} // namespace geomsrv::archviz::hudmassingrules
#endif
