#include "ArchViz/ImGuiGraphInteractionLab.hpp"

#include "ArchViz/InputRingBuffer.hpp"
#include "Python/PathUtils.hpp"

#include <windows.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace geomsrv::archviz {

namespace {

uint64_t QpcNow ()
{
    LARGE_INTEGER value = {};
    return ::QueryPerformanceCounter (&value) ? uint64_t (value.QuadPart) : 0;
}

uint64_t QpcFrequency ()
{
    static const uint64_t frequency = [] () {
        LARGE_INTEGER value = {};
        return ::QueryPerformanceFrequency (&value) ? uint64_t (value.QuadPart) : 0;
    }();
    return frequency;
}

uint64_t QpcDiffUs (uint64_t newer, uint64_t older)
{
    const uint64_t frequency = QpcFrequency ();
    if (frequency == 0 || newer < older || older == 0)
        return 0;
    return uint64_t ((double (newer - older) * 1000000.0) / double (frequency));
}

double Hertz (uint64_t gapUs)
{
    return gapUs > 0 ? 1000000.0 / double (gapUs) : 0.0;
}

struct NodeRect {
    ImVec2 min;
    ImVec2 max;
    bool dragging = false;
};

void DrawLink (ImDrawList* drawList, const NodeRect& from, const NodeRect& to);

struct CanvasInteraction {
    bool panning = false;
    bool zoomed = false;
    int wireDrop = 0; // 1 connected, -1 cancelled
};

CanvasInteraction DrawInteractionCanvas (float& panX, float& panY, float& zoom, bool& wireDragging)
{
    CanvasInteraction interaction;
    ImGui::SetNextWindowPos ({ 350.0f, 430.0f }, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize ({ 420.0f, 220.0f }, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin ("Graph canvas benchmark")) {
        ImGui::End ();
        return interaction;
    }
    ImGui::TextDisabled ("right-drag pan | wheel zoom | drag blue socket to green");
    const ImVec2 canvasMin = ImGui::GetCursorScreenPos ();
    const ImVec2 canvasSize { ImGui::GetContentRegionAvail ().x, ImGui::GetContentRegionAvail ().y };
    const ImVec2 canvasMax { canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y };
    ImGui::InvisibleButton ("##graph-canvas", canvasSize,
                            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool canvasHovered = ImGui::IsItemHovered ();
    if (ImGui::IsItemActive () && ImGui::IsMouseDragging (ImGuiMouseButton_Right)) {
        panX += ImGui::GetIO ().MouseDelta.x;
        panY += ImGui::GetIO ().MouseDelta.y;
        interaction.panning = true;
    }
    if (canvasHovered && ImGui::GetIO ().MouseWheel != 0.0f) {
        zoom = std::clamp (zoom * std::pow (1.1f, ImGui::GetIO ().MouseWheel), 0.25f, 4.0f);
        interaction.zoomed = true;
    }

    ImDrawList* draw = ImGui::GetWindowDrawList ();
    draw->AddRectFilled (canvasMin, canvasMax, IM_COL32 (22, 25, 31, 255));
    draw->PushClipRect (canvasMin, canvasMax, true);
    const float spacing = 32.0f * zoom;
    if (spacing >= 8.0f) {
        for (float x = std::fmod (panX, spacing); x < canvasSize.x; x += spacing)
            draw->AddLine ({ canvasMin.x + x, canvasMin.y }, { canvasMin.x + x, canvasMax.y },
                           IM_COL32 (48, 53, 64, 255));
        for (float y = std::fmod (panY, spacing); y < canvasSize.y; y += spacing)
            draw->AddLine ({ canvasMin.x, canvasMin.y + y }, { canvasMax.x, canvasMin.y + y },
                           IM_COL32 (48, 53, 64, 255));
    }
    const ImVec2 source { canvasMin.x + 90.0f + panX, canvasMin.y + 75.0f + panY };
    const ImVec2 target { canvasMin.x + 300.0f + panX, canvasMin.y + 120.0f + panY };
    draw->AddRectFilled ({ source.x - 55.0f, source.y - 30.0f }, { source.x, source.y + 30.0f },
                         IM_COL32 (55, 64, 82, 255), 5.0f);
    draw->AddRectFilled ({ target.x, target.y - 30.0f }, { target.x + 55.0f, target.y + 30.0f },
                         IM_COL32 (55, 64, 82, 255), 5.0f);

    const ImVec2 mouse = ImGui::GetIO ().MousePos;
    const float sourceDx = mouse.x - source.x;
    const float sourceDy = mouse.y - source.y;
    if (canvasHovered && sourceDx * sourceDx + sourceDy * sourceDy <= 100.0f &&
        ImGui::IsMouseClicked (ImGuiMouseButton_Left))
        wireDragging = true;
    const float targetDx = mouse.x - target.x;
    const float targetDy = mouse.y - target.y;
    const bool targetHovered = targetDx * targetDx + targetDy * targetDy <= 100.0f;
    const ImVec2 wireEnd = wireDragging ? ImGui::GetIO ().MousePos : target;
    DrawLink (draw, { source, source }, { wireEnd, wireEnd });
    if (wireDragging && !ImGui::IsMouseDown (ImGuiMouseButton_Left)) {
        interaction.wireDrop = targetHovered ? 1 : -1;
        wireDragging = false;
    }
    draw->AddCircleFilled (source, 7.0f, IM_COL32 (75, 190, 255, 255));
    draw->AddCircleFilled (target, 7.0f, IM_COL32 (80, 220, 130, 255));
    draw->PopClipRect ();
    ImGui::SetCursorScreenPos ({ canvasMin.x, canvasMax.y });
    ImGui::End ();
    return interaction;
}

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
    const bool dragging = ImGui::IsWindowHovered () && ImGui::IsMouseDragging (ImGuiMouseButton_Left) &&
                          ImGui::GetIO ().MousePos.y <= position.y + ImGui::GetFrameHeight ();
    const NodeRect rect { position, { position.x + size.x, position.y + size.y }, dragging };
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
    const bool dragging = ImGui::IsWindowHovered () && ImGui::IsMouseDragging (ImGuiMouseButton_Left) &&
                          ImGui::GetIO ().MousePos.y <= position.y + ImGui::GetFrameHeight ();
    const NodeRect rect { position, { position.x + size.x, position.y + size.y }, dragging };
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
    const bool dragging = ImGui::IsWindowHovered () && ImGui::IsMouseDragging (ImGuiMouseButton_Left) &&
                          ImGui::GetIO ().MousePos.y <= position.y + ImGui::GetFrameHeight ();
    const NodeRect rect { position, { position.x + size.x, position.y + size.y }, dragging };
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

struct ImGuiGraphInteractionLab::Telemetry {
    GS::UniString path;
    std::vector<std::string> pending;
    bool running = false;

    void Flush ()
    {
        if (pending.empty () || path.IsEmpty ())
            return;
        std::string block;
        for (const std::string& row : pending) {
            block += row;
            block += "\r\n";
        }
        if (evp::AppendTextLine (path, GS::UniString (block.c_str (), CC_UTF8)))
            pending.clear ();
    }

    bool Start ()
    {
        pending.clear ();
        pending.push_back ("");
        pending.push_back ("# ---- ImGui interaction telemetry, new session ----");
        pending.push_back (
            "qpc_us,pointer_seq,wm_pointer_seq,screen_x,screen_y,client_x,client_y,wm_x,wm_y,imgui_x,imgui_y,inside,"
            "buttons,wheel,transitions,native_event_seq,native_event_age_us,pointer_sample_age_us,pointer_sample_gap_"
            "us,"
            "wm_age_us,wm_gap_us,pointer_move_gap_us,imgui_frame_gap_us,imgui_delta_us,mouse_delta_x,mouse_delta_y,"
            "item_active,dxgi_max_frame_latency,client_width,client_height,dpi,visible,host_minimized,host_toolwindow,"
            "host_style,host_exstyle,monitor,host_x,host_y,host_width,host_height,gesture");
        running = true;
        return true;
    }

    void Stop ()
    {
        if (!running)
            return;
        pending.push_back ("# ---- end ImGui interaction telemetry ----");
        const GS::UniString dataDir = evp::EvpDataDir ();
        if (!dataDir.IsEmpty ()) {
            evp::CreateDirectoryChain (dataDir + GS::UniString ("\\logs"));
            path = dataDir + GS::UniString ("\\logs\\imgui_interaction.csv");
        }
        Flush ();
        running = false;
    }

    void Write (const std::string& row)
    {
        if (!running)
            return;
        // No file I/O during a run: even batched synchronous writes create a
        // periodic hitch in the cadence being measured. 100k rows is roughly
        // 28 minutes at 60 Hz and bounds memory if Stop is forgotten.
        if (pending.size () < 100000)
            pending.push_back (row);
    }
};

ImGuiGraphInteractionLab::ImGuiGraphInteractionLab () : telemetry_ (std::make_unique<Telemetry> ())
{
}

ImGuiGraphInteractionLab::~ImGuiGraphInteractionLab ()
{
    telemetry_->Stop ();
}

void ImGuiGraphInteractionLab::Draw (uint32_t width, uint32_t height, const InputSnapshot& input, uint32_t frameLatency,
                                     bool& open, bool& fastPath)
{
    const uint64_t nowQpc = QpcNow ();
    frameGapUs_ = QpcDiffUs (nowQpc, lastFrameQpc_);
    lastFrameQpc_ = nowQpc;
    pointerSampleAgeUs_ = QpcDiffUs (nowQpc, input.pointerQpc);
    pointerSampleGapUs_ = QpcDiffUs (input.pointerQpc, lastPointerQpc_);
    lastPointerQpc_ = input.pointerQpc;
    pointerMessageAgeUs_ = QpcDiffUs (nowQpc, input.pointerMessageQpc);
    if (input.pointerMessageQpc != lastPointerMessageQpc_) {
        pointerMessageGapUs_ = QpcDiffUs (input.pointerMessageQpc, lastPointerMessageQpc_);
        lastPointerMessageQpc_ = input.pointerMessageQpc;
    }
    const bool pointerMoved = input.x != lastPointerX_ || input.y != lastPointerY_;
    if (pointerMoved) {
        pointerMoveGapUs_ = QpcDiffUs (input.pointerQpc, lastMoveQpc_);
        lastMoveQpc_ = input.pointerQpc;
        lastPointerX_ = input.x;
        lastPointerY_ = input.y;
    }
    if (input.eventQpc != 0 && input.eventSequence != lastEventSequence_) {
        lastEventSequence_ = input.eventSequence;
        lastEventLatencyUs_ = QpcDiffUs (nowQpc, input.eventQpc);
    }

    ImGui::SetNextWindowPos ({ float (width) - 12.0f, float (height) - 12.0f }, ImGuiCond_FirstUseEver, { 1.0f, 1.0f });
    ImGui::SetNextWindowSize ({ 300.0f, 0.0f }, ImGuiCond_FirstUseEver);
    if (ImGui::Begin ("Graph interaction lab", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped (
            "Move all three node panels. Drag the slider, click the integer +/- buttons and toggle the checkbox.");
        ImGui::Separator ();
        const ImVec2 imguiPointer = ImGui::GetIO ().MousePos;
        ImGui::Text ("OS GetCursorPos %d, %d px", input.screenX, input.screenY);
        ImGui::Text ("WM_MOUSEMOVE    %d, %d client px", input.messageX, input.messageY);
        ImGui::Text ("provided client %d, %d physical px", input.x, input.y);
        ImGui::Text ("ImGui consumed  %.0f, %.0f px", imguiPointer.x, imguiPointer.y);
        ImGui::Text ("WM messages     %.1f Hz  age %.3f ms", Hertz (pointerMessageGapUs_),
                     double (pointerMessageAgeUs_) / 1000.0);
        ImGui::Text ("pointer sample  %.1f Hz  age %.3f ms", Hertz (pointerSampleGapUs_),
                     double (pointerSampleAgeUs_) / 1000.0);
        ImGui::Text ("ImGui frames    %.1f Hz  gap %.3f ms", Hertz (frameGapUs_), double (frameGapUs_) / 1000.0);
        ImGui::Text ("native event -> UI %.3f ms", double (lastEventLatencyUs_) / 1000.0);
        ImGui::Text ("DXGI max latency  %u frame%s", frameLatency, frameLatency == 1 ? "" : "s");
        const bool sixtyHz = Hertz (frameGapUs_) >= 59.5;
        ImGui::TextColored (sixtyHz ? ImVec4 { 0.4f, 1.0f, 0.5f, 1.0f } : ImVec4 { 1.0f, 0.45f, 0.3f, 1.0f },
                            "%s 60 Hz frame cadence", sixtyHz ? "PASS" : "MISS");
        if (frameLatency != 1)
            ImGui::TextColored ({ 1.0f, 0.55f, 0.25f, 1.0f }, "LOW-LATENCY QUEUE NOT ACTIVE");
        ImGui::Checkbox ("UI isolation while interacting", &fastPath);
        if (ImGui::Checkbox ("log interaction telemetry", &telemetryEnabled_)) {
            if (telemetryEnabled_)
                telemetryEnabled_ = telemetry_->Start ();
            else
                telemetry_->Stop ();
        }
        ImGui::TextDisabled ("buffered until off: %%LOCALAPPDATA%%\\Tapioca\\logs\\imgui_interaction.csv");
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

    const CanvasInteraction canvas = DrawInteractionCanvas (canvasPanX_, canvasPanY_, canvasZoom_, wireDragging_);
    const NodeRect numberNode = DrawNumberNode (number_);
    const NodeRect scaleNode = DrawScaleNode (multiplier_, repetitions_, clamp_);
    float result = number_ * multiplier_ * float (repetitions_);
    if (clamp_)
        result = std::clamp (result, -100.0f, 100.0f);
    const NodeRect resultNode = DrawResultNode (result);
    const bool nodeDragging = numberNode.dragging || scaleNode.dragging || resultNode.dragging;
    ImDrawList* links = ImGui::GetBackgroundDrawList ();
    DrawLink (links, numberNode, scaleNode);
    DrawLink (links, scaleNode, resultNode);

    if (telemetryEnabled_) {
        const ImGuiIO& io = ImGui::GetIO ();
        const char* gesture = "idle";
        if (wireDragging_)
            gesture = "wire_drag";
        else if (canvas.wireDrop > 0)
            gesture = "wire_connect";
        else if (canvas.wireDrop < 0)
            gesture = "wire_cancel";
        else if (nodeDragging)
            gesture = "node_drag";
        else if (canvas.panning)
            gesture = "pan";
        else if (canvas.zoomed)
            gesture = "zoom";
        else if (input.wheelDelta != 0)
            gesture = "scroll";
        else if (ImGui::IsMouseDragging (ImGuiMouseButton_Left) && ImGui::IsAnyItemActive ())
            gesture = "control_drag";
        else if (pointerMoved)
            gesture = "pointer_move";
        char row[1024] = {};
        const uint64_t qpcUs = QpcFrequency () > 0 ? uint64_t (double (nowQpc) * 1000000.0 / QpcFrequency ()) : 0;
        std::snprintf (row, sizeof (row),
                       "%llu,%llu,%llu,%d,%d,%d,%d,%d,%d,%.1f,%.1f,%d,%u,%d,%d,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
                       "%.0f,%.1f,%.1f,%d,%u,%d,%d,%u,%d,%d,%d,%llu,%llu,%llu,%d,%d,%d,%d,%s",
                       (unsigned long long) qpcUs, (unsigned long long) input.pointerSequence,
                       (unsigned long long) input.pointerMessageSequence, input.screenX, input.screenY, input.x,
                       input.y, input.messageX, input.messageY, io.MousePos.x, io.MousePos.y, input.inside ? 1 : 0,
                       unsigned (input.buttons), input.wheelDelta, input.transitionCount,
                       (unsigned long long) input.eventSequence, (unsigned long long) lastEventLatencyUs_,
                       (unsigned long long) pointerSampleAgeUs_, (unsigned long long) pointerSampleGapUs_,
                       (unsigned long long) pointerMessageAgeUs_, (unsigned long long) pointerMessageGapUs_,
                       (unsigned long long) pointerMoveGapUs_, (unsigned long long) frameGapUs_,
                       io.DeltaTime * 1000000.0f, io.MouseDelta.x, io.MouseDelta.y, ImGui::IsAnyItemActive () ? 1 : 0,
                       frameLatency, input.clientWidth, input.clientHeight, input.dpi, input.visible ? 1 : 0,
                       input.hostMinimized ? 1 : 0, input.hostToolWindow ? 1 : 0, (unsigned long long) input.hostStyle,
                       (unsigned long long) input.hostExStyle, (unsigned long long) input.monitor, input.hostX,
                       input.hostY, input.hostWidth, input.hostHeight, gesture);
        telemetry_->Write (row);
    }
}

} // namespace geomsrv::archviz
