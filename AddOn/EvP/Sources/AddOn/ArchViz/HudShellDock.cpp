// Shared dock placement and rendering. OVERLAY-INVARIANTS.md: layout only;
// published regions remain the authority for pointer ownership.
#include "ArchViz/HudShell.hpp"
#include "ArchViz/GraphicsSettings.hpp"
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::hudshell {
namespace layers = overlaylayers;
namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kMagnetPixels = 36.0f;
DockEdge PanelSide (ImVec2 view, ImVec2 a, ImVec2 b, DockEdge edge, bool horizontal)
{
    if (edge != DockEdge::Floating)
        return edge;
    return horizontal ? (a.y > view.y - b.y ? DockEdge::Bottom : DockEdge::Top)
                      : (a.x > view.x - b.x ? DockEdge::Right : DockEdge::Left);
}
ImDrawFlags Corners (ImDrawFlags ends, bool open, DockEdge side)
{
    const ImDrawFlags outside =
        side == DockEdge::Top      ? (open ? ImDrawFlags_RoundCornersTopLeft | ImDrawFlags_RoundCornersTopRight
                                           : ImDrawFlags_RoundCornersBottomLeft | ImDrawFlags_RoundCornersBottomRight)
        : side == DockEdge::Bottom ? (open ? ImDrawFlags_RoundCornersBottomLeft | ImDrawFlags_RoundCornersBottomRight
                                           : ImDrawFlags_RoundCornersTopLeft | ImDrawFlags_RoundCornersTopRight)
        : side == DockEdge::Left   ? (open ? ImDrawFlags_RoundCornersTopLeft | ImDrawFlags_RoundCornersBottomLeft
                                           : ImDrawFlags_RoundCornersTopRight | ImDrawFlags_RoundCornersBottomRight)
                                   : (open ? ImDrawFlags_RoundCornersTopRight | ImDrawFlags_RoundCornersBottomRight
                                           : ImDrawFlags_RoundCornersTopLeft | ImDrawFlags_RoundCornersBottomLeft);
    ends &= outside;
    return ends != 0 ? ends : ImDrawFlags_RoundCornersNone;
}
} // namespace

