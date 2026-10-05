#ifndef EVP_ARCHVIZ_HUDMASSINGDIAGRAM_HPP
#define EVP_ARCHVIZ_HUDMASSINGDIAGRAM_HPP
#include "ArchViz/TraceAnnotationLayer.hpp"
#include <imgui.h>

namespace geomsrv::archviz::hudmassingrules {
// Closed inset path; dash phase continues across corners. Collision reservations
// cover the full path, not only its visible dashes.
void DrawDiagramOffset (ImDrawList& draw, const std::vector<ScreenPoint>& points, float fontSize,
                        ProjectedDrawList* occupied = nullptr);
} // namespace geomsrv::archviz::hudmassingrules
#endif
