#include "ArchViz/ImGuiGraphInteractionLab.hpp"

#include "ArchViz/InputRingBuffer.hpp"

#include <windows.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace geomsrv::archviz {

namespace {

struct NodeRect {
    ImVec2 min;
    ImVec2 max;
};

NodeRect DrawNumberNode (float& number)
{
    ImGui::SetNextWindowPos ({ 360.0f, 110.0f }, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize ({ 220.0f, 0.0f }, ImGuiCond_FirstUseEver);
    ImGui::Begin ("Number##graph-lab-number", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextDisabled ("drag this panel by its title");
    ImGui::SetNextItemWidth (180.0f);
    ImGui::DragFloat ("value", &number, 0.05f, -1000.0f, 1000.0f, "%.2f");
    const ImVec2 position = ImGui::GetWindowPos ();
    const ImVec2 size = ImGui::GetWindowSize ();
    const NodeRect rect { position, { position.x + size.x, position.y + size.y } };
    ImGui::End ();
    return rect;
}

NodeRect DrawScaleNode (float& multiplier, int& repetitions, bool& clamp)
{
    ImGui::SetNextWindowPos ({ 650.0f, 250.0f }, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize ({ 235.0f, 0.0f }, ImGuiCond_FirstUseEver);
    ImGui::Begin ("Scale##graph-lab-scale", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextDisabled ("every control updates immediately");
    ImGui::SetNextItemWidth (180.0f);
    ImGui::SliderFloat ("factor", &multiplier, -10.0f, 10.0f, "%.2f");
    ImGui::SetNextItemWidth (180.0f);
    ImGui::InputInt ("copies", &repetitions);
    repetitions = std::clamp (repetitions, 1, 100);
    ImGui::Checkbox ("clamp to 100", &clamp);
    const ImVec2 position = ImGui::GetWindowPos ();
    const ImVec2 size = ImGui::GetWindowSize ();
    const NodeRect rect { position, { position.x + size.x, position.y + size.y } };
    ImGui::End ();
    return rect;
}

NodeRect DrawResultNode (float result)
{
    ImGui::SetNextWindowPos ({ 970.0f, 150.0f }, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize ({ 190.0f, 0.0f }, ImGuiCond_FirstUseEver);
    ImGui::Begin ("Result##graph-lab-result", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text ("%.2f", result);
    ImGui::TextDisabled ("live graph output");
    const ImVec2 position = ImGui::GetWindowPos ();
    const ImVec2 size = ImGui::GetWindowSize ();
    const NodeRect rect { position, { position.x + size.x, position.y + size.y } };
    ImGui::End ();
    return rect;
}

void DrawLink (ImDrawList* drawList, const NodeRect& from, const NodeRect& to)
{
    const ImVec2 a { from.max.x, (from.min.y + from.max.y) * 0.5f };
    const ImVec2 b { to.min.x, (to.min.y + to.max.y) * 0.5f };
    const float bend = (std::max) (50.0f, std::abs (b.x - a.x) * 0.45f);
    drawList->AddBezierCubic (a, { a.x + bend, a.y }, { b.x - bend, b.y }, b, IM_COL32 (75, 190, 255, 255), 3.0f);
    drawList->AddCircleFilled (a, 5.0f, IM_COL32 (75, 190, 255, 255));
    drawList->AddCircleFilled (b, 5.0f, IM_COL32 (75, 190, 255, 255));
}

} // namespace

void ImGuiGraphInteractionLab::Draw (uint32_t width, uint32_t height, const InputSnapshot& input, uint32_t frameLatency,
                                     bool& open)
{
    if (input.eventTickMs != 0 && input.eventSequence != lastEventSequence_) {
        lastEventSequence_ = input.eventSequence;
        const uint64_t now = ::GetTickCount64 ();
        lastEventLatencyMs_ = now >= input.eventTickMs ? now - input.eventTickMs : 0;
    }

    ImGui::SetNextWindowPos ({ float (width) - 12.0f, float (height) - 12.0f }, ImGuiCond_FirstUseEver, { 1.0f, 1.0f });
    ImGui::SetNextWindowSize ({ 300.0f, 0.0f }, ImGuiCond_FirstUseEver);
    if (ImGui::Begin ("Graph interaction lab", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped (
            "Move all three node panels. Drag the slider, click the integer +/- buttons and toggle the checkbox.");
        ImGui::Separator ();
        ImGui::Text ("input -> ImGui frame  %llu ms", (unsigned long long) lastEventLatencyMs_);
        ImGui::Text ("DXGI queue            %u frame%s", frameLatency, frameLatency == 1 ? "" : "s");
        ImGui::Text ("ImGui frame           %.2f ms", ImGui::GetIO ().DeltaTime * 1000.0f);
        if (frameLatency != 1)
            ImGui::TextColored ({ 1.0f, 0.55f, 0.25f, 1.0f }, "LOW-LATENCY QUEUE NOT ACTIVE");
        ImGui::TextDisabled ("wheel test");
        if (ImGui::BeginChild ("##graph-lab-wheel", { 0.0f, 78.0f }, true)) {
            for (int row = 1; row <= 20; ++row)
                ImGui::Text ("scroll row %02d", row);
        }
        ImGui::EndChild ();
    }
    ImGui::End ();

    if (!open)
        return;

    const NodeRect numberNode = DrawNumberNode (number_);
    const NodeRect scaleNode = DrawScaleNode (multiplier_, repetitions_, clamp_);
    float result = number_ * multiplier_ * float (repetitions_);
    if (clamp_)
        result = std::clamp (result, -100.0f, 100.0f);
    const NodeRect resultNode = DrawResultNode (result);
    ImDrawList* links = ImGui::GetBackgroundDrawList ();
    DrawLink (links, numberNode, scaleNode);
    DrawLink (links, scaleNode, resultNode);
}

} // namespace geomsrv::archviz