// One dock with surface circles, a workflow-list title and a separate panel arrow.
// Top/bottom edges use a horizontal title; left/right edges turn it vertically.
DockPress DockTab (const char* id, const std::string& label, const layers::Panel& panel, bool open, const Circle& top,
                   const Circle* bottom, ImVec2 padding, float scale, const bool* locked, bool horizontal,
                   DockEdge panelSide)
{
    DockPress press;
    const ImVec2 text = ImGui::CalcTextSize (label.c_str ());
    const float across = std::ceil (text.y + 2.0f * padding.x);
    const float r = (std::min) (float (graphicssettings::Number ("ui.dock.rounding", 6)) * scale, across * 0.5f);
    const uint32_t fill = open ? graphicssettings::Colour ("ui.dock.activeBackground", panel.accentRgba)
                               : graphicssettings::Colour ("ui.dock.background", panel.backgroundRgba);
    const uint32_t ink = open ? graphicssettings::Colour ("ui.dock.activeText", 0xFFFFFFFFu)
                              : graphicssettings::Colour ("ui.dock.text", panel.textRgba);
    const double seconds = ImGui::GetTime ();
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    const ImDrawFlags firstCorners =
        Corners (horizontal ? ImDrawFlags_RoundCornersTopLeft | ImDrawFlags_RoundCornersBottomLeft
                            : ImDrawFlags_RoundCornersTopLeft | ImDrawFlags_RoundCornersTopRight,
                 open, panelSide);
    const ImDrawFlags lastCorners =
        Corners (horizontal ? ImDrawFlags_RoundCornersTopRight | ImDrawFlags_RoundCornersBottomRight
                            : ImDrawFlags_RoundCornersBottomLeft | ImDrawFlags_RoundCornersBottomRight,
                 open, panelSide);
    // One part: its ground, its tint when pointed at and pressed, its edge while closed.
    const auto part = [&] (ImVec2 a, ImVec2 b, ImDrawFlags corners) {
        const bool hovered = ImGui::IsItemHovered (), held = ImGui::IsItemActive ();
        draw->AddRectFilled (a, b, Packed (fill), r, corners);
        if (hovered || held) {
            // White over the open tab, the accent over the closed one.
            const uint32_t tint = graphicssettings::Colour (held ? "ui.dock.pressed" : "ui.dock.hover",
                                                            open ? WithAlpha (0xFFFFFFFFu, held ? 0.30f : 0.18f)
                                                                 : WithAlpha (panel.accentRgba, held ? 0.48f : 0.30f));
            draw->AddRectFilled (a, b, Packed (tint), r, corners);
        }
    };
    // A circle: its square, a disc while its surface is shown or a ring, an arc while busy.
    const auto circle = [&] (const char* name, const Circle& c, ImDrawFlags corners, ImVec2& from, ImVec2& to) {
        const bool pressed = ImGui::InvisibleButton (name, ImVec2 (across, across));
        from = ImGui::GetItemRectMin ();
        to = ImGui::GetItemRectMax ();
        part (from, to, corners);
        const ImVec2 centre (std::floor ((from.x + to.x) * 0.5f), std::floor ((from.y + to.y) * 0.5f));
        const float radius =
            std::floor (across * float (graphicssettings::Number ("ui.dock.circleRadius", 0.22))) + 0.5f;
        const ImU32 colour = Packed (CircleColour (c, ink, seconds));
        const float stroke = (std::max) (1.0f, 1.5f * scale);
        if (c.active)
            draw->AddCircleFilled (centre, radius, colour);
        else
            draw->AddCircle (centre, radius, colour, 0, stroke);
        if (c.phase == Phase::Busy) {
            // How far, clockwise from the top; turning a quarter round when it cannot say.
            const float start =
                c.progress >= 0.0f ? -0.5f * kPi : -0.5f * kPi + float (std::fmod (seconds, 1.0)) * 2.0f * kPi;
            const float sweep = c.progress >= 0.0f ? 2.0f * kPi * (std::min) (c.progress, 1.0f) : 0.5f * kPi;
            if (sweep > 0.0f) {
                draw->PathArcTo (centre, radius + 2.5f * scale, start, start + sweep, 24);
                draw->PathStroke (Packed (graphicssettings::Colour ("ui.dock.busy", kBusyRgba)), 0, stroke);
            }
        }
        // Beside the circle, out over the view: the dock is at the view's right edge.
        Tip (c.tip, TipSide::Left);
        return pressed;
    };
    ImGui::PushID (id);
    ImVec2 topFrom, topTo, bottomFrom, bottomTo;
    press.top = circle ("##shown", top, firstCorners, topFrom, topTo);
    if (horizontal)
        ImGui::SameLine (0, 0);
    if (locked != nullptr) {
        // The lock: a padlock, shut while the view's clicks are the HUD's.
        press.lock = ImGui::InvisibleButton ("##lock", ImVec2 (across, across));
        const ImVec2 from = ImGui::GetItemRectMin (), to = ImGui::GetItemRectMax ();
        part (from, to, ImDrawFlags_RoundCornersNone);
        if (*locked)
            draw->AddRectFilled (from, to, Packed (WithAlpha (panel.accentRgba, 0.55f)), 0.0f);
        const ImVec2 c (std::floor ((from.x + to.x) * 0.5f), std::floor ((from.y + to.y) * 0.5f) + 2.0f * scale);
        const float w = std::floor (across * 0.18f) + 0.5f, h = std::floor (across * 0.14f) + 0.5f;
        const ImU32 colour = Packed (ink);
        const float stroke = (std::max) (1.0f, 1.5f * scale);
        draw->AddRectFilled (ImVec2 (c.x - w, c.y - h * 0.4f), ImVec2 (c.x + w, c.y + h * 1.4f), colour, scale);
        // The shackle: closed over the body while locked, swung up and open otherwise.
        const float lift = *locked ? 0.0f : 2.5f * scale;
        draw->PathArcTo (ImVec2 (c.x, c.y - h * 0.4f - lift), w * 0.7f, kPi, 2.0f * kPi, 12);
        draw->PathStroke (colour, 0, stroke);
        if (!*locked)
            draw->AddLine (ImVec2 (c.x + w * 0.7f, c.y - h * 0.4f - lift), ImVec2 (c.x + w * 0.7f, c.y - h * 0.4f),
                           colour, stroke);
        Tip (*locked ? "Locked: clicks on the view edit the floor plan; the wheel and the middle button navigate. "
                       "Press to give the view back to Archicad."
                     : "Lock the view to edit the floor plan on it: clicks are the HUD's, the wheel and the middle "
                       "button still navigate.",
             TipSide::Left);
    }
    if (horizontal && locked != nullptr)
        ImGui::SameLine (0, 0);
    // Drag the label to move the whole assembly; a click opens workflow choices.
    press.title =
        ImGui::InvisibleButton ("##title", horizontal ? ImVec2 (std::ceil (text.x + 2.0f * padding.y), across)
                                                      : ImVec2 (across, std::ceil (text.x + 2.0f * padding.y)));
    press.titleActive = ImGui::IsItemActive ();
    const ImVec2 a = ImGui::GetItemRectMin (), b = ImGui::GetItemRectMax ();
    part (a, b, ImDrawFlags_RoundCornersNone);
    press.dragging = ImGui::IsItemActive () && ImGui::IsMouseDragging (ImGuiMouseButton_Left);
    if (press.dragging)
        press.delta = ImGui::GetIO ().MouseDelta;
    if (ImGui::GetCurrentContext ()->IO.MouseDragMaxDistanceSqr[0] > 36.0f)
        press.title = false;
    Tip ("Choose a workflow. Drag to move; right-click for dock location.");
    if (horizontal)
        ImGui::SameLine (0, 0);
    press.collapse = ImGui::InvisibleButton ("##collapse", ImVec2 (across, across));
    const ImVec2 arrowMin = ImGui::GetItemRectMin ();
    ImVec2 last = ImGui::GetItemRectMax ();
    part (arrowMin, last, bottom != nullptr ? ImDrawFlags_RoundCornersNone : lastCorners);
    const ImVec2 centre (std::floor ((arrowMin.x + last.x) * 0.5f), std::floor ((arrowMin.y + last.y) * 0.5f));
    const float half = std::floor (across * 0.16f) + 0.5f;
    DockEdge direction = panelSide;
    if (open)
        direction = panelSide == DockEdge::Top      ? DockEdge::Bottom
                    : panelSide == DockEdge::Bottom ? DockEdge::Top
                    : panelSide == DockEdge::Left   ? DockEdge::Right
                                                    : DockEdge::Left;
    ImVec2 arrowA, arrowB, arrowC;
    if (direction == DockEdge::Top) {
        arrowA = { centre.x - half, centre.y - half * 0.5f };
        arrowB = { centre.x + half, centre.y - half * 0.5f };
        arrowC = { centre.x, centre.y + half };
    }
    else if (direction == DockEdge::Bottom) {
        arrowA = { centre.x - half, centre.y + half * 0.5f };
        arrowB = { centre.x + half, centre.y + half * 0.5f };
        arrowC = { centre.x, centre.y - half };
    }
    else if (direction == DockEdge::Left) {
        arrowA = { centre.x - half * 0.5f, centre.y - half };
        arrowB = { centre.x - half * 0.5f, centre.y + half };
        arrowC = { centre.x + half, centre.y };
    }
    else {
        arrowA = { centre.x + half * 0.5f, centre.y - half };
        arrowB = { centre.x + half * 0.5f, centre.y + half };
        arrowC = { centre.x - half, centre.y };
    }
    draw->AddTriangleFilled (arrowA, arrowB, arrowC, Packed (ink));
    Tip (open ? "Collapse panel" : "Expand panel");
    if (bottom != nullptr) {
        if (horizontal)
            ImGui::SameLine (0, 0);
        press.bottom = circle ("##viewer", *bottom, lastCorners, bottomFrom, bottomTo);
        last = bottomTo;
    }
    ImGui::PopID ();
    if (!open) {
        const uint32_t edge = (panel.borderRgba & 0xFFu) != 0 ? panel.borderRgba : WithAlpha (panel.textRgba, 0.25f);
        draw->AddRect (topFrom, last, Packed (edge), r, Corners (ImDrawFlags_RoundCornersAll, false, panelSide),
                       (std::max) (1.0f, scale));
    }
    // Laid out across, round the title's middle, then turned: every corner stays on a
    // whole pixel. Unclipped while across -- the window is as narrow as the text is tall.
    const ImVec2 middle (std::floor ((a.x + b.x) * 0.5f), std::floor ((a.y + b.y) * 0.5f));
    const int first = draw->VtxBuffer.Size;
    draw->PushClipRect (ImVec2 (-32768.0f, -32768.0f), ImVec2 (32768.0f, 32768.0f), false);
    draw->AddText (ImVec2 (middle.x - std::floor (text.x * 0.5f), middle.y - std::floor (text.y * 0.5f)), Packed (ink),
                   label.c_str ());
    // A translucent whole-pixel pass adds a little weight without blending the glyph between pixels.
    draw->AddText (ImVec2 (middle.x - std::floor (text.x * 0.5f) + 1.0f, middle.y - std::floor (text.y * 0.5f)),
                   Packed (WithAlpha (ink, 0.32f)), label.c_str ());
    draw->PopClipRect ();
    for (int k = first; !horizontal && k < draw->VtxBuffer.Size; ++k) {
        ImDrawVert& v = draw->VtxBuffer[k];
        const float dx = v.pos.x - middle.x, dy = v.pos.y - middle.y;
        v.pos = ImVec2 (middle.x - dy, middle.y + dx);
    }
    return press;
}

