#include "ArchViz/TapiocaUI/Foundation/AnalysisRunControl.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tapioca::ui {

AnalysisRunAction AnalysisRunControl (const char* id, const AnalysisRunView& view)
{
    using Stage = AnalysisRunStage;
    const bool active = view.stage != Stage::Idle && view.stage != Stage::Failed;
    const bool enabled = view.enabled && view.stage != Stage::Cancelling && (!active || view.cancellable);
    const auto& style = ImGui::GetStyle ();
    const ImVec2 size ((std::max) (1.0f, ImGui::GetContentRegionAvail ().x),
                       2.0f * ImGui::GetTextLineHeight () + 2.0f * style.FramePadding.y);
    const ImVec2 start = ImGui::GetCursorScreenPos ();
    const ImVec2 end (start.x + size.x, start.y + size.y);
    ImGui::BeginDisabled (!enabled);
    const bool pressed = ImGui::InvisibleButton (id, size, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered ();
    const bool focused = ImGui::IsItemFocused ();
    const bool keyboard =
        focused && (ImGui::IsKeyPressed (ImGuiKey_Enter, false) || ImGui::IsKeyPressed (ImGuiKey_Space, false));
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    draw->AddRectFilled (start, end, ImGui::GetColorU32 (ImGuiCol_Button), style.FrameRounding);
    draw->PushClipRect (start, end, true);
    const bool measured = view.total > 0 && (view.stage == Stage::Calculating || view.stage == Stage::Cancelling);
    if (measured) {
        const float fraction = std::clamp (float (view.resolved) / float (view.total), 0.0f, 1.0f);
        draw->AddRectFilled (start, ImVec2 (start.x + size.x * fraction, end.y),
                             ImGui::GetColorU32 (ImGuiCol_PlotHistogram), style.FrameRounding);
    }
    else if (active && view.stage != Stage::Cancelling) {
        const float phase = float (std::fmod (ImGui::GetTime () * 0.6, 1.25)) - 0.25f;
        draw->AddRectFilled (ImVec2 (start.x + size.x * phase, start.y),
                             ImVec2 (start.x + size.x * (phase + 0.25f), end.y),
                             ImGui::GetColorU32 (ImGuiCol_PlotHistogram), style.FrameRounding);
    }
    char progress[80];
    const char* label = view.runLabel;
    switch (view.stage) {
        case Stage::Waiting:
            label = "Waiting...";
            break;
        case Stage::Capturing:
            label = "Capturing...";
            break;
        case Stage::Preparing:
            label = "Preparing...";
            break;
        case Stage::Publishing:
            label = "Publishing...";
            break;
        case Stage::Cancelling:
            label = "Cancelling...";
            break;
        case Stage::Calculating:
            if (view.total > 0) {
                std::snprintf (progress, sizeof (progress), "%zu / %zu steps", view.resolved, view.total);
                label = progress;
            }
            else
                label = "Calculating...";
            break;
        case Stage::Idle:
        case Stage::Failed:
            break;
    }
    const char* action = active && view.stage != Stage::Cancelling && view.cancellable ? "Cancel" : "";
    const auto text = [&] (const char* value, float y) {
        const float width = ImGui::CalcTextSize (value).x;
        draw->AddText (ImVec2 (start.x + (size.x - width) * 0.5f, y), ImGui::GetColorU32 (ImGuiCol_Text), value);
    };
    text (label, start.y + style.FramePadding.y + (action[0] == 0 ? ImGui::GetTextLineHeight () * 0.5f : 0.0f));
    if (action[0] != 0)
        text (action, start.y + style.FramePadding.y + ImGui::GetTextLineHeight ());
    draw->PopClipRect ();
    const ImU32 border = hovered && active ? IM_COL32 (173, 90, 90, 255)
                                           : ImGui::GetColorU32 (focused ? ImGuiCol_NavCursor : ImGuiCol_Border);
    draw->AddRect (start, end, border, style.FrameRounding, 0, hovered || focused ? 2.0f : 1.0f);
    ImGui::EndDisabled ();
    if ((!pressed && !keyboard) || !enabled)
        return AnalysisRunAction::None;
    return active ? AnalysisRunAction::Cancel : AnalysisRunAction::Run;
}

} // namespace tapioca::ui
