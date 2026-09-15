#include "ArchViz/AnnotationHudControls.hpp"

#include "ArchViz/DiligentHud.hpp"

#include <imgui.h>

#include <algorithm>

namespace geomsrv::archviz {

void DrawAnnotationHudControls (HudState& state)
{
    ImGui::Checkbox ("dimensions on edge hover (0.5 s)", &state.annotationDimensionsOnHover);
    ImGui::SliderFloat ("annotation model text height", &state.annotationTextHeightMetres, 0.02f, 0.50f, "%.2f m");
    ImGui::SliderFloat ("dimension offset", &state.annotationDimensionOffsetMetres, 0.02f, 1.00f, "%.2f m");
    ImGui::SliderFloat ("witness start gap", &state.annotationWitnessStartGapMetres, 0.0f, 0.20f, "%.2f m");
    ImGui::SliderFloat ("witness overhang", &state.annotationWitnessOverhangMetres, 0.0f, 0.20f, "%.2f m");
    ImGui::SliderFloat ("annotation hide below", &state.annotationHideBelowPixels, 2.0f, 24.0f, "%.0f px");
    ImGui::SliderFloat ("annotation cap above", &state.annotationCapAbovePixels, 12.0f, 96.0f, "%.0f px");
    state.annotationCapAbovePixels = std::max (state.annotationCapAbovePixels, state.annotationHideBelowPixels);
}

annotation::DimensionStyle AnnotationDimensionStyle (const HudState& state)
{
    annotation::DimensionStyle style;
    style.textHeightModel = state.annotationTextHeightMetres;
    style.dimensionOffset = state.annotationDimensionOffsetMetres;
    style.witnessStartGap = state.annotationWitnessStartGapMetres;
    style.witnessOverhang = state.annotationWitnessOverhangMetres;
    style.hideBelowPixels = state.annotationHideBelowPixels;
    style.capAbovePixels = state.annotationCapAbovePixels;
    return style;
}

} // namespace geomsrv::archviz