ImVec2 DockFraction (const DockPlacement& p)
{
    const float along = std::clamp (p.along, 0.0f, 1.0f);
    switch (p.edge) {
        case DockEdge::Top:
            return { along, 0 };
        case DockEdge::Bottom:
            return { along, 1 };
        case DockEdge::Left:
            return { 0, along };
        case DockEdge::Right:
            return { 1, along };
        default:
            return { 0, 0 };
    }
}

ImVec2 DockPosition (const DockPlacement& p, ImVec2 view, ImVec2 size, float scale)
{
    const float x = (std::max) (0.0f, view.x - size.x), y = (std::max) (0.0f, view.y - size.y);
    const float along = std::clamp (p.along, 0.0f, 1.0f);
    switch (p.edge) {
        case DockEdge::Top:
            return { x * along, 0 };
        case DockEdge::Bottom:
            return { x * along, y };
        case DockEdge::Left:
            return { 0, y * along };
        case DockEdge::Right:
            return { x, y * along };
        default:
            return { std::clamp (p.freePosition.x * scale, 0.0f, x), std::clamp (p.freePosition.y * scale, 0.0f, y) };
    }
}

void SnapDock (DockPlacement& p, ImVec2 pos, ImVec2 view, ImVec2 size, float scale)
{
    const float x = (std::max) (0.0f, view.x - size.x), y = (std::max) (0.0f, view.y - size.y);
    pos = { std::clamp (pos.x, 0.0f, x), std::clamp (pos.y, 0.0f, y) };
    p.freePosition = { pos.x / scale, pos.y / scale };
    const float horizontal = (std::min) (pos.y, y - pos.y), vertical = (std::min) (pos.x, x - pos.x);
    p.edge = DockEdge::Floating;
    if ((std::min) (horizontal, vertical) > kMagnetPixels * scale)
        return;
    if (horizontal < vertical || (horizontal == vertical && p.horizontal)) {
        p.edge = pos.y < y * 0.5f ? DockEdge::Top : DockEdge::Bottom;
        p.along = x > 0 ? pos.x / x : 0;
        if (pos.x < 24 * scale)
            p.along = 0;
        if (x - pos.x < 24 * scale)
            p.along = 1;
        p.horizontal = true;
    }
    else {
        p.edge = pos.x < x * 0.5f ? DockEdge::Left : DockEdge::Right;
        p.along = y > 0 ? pos.y / y : 0;
        if (pos.y < 24 * scale)
            p.along = 0;
        if (y - pos.y < 24 * scale)
            p.along = 1;
        p.horizontal = false;
    }
}

