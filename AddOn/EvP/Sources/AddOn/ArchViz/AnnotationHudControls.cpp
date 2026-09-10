#include "ArchViz/AnnotationHudControls.hpp"

#include "ArchViz/DiligentHud.hpp"

#include <imgui.h>

#include <algorithm>

namespace geomsrv::archviz {

void DrawAnnotationHudControls (HudState& state)
{
    ImGui::SliderFloat ("annotation model text height", &state.annotationTextHeightMetres, 0.02f, 0.50f, "%.2f m");
    ImGui::SliderFloat ("annotation hide below", &state.annotationHideBelowPixels, 2.0f, 24.0f, "%.0f px");
    ImGui::SliderFloat ("annotation cap above", &state.annotationCapAbovePixels, 12.0f, 96.0f, "%.0f px");
    state.annotationCapAbovePixels = std::max (state.annotationCapAbovePixels, state.annotationHideBelowPixels);
}

} // namespace geomsrv::archviz
