#include "ArchViz/DiligentHudSunPeriodLegend.hpp"

namespace geomsrv::archviz {

namespace {
// One legend entry: its swatch and word, its meaning on hover.
void LegendItem (ImU32 colour, const char* text, const char* meaning)
{
    const ImVec2 at = ImGui::GetCursorScreenPos ();
    const float size = ImGui::GetTextLineHeight () * 0.7f;
    ImGui::GetWindowDrawList ()->AddRectFilled (ImVec2 (at.x, at.y + size * 0.2f),
                                                ImVec2 (at.x + size, at.y + size * 1.2f), colour);
    ImGui::Dummy (ImVec2 (size, size));
    ImGui::SameLine (0.0f, 4.0f);
    ImGui::TextDisabled ("%s", text);
    if (meaning != nullptr && ImGui::IsItemHovered ())
        ImGui::SetTooltip ("%s", meaning);
    ImGui::SameLine ();
}

constexpr const char* kMeanDirect = "Direct sun: hours with a clear line to the sun.";
constexpr const char* kMeanFront = "In front: hours the sun is on the surface's side -- the most it could get with "
                                   "nothing in the way. Depends only on orientation.";
constexpr const char* kMeanUp = "Sun up: hours the sun is above the horizon in the study's time slice. The same for "
                                "every surface, so it is the ceiling, not a property of the point.";
constexpr const char* kMeanLost = "Lost: hours the sun was in front of the surface but the model blocked it.";
constexpr const char* kMeanAccess = "Access: direct sun as a share of the sun in front -- how much of what it could "
                                    "get it actually gets.";
} // namespace

void DrawSunStudyPeriodLegend (int quantity, ImU32 rampColour, ImU32 ghost)
{
    switch (quantity) {
        case 1:
            LegendItem (rampColour, "in front", kMeanFront);
            LegendItem (ghost, "sun up", kMeanUp);
            break;
        case 2:
            LegendItem (rampColour, "lost", kMeanLost);
            LegendItem (ghost, "in front", kMeanFront);
            break;
        case 3:
            LegendItem (rampColour, "access %", kMeanAccess);
            break;
        case 4:
            LegendItem (rampColour, "days at or above", "Days whose direct sun reached the threshold (line).");
            LegendItem (ghost, "below", "Days below the threshold.");
            break;
        default:
            LegendItem (rampColour, "direct", kMeanDirect);
            LegendItem (ghost, "in front", kMeanFront);
            break;
    }
    ImGui::NewLine ();
}

} // namespace geomsrv::archviz