ImVec2 MagneticDockPosition (ImVec2 position, ImVec2 view, ImVec2 size, float scale)
{
    const float x = (std::max) (0.0f, view.x - size.x), y = (std::max) (0.0f, view.y - size.y);
    const float reach = kMagnetPixels * scale;
    const auto pull = [reach] (float value, float end) {
        const float distance = std::abs (value - end);
        if (distance >= reach)
            return value;
        const float proximity = 1.0f - distance / reach;
        return value + (end - value) * proximity;
    };
    position.x = std::clamp (position.x, 0.0f, x);
    position.y = std::clamp (position.y, 0.0f, y);
    position.x = pull (position.x, position.x < x * 0.5f ? 0.0f : x);
    position.y = pull (position.y, position.y < y * 0.5f ? 0.0f : y);
    return position;
}

DockPanelRect WorkflowFlyoutAtDock (ImVec2 view, ImVec2 dockMin, ImVec2 dockMax, ImVec2 wanted)
{
    struct Candidate {
        DockEdge side;
        float room;
        float required;
    };
    const Candidate candidates[] = { { DockEdge::Right, view.x - dockMax.x, wanted.x },
                                     { DockEdge::Left, dockMin.x, wanted.x },
                                     { DockEdge::Bottom, view.y - dockMax.y, wanted.y },
                                     { DockEdge::Top, dockMin.y, wanted.y } };
    const bool horizontal = dockMax.x - dockMin.x >= dockMax.y - dockMin.y;
    const Candidate* chosen = &candidates[horizontal ? 0 : 2];
    for (const Candidate& candidate : candidates)
        if ((horizontal == (candidate.side == DockEdge::Right || candidate.side == DockEdge::Left)))
            if ((candidate.room >= candidate.required && chosen->room < chosen->required) ||
                ((candidate.room >= candidate.required) == (chosen->room >= chosen->required) &&
                 candidate.room > chosen->room))
                chosen = &candidate;
    DockPanelRect result;
    result.side = chosen->side;
    result.size = { (std::min) (wanted.x, view.x), (std::min) (wanted.y, view.y) };
    if (chosen->side == DockEdge::Right || chosen->side == DockEdge::Left)
        result.size.x = (std::min) (result.size.x, (std::max) (0.0f, chosen->room));
    else
        result.size.y = (std::min) (result.size.y, (std::max) (0.0f, chosen->room));
    result.position = { std::clamp ((dockMin.x + dockMax.x - result.size.x) * 0.5f, 0.0f, view.x - result.size.x),
                        std::clamp ((dockMin.y + dockMax.y - result.size.y) * 0.5f, 0.0f, view.y - result.size.y) };
    if (chosen->side == DockEdge::Right)
        result.position.x = dockMax.x;
    else if (chosen->side == DockEdge::Left)
        result.position.x = dockMin.x - result.size.x;
    else if (chosen->side == DockEdge::Bottom)
        result.position.y = dockMax.y;
    else
        result.position.y = dockMin.y - result.size.y;
    return result;
}

DockPanelRect PanelAtDock (ImVec2 view, ImVec2 a, ImVec2 b, DockEdge edge, bool horizontal, ImVec2 wanted, float gap)
{
    edge = PanelSide (view, a, b, edge, horizontal);
    ImVec2 lo (0, 0), hi = view;
    if (edge == DockEdge::Top)
        lo.y = (std::min) (view.y, b.y + gap);
    if (edge == DockEdge::Bottom)
        hi.y = (std::max) (0.0f, a.y - gap);
    if (edge == DockEdge::Left)
        lo.x = (std::min) (view.x, b.x + gap);
    if (edge == DockEdge::Right)
        hi.x = (std::max) (0.0f, a.x - gap);
    DockPanelRect result;
    result.side = edge;
    result.size = { (std::min) (wanted.x, hi.x - lo.x), (std::min) (wanted.y, hi.y - lo.y) };
    result.position = { std::clamp (a.x, lo.x, hi.x - result.size.x), std::clamp (a.y, lo.y, hi.y - result.size.y) };
    if (edge == DockEdge::Right)
        result.position.x = hi.x - result.size.x;
    if (edge == DockEdge::Bottom)
        result.position.y = hi.y - result.size.y;
    return result;
}

DockResult Dock (DockPlacement& p, ImVec2 view, float scale, float ui, ImFont* font, const std::string& label,
                 const layers::Panel& look, bool open, const Circle& top, const Circle* bottom, const bool* locked,
                 const std::vector<HostTab>& tabs, const std::string& held)
{
    DockResult result;
    if (p.edge != DockEdge::Floating)
        p.horizontal = p.edge == DockEdge::Top || p.edge == DockEdge::Bottom;
    ImGui::PushFont (font, kDockFontPixels * ui);
    const ImVec2 padding (kDockPadding[0] * ui, kDockPadding[1] * ui);
    const ImVec2 text = ImGui::CalcTextSize (label.c_str ());
    const float across = std::ceil (text.y + 2 * padding.x);
    const float along = std::ceil (text.x + 2 * padding.y) + across * (2 + (bottom ? 1 : 0) + (locked ? 1 : 0));
    const ImVec2 size = p.horizontal ? ImVec2 (along, across) : ImVec2 (across, along);
    ImVec2 pos = DockPosition (p, view, size, scale);
    ImGui::SetNextWindowPos (pos);
    ImGui::SetNextWindowSize (size);
    ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (0, 0));
    ImGui::PushStyleVar (ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar (ImGuiStyleVar_ItemSpacing, ImVec2 (0, 0));
    const auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                       ImGuiWindowFlags_NoBackground;
    if (ImGui::Begin ("###tapioca.dock", nullptr, flags)) {
        const DockEdge side = PanelSide (view, pos, ImVec2 (pos.x + size.x, pos.y + size.y), p.edge, p.horizontal);
        result.press = DockTab ("##hud", label, look, open, top, bottom, padding, ui, locked, p.horizontal, side);
        if (result.press.title) {
            p.workflowFlyoutOpen = !p.workflowFlyoutOpen;
            if (p.workflowFlyoutOpen)
                ImGui::OpenPopup ("##dockWorkflows");
        }
        if (ImGui::IsWindowHovered () && ImGui::IsMouseClicked (ImGuiMouseButton_Right)) {
            ClaimRightClick ();
            ImGui::OpenPopup ("##dockPosition");
        }
        const int menuColours = PushLook (look, ui);
        const float padAlong = std::round (8.0f * ui);
        const float padAcross = std::round (3.0f * ui);
        const float spacing = std::round (2.0f * ui);
        const float buttonAcross = std::ceil (ImGui::GetTextLineHeight () + 2.0f * padAcross);
        float totalAlong = 0.0f;
        size_t workflowCount = 0;
        for (const HostTab& tab : tabs)
            if (tab.key != kStatsKey && tab.key != kSelectionKey && tab.key != kSettingsKey && tab.key != kDebugKey) {
                const float length = std::ceil (ImGui::CalcTextSize (tab.title.c_str ()).x + 2.0f * padAlong);
                totalAlong += length;
                ++workflowCount;
            }
        if (workflowCount == 0) {
            totalAlong = std::ceil (ImGui::CalcTextSize ("No workflows available").x + 2.0f * padAlong);
        }
        const float gaps = float (workflowCount > 0 ? workflowCount - 1 : 0) * spacing;
        const ImVec2 wanted = p.horizontal ? ImVec2 (totalAlong + gaps, across) : ImVec2 (across, totalAlong + gaps);
        const DockPanelRect flyout = WorkflowFlyoutAtDock (view, pos, ImVec2 (pos.x + size.x, pos.y + size.y), wanted);
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (0, 0));
        ImGui::PushStyleVar (ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar (ImGuiStyleVar_ItemSpacing, ImVec2 (0, spacing));
        ImGui::PushStyleVar (ImGuiStyleVar_WindowMinSize, ImVec2 (0, 0));
        ImGui::PushStyleColor (ImGuiCol_PopupBg, ImVec4 (0, 0, 0, 0));
        ImGui::SetNextWindowPos (flyout.position);
        ImGui::SetNextWindowSize (flyout.size);
        if (ImGui::BeginPopup ("##dockWorkflows",
                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
            if (!p.workflowFlyoutOpen)
                ImGui::CloseCurrentPopup ();
            else {
                const uint32_t fill = open ? graphicssettings::Colour ("ui.dock.activeBackground", look.accentRgba)
                                           : graphicssettings::Colour ("ui.dock.background", look.backgroundRgba);
                const uint32_t ink = open ? graphicssettings::Colour ("ui.dock.activeText", 0xFFFFFFFFu)
                                          : graphicssettings::Colour ("ui.dock.text", look.textRgba);
                const ImVec4 base = ImGui::ColorConvertU32ToFloat4 (Packed (fill));
                const auto darker = [&] (float factor) {
                    return ImVec4 (base.x * factor, base.y * factor, base.z * factor, base.w);
                };
                ImGui::PushStyleColor (ImGuiCol_Button, base);
                ImGui::PushStyleColor (ImGuiCol_ButtonHovered, darker (0.85f));
                ImGui::PushStyleColor (ImGuiCol_ButtonActive, darker (0.72f));
                ImGui::PushStyleColor (ImGuiCol_Text, Packed (ink));
                ImGui::PushStyleVar (ImGuiStyleVar_FramePadding, ImVec2 (padAlong, padAcross));
                ImGui::PushStyleVar (
                    ImGuiStyleVar_FrameRounding,
                    (std::min) (float (graphicssettings::Number ("ui.dock.rounding", 6)) * scale, buttonAcross * 0.5f));
                bool firstChoice = true;
                const auto choice = [&] (const char* title) {
                    const ImVec2 labelSize = ImGui::CalcTextSize (title);
                    const float length = std::ceil (labelSize.x + 2.0f * padAlong);
                    const ImVec2 buttonSize = p.horizontal ? ImVec2 ((std::min) (flyout.size.x, length), buttonAcross)
                                                           : ImVec2 ((std::min) (flyout.size.x, buttonAcross), length);
                    if (p.horizontal) {
                        if (!firstChoice)
                            ImGui::SameLine (0, spacing);
                        ImGui::SetCursorPosY (std::floor ((flyout.size.y - buttonSize.y) * 0.5f));
                    }
                    else
                        ImGui::SetCursorPosX (std::floor ((flyout.size.x - buttonSize.x) * 0.5f));
                    firstChoice = false;
                    const bool pressed = ImGui::Button (p.horizontal ? title : "##workflow", buttonSize);
                    if (!p.horizontal) {
                        ImDrawList* draw = ImGui::GetWindowDrawList ();
                        const ImVec2 a = ImGui::GetItemRectMin (), b = ImGui::GetItemRectMax ();
                        const ImVec2 middle (std::floor ((a.x + b.x) * 0.5f), std::floor ((a.y + b.y) * 0.5f));
                        const int first = draw->VtxBuffer.Size;
                        draw->PushClipRect (ImVec2 (-32768.0f, -32768.0f), ImVec2 (32768.0f, 32768.0f), false);
                        draw->AddText (ImVec2 (middle.x - std::floor (labelSize.x * 0.5f),
                                               middle.y - std::floor (labelSize.y * 0.5f)),
                                       Packed (ink), title);
                        draw->PopClipRect ();
                        for (int k = first; k < draw->VtxBuffer.Size; ++k) {
                            ImDrawVert& v = draw->VtxBuffer[k];
                            const float dx = v.pos.x - middle.x, dy = v.pos.y - middle.y;
                            v.pos = ImVec2 (middle.x - dy, middle.y + dx);
                        }
                    }
                    return pressed;
                };
                if (workflowCount == 0) {
                    ImGui::BeginDisabled ();
                    ImGui::PushID ("##none");
                    choice ("No workflows available");
                    ImGui::PopID ();
                    ImGui::EndDisabled ();
                }
                for (const HostTab& tab : tabs) {
                    if (tab.key == kStatsKey || tab.key == kSelectionKey || tab.key == kSettingsKey ||
                        tab.key == kDebugKey)
                        continue;
                    ImGui::PushID (tab.key.c_str ());
                    if (choice (tab.title.c_str ())) {
                        result.selected = tab.key;
                        p.workflowFlyoutOpen = false;
                        ImGui::CloseCurrentPopup ();
                    }
                    if (tab.key == held)
                        Tip ("Current workflow");
                    else if (!tab.purpose.empty ())
                        Tip (tab.purpose);
                    ImGui::PopID ();
                }
                ImGui::PopStyleVar (2);
                ImGui::PopStyleColor (4);
            }
            ImGui::EndPopup ();
        }
        else if (!result.press.titleActive && !result.press.title)
            p.workflowFlyoutOpen = false;
        ImGui::PopStyleColor ();
        ImGui::PopStyleVar (4);
        if (ImGui::BeginPopup ("##dockPosition")) {
            if (ImGui::BeginMenu ("Dock location")) {
                struct Anchor {
                    const char* name;
                    DockEdge edge;
                    float along;
                };
                const Anchor anchors[] = {
                    { "Top left", DockEdge::Top, 0 },     { "Top", DockEdge::Top, 0.5f },
                    { "Top right", DockEdge::Top, 1 },    { "Left", DockEdge::Left, 0.5f },
                    { "Right", DockEdge::Right, 0.5f },   { "Bottom left", DockEdge::Bottom, 0 },
                    { "Bottom", DockEdge::Bottom, 0.5f }, { "Bottom right", DockEdge::Bottom, 1 }
                };
                for (const auto& anchor : anchors)
                    if (ImGui::MenuItem (anchor.name, nullptr, p.edge == anchor.edge && p.along == anchor.along)) {
                        p.edge = anchor.edge;
                        p.along = anchor.along;
                    }
                if (ImGui::MenuItem ("Free floating", nullptr, p.edge == DockEdge::Floating)) {
                    p.edge = DockEdge::Floating;
                    p.freePosition = {
                        std::clamp (pos.x, 32.0f * scale, (std::max) (32.0f * scale, view.x - size.x - 32 * scale)) /
                            scale,
                        std::clamp (pos.y, 32.0f * scale, (std::max) (32.0f * scale, view.y - size.y - 32 * scale)) /
                            scale
                    };
                }
                if (p.edge == DockEdge::Floating && ImGui::MenuItem ("Horizontal", nullptr, p.horizontal))
                    p.horizontal = !p.horizontal;
                ImGui::EndMenu ();
            }
            ImGui::EndPopup ();
        }
        ImGui::PopStyleColor (menuColours);
        ImGui::PopStyleVar (kLookVars);
        if (result.press.dragging) {
            if (!p.dragging)
                p.dragPosition = { pos.x / scale, pos.y / scale };
            p.dragPosition.x += result.press.delta.x / scale;
            p.dragPosition.y += result.press.delta.y / scale;
            p.edge = DockEdge::Floating;
            pos = MagneticDockPosition ({ p.dragPosition.x * scale, p.dragPosition.y * scale }, view, size, scale);
            p.freePosition = { pos.x / scale, pos.y / scale };
            p.dragging = true;
        }
        else if (p.dragging && !ImGui::IsMouseDown (ImGuiMouseButton_Left)) {
            SnapDock (p, { p.dragPosition.x * scale, p.dragPosition.y * scale }, view, size, scale);
            p.dragging = false;
            result.press.title = false;
        }
    }
    result.window = ImGui::GetCurrentWindow ();
    ImGui::End ();
    ImGui::PopStyleVar (3);
    ImGui::PopFont ();
    return result;
}

} // namespace geomsrv::archviz::hudshell
