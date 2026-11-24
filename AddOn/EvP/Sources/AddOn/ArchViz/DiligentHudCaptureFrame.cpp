#include "ArchViz/DiligentHudCaptureFrame.hpp"

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/LayoutReportPage.hpp"
#include "ArchViz/SunStudyColourControls.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/TapiocaUI/Foundation/ScopedStyle.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::viewerhud {

namespace {

using hudshell::DockEdge;

void ShadowClockRange (CaptureFrame& f, float dpi)
{
    ImGui::Text ("Start %02d:%02d     End %02d:%02d", f.startMinute / 60, f.startMinute % 60, f.endMinute / 60,
                 f.endMinute % 60);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("-##shadow-divisions"))
        f.divisions = std::max (1, f.divisions - 1);
    ImGui::SameLine ();
    ImGui::Text ("%d divisions", f.divisions);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("+##shadow-divisions"))
        f.divisions = std::min (8, f.divisions + 1);
    auto pos = ImGui::GetCursorScreenPos ();
    float w = std::clamp (ImGui::GetContentRegionAvail ().x, 1.f, 324.f * dpi);
    ImGui::InvisibleButton ("##shadow-clock-range", { w, 24 * dpi });
    const auto x = [&] (int minute) { return pos.x + 6 * dpi + (w - 12 * dpi) * minute / 1435.f; };
    float y = pos.y + 12 * dpi, mx = ImGui::GetIO ().MousePos.x;
    if (ImGui::IsItemActivated ())
        f.grip = std::abs (mx - x (f.startMinute)) <= std::abs (mx - x (f.endMinute)) ? 1 : 2;
    if (ImGui::IsItemActive ()) {
        int minute = std::clamp (int (std::lround ((mx - x (0)) / (w - 12 * dpi) * 287)) * 5, 0, 1435);
        if (f.grip == 1)
            f.startMinute = std::min (minute, f.endMinute - 5 * f.divisions);
        else
            f.endMinute = std::max (minute, f.startMinute + 5 * f.divisions);
    }
    f.divisions = std::min (f.divisions, (f.endMinute - f.startMinute) / 5);
    auto* draw = ImGui::GetWindowDrawList ();
    draw->AddLine ({ x (0), y }, { x (1435), y }, IM_COL32 (150, 150, 150, 255), 3 * dpi);
    draw->AddLine ({ x (f.startMinute), y }, { x (f.endMinute), y }, IM_COL32 (65, 120, 203, 255), 4 * dpi);
    f.clocks.clear ();
    int closest = 0;
    for (int i = 0; i <= f.divisions; ++i) {
        int m = f.startMinute + 5 * int (std::lround (double (i) * (f.endMinute - f.startMinute) / (5 * f.divisions)));
        f.clocks.push_back (m);
        if (std::abs (mx - x (m)) < std::abs (mx - x (f.clocks[closest])))
            closest = i;
        if (i == 0 || i == f.divisions)
            draw->AddCircleFilled ({ x (m), y }, 6 * dpi, IM_COL32 (65, 120, 203, 255));
        else
            draw->AddLine ({ x (m), y - 5 * dpi }, { x (m), y + 5 * dpi }, ImGui::GetColorU32 (ImGuiCol_Text),
                           1.5f * dpi);
    }
    int hover = f.clocks[closest];
    if (ImGui::IsItemHovered () && std::abs (mx - x (hover)) <= 7 * dpi)
        ImGui::SetTooltip ("%02d:%02d", hover / 60, hover % 60);
}

constexpr auto kCaptureWindowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                     ImGuiWindowFlags_NoFocusOnAppearing;

void UseCaptureLook (ui::ScopedStyle& style, float scale)
{
    static const auto look = [] {
        auto panel = hudshell::PlainLook ();
        panel.accentRgba = 0x4178CBFF;
        return panel;
    }();
    style.Adopt (hudshell::PushLook (look, scale), hudshell::kLookVars);
}

bool ShotPending (const HudState& state)
{
    return state.sunShotRequested || state.sunShotBusy;
}

bool HasArea (ScreenshotRect rect)
{
    return rect.Width () > 0.0f && rect.Height () > 0.0f;
}

ScreenshotRect ToPixels (ScreenshotRect rect, ScreenshotSize view)
{
    return { rect.left * view.width, rect.top * view.height, rect.right * view.width, rect.bottom * view.height };
}

ScreenshotRect ToNormalized (ScreenshotRect rect, ScreenshotSize view)
{
    return { std::clamp (rect.left / view.width, 0.0f, 1.0f), std::clamp (rect.top / view.height, 0.0f, 1.0f),
             std::clamp (rect.right / view.width, 0.0f, 1.0f), std::clamp (rect.bottom / view.height, 0.0f, 1.0f) };
}

DockEdge CaptureDockEdge (const CaptureDock& dock)
{
    if (dock.edge != DockEdge::Floating)
        return dock.edge;
    if (dock.horizontal)
        return dock.rect.top < float (dock.view.height) - dock.rect.bottom ? DockEdge::Top : DockEdge::Bottom;
    return dock.rect.left < float (dock.view.width) - dock.rect.right ? DockEdge::Left : DockEdge::Right;
}

float CaptureAspect (int preset, ScreenshotSize view)
{
    const float ratios[] = { 0.0f, 16.0f / 9.0f, 4.0f / 3.0f, 1.0f, float (view.width) / view.height };
    return ratios[preset];
}

// Fixed-aspect edges resize around the centre of the other axis, while corners
// leave the opposite corner fixed. These anchors also determine the available space.
float Anchor (float low, float high, int side)
{
    return side < 0 ? high : side > 0 ? low : (low + high) * 0.5f;
}

float ExtentLimit (float anchor, float limit, int side)
{
    return side < 0 ? anchor : side > 0 ? limit - anchor : 2.0f * (std::min) (anchor, limit - anchor);
}

void ResizeAxis (float anchor, float size, int side, float& low, float& high)
{
    low = anchor - (side < 0 ? size : side > 0 ? 0.0f : size * 0.5f);
    high = low + size;
}

int HitSide (float mouse, float low, float high, float grip)
{
    if (std::abs (mouse - low) < grip)
        return -1;
    return std::abs (mouse - high) < grip ? 1 : 0;
}

ImGuiMouseCursor DragCursor (int sideX, int sideY)
{
    if (sideX && sideY)
        return sideX == sideY ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW;
    if (sideX)
        return ImGuiMouseCursor_ResizeEW;
    return sideY ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeAll;
}

bool DragFrame (CaptureFrame& frame, ScreenshotSize view, float grip, float minimum)
{
    const ScreenshotRect rect = ToPixels (frame.rect, view);
    const ImVec2 start { (std::max) (0.0f, rect.left - grip), (std::max) (0.0f, rect.top - grip) };
    const ImVec2 end { (std::min) (float (view.width), rect.right + grip),
                       (std::min) (float (view.height), rect.bottom + grip) };
    ImGui::SetNextWindowPos (start);
    ImGui::SetNextWindowSize ({ end.x - start.x, end.y - start.y });
    ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, { 0, 0 });
    ImGui::PushStyleVar (ImGuiStyleVar_WindowBorderSize, 0.0f);
    bool active = false;
    frame.hotX = frame.hotY = 0;
    if (ImGui::Begin ("##capture-frame", nullptr,
                      kCaptureWindowFlags | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus)) {
        ImGui::InvisibleButton ("##move-resize", { end.x - start.x, end.y - start.y });
        const ImVec2 mouse = ImGui::GetMousePos ();
        const int sideX = HitSide (mouse.x, rect.left, rect.right, grip);
        const int sideY = HitSide (mouse.y, rect.top, rect.bottom, grip);
        if (ImGui::IsItemActivated ()) {
            frame.dragX = mouse.x;
            frame.dragY = mouse.y;
            frame.dragRect = frame.rect;
            frame.dragSideX = sideX;
            frame.dragSideY = sideY;
        }
        active = ImGui::IsItemActive ();
        if ((active || ImGui::IsItemDeactivated ()) && ImGui::IsMousePosValid (&mouse)) {
            const auto edited =
                EditCaptureRect (ToPixels (frame.dragRect, view), view, mouse.x - frame.dragX, mouse.y - frame.dragY,
                                 frame.dragSideX, frame.dragSideY, minimum, CaptureAspect (frame.aspect, view));
            frame.rect = ToNormalized (edited, view);
        }
        if (active || ImGui::IsItemHovered ()) {
            frame.hotX = active ? frame.dragSideX : sideX;
            frame.hotY = active ? frame.dragSideY : sideY;
            ImGui::SetMouseCursor (DragCursor (frame.hotX, frame.hotY));
        }
    }
    ImGui::End ();
    ImGui::PopStyleVar (2);
    return active;
}

void DrawMask (ScreenshotRect rect, ScreenshotSize view, float dpiScale, const CaptureFrame& frame, bool active)
{
    ImDrawList* draw = ImGui::GetBackgroundDrawList ();
    const float width = float (view.width);
    const float height = float (view.height);
    constexpr ImU32 mask = IM_COL32 (0, 0, 0, 148);
    constexpr ImU32 frameColor = IM_COL32 (255, 255, 255, 255);
    draw->AddRectFilled ({ 0, 0 }, { width, rect.top }, mask);
    draw->AddRectFilled ({ 0, rect.bottom }, { width, height }, mask);
    draw->AddRectFilled ({ 0, rect.top }, { rect.left, rect.bottom }, mask);
    draw->AddRectFilled ({ rect.right, rect.top }, { width, rect.bottom }, mask);
    draw->AddRect ({ rect.left, rect.top }, { rect.right, rect.bottom }, frameColor, 0.0f, 0, dpiScale);
    const float midX = (rect.left + rect.right) * 0.5f;
    const float midY = (rect.top + rect.bottom) * 0.5f;
    const ImVec2 handles[] = { { rect.left, rect.top }, { midX, rect.top },         { rect.right, rect.top },
                               { rect.left, midY },     { rect.right, midY },       { rect.left, rect.bottom },
                               { midX, rect.bottom },   { rect.right, rect.bottom } };
    for (const ImVec2 p : handles) {
        const bool hot = (frame.hotX || frame.hotY) && frame.hotX == (p.x > midX) - (p.x < midX) &&
                         frame.hotY == (p.y > midY) - (p.y < midY);
        const float radius = (hot ? active ? 6.f : 5.f : 3.f) * dpiScale;
        draw->AddRectFilled ({ p.x - radius, p.y - radius }, { p.x + radius, p.y + radius },
                             hot ? active ? IM_COL32 (28, 86, 194, 255) : IM_COL32 (85, 157, 255, 255) : frameColor);
    }
}

void DrawResult (const HudState& state, int kind)
{
    if (state.shotKind != kind)
        return;
    if (ShotPending (state))
        ImGui::TextDisabled ("Rendering screenshot...");
    else if (state.sunShotFailed)
        hudshell::ErrorNotice ("Screenshot not saved: " + state.sunShotResult);
    else if (!state.sunShotResult.empty ())
        ImGui::TextWrapped ("Saved %s", state.sunShotResult.c_str ());
}

void DrawShotButton (HudState& state, int kind, const char* label, ImVec2 size = {})
{
    ImGui::BeginDisabled (state.readOnly || state.captureFrame.active || ShotPending (state));
    if (ImGui::Button (label, size))
        BeginCaptureFrame (state, kind);
    ImGui::EndDisabled ();
}

} // namespace

ScreenshotRect DefaultCaptureRect (const CaptureDock& dock, ScreenshotSize view, float margin)
{
    const float width = float (view.width);
    const float height = float (view.height);
    ScreenshotRect available { 0, 0, width, height };
    if (HasArea (dock.rect) && dock.view.width && dock.view.height) {
        const ScreenshotRect former = ToPixels (ToNormalized (dock.rect, dock.view), view);
        const ScreenshotRect bands[] = { { 0, std::clamp (former.bottom, 0.0f, height), width, height },
                                         { 0, 0, width, std::clamp (former.top, 0.0f, height) },
                                         { std::clamp (former.right, 0.0f, width), 0, width, height },
                                         { 0, 0, std::clamp (former.left, 0.0f, width), height } };
        const DockEdge edge = CaptureDockEdge (dock);
        available = bands[int (edge) - int (DockEdge::Top)];
        if (available.Width () <= 2.0f * margin || available.Height () <= 2.0f * margin) {
            for (const ScreenshotRect band : bands)
                if (band.Width () * band.Height () > available.Width () * available.Height ())
                    available = band;
        }
    }
    const float insetX = (std::min) (margin, available.Width () * 0.125f);
    const float insetY = (std::min) (margin, available.Height () * 0.125f);
    return { available.left + insetX, available.top + insetY, available.right - insetX, available.bottom - insetY };
}

ScreenshotRect EditCaptureRect (ScreenshotRect rect, ScreenshotSize view, float dx, float dy, int sideX, int sideY,
                                float minimum, float aspect)
{
    const float width = float (view.width);
    const float height = float (view.height);
    if (!sideX && !sideY) {
        dx = std::clamp (dx, -rect.left, width - rect.right);
        dy = std::clamp (dy, -rect.top, height - rect.bottom);
        return { rect.left + dx, rect.top + dy, rect.right + dx, rect.bottom + dy };
    }
    const float x = Anchor (rect.left, rect.right, sideX);
    const float y = Anchor (rect.top, rect.bottom, sideY);
    const float limitX = ExtentLimit (x, width, sideX);
    const float limitY = ExtentLimit (y, height, sideY);
    const float wantedX = rect.Width () + sideX * dx;
    const float wantedY = rect.Height () + sideY * dy;
    if (aspect > 0.0f) {
        const float maxHeight = (std::min) (limitY, limitX / aspect);
        const float minHeight = (std::min) (maxHeight, (std::max) (minimum, minimum / aspect));
        const bool fromWidth = sideX && (!sideY || std::abs (dx) >= std::abs (dy * aspect));
        const float aspectHeight = std::clamp (fromWidth ? wantedX / aspect : wantedY, minHeight, maxHeight);
        ResizeAxis (x, aspectHeight * aspect, sideX, rect.left, rect.right);
        ResizeAxis (y, aspectHeight, sideY, rect.top, rect.bottom);
        return rect;
    }
    if (sideX)
        ResizeAxis (x, std::clamp (wantedX, (std::min) (minimum, limitX), limitX), sideX, rect.left, rect.right);
    if (sideY)
        ResizeAxis (y, std::clamp (wantedY, (std::min) (minimum, limitY), limitY), sideY, rect.top, rect.bottom);
    return rect;
}

ScreenshotRect FitCaptureAspect (ScreenshotRect rect, ScreenshotSize view, float minimum, float aspect)
{
    if (aspect <= 0.0f)
        return rect;
    const float x = Anchor (rect.left, rect.right, 0);
    const float y = Anchor (rect.top, rect.bottom, 0);
    const float maxHeight =
        (std::min) (ExtentLimit (y, float (view.height), 0), ExtentLimit (x, float (view.width), 0) / aspect);
    const float minHeight = (std::min) (maxHeight, (std::max) (minimum, minimum / aspect));
    const float height = std::clamp ((std::min) (rect.Height (), rect.Width () / aspect), minHeight, maxHeight);
    ResizeAxis (x, height * aspect, 0, rect.left, rect.right);
    ResizeAxis (y, height, 0, rect.top, rect.bottom);
    return rect;
}

bool BeginCaptureFrame (HudState& state, int kind)
{
    if (ShotPending (state) || state.captureFrame.active || state.readOnly)
        return false;
    state.captureFrame = {};
    state.captureFrame.active = true;
    state.captureFrame.kind = kind;
    state.captureFrame.viewSlot = state.captureTopView ? 2 : state.orthographic ? 1 : 0;
    state.captureFrame.rect = state.captureRegions[state.captureFrame.viewSlot];
    state.captureFrame.month = std::clamp (state.sunRunMonth, 1, 12);
    state.captureFrame.day = state.sunRunDay;
    state.captureFrame.preset = std::clamp (state.sunShotPreset, 0, kViewerScreenshotScaleCount - 1);
    if (kind != kShotExposedSurfaces)
        state.exposedShot.reset ();
    return true;
}

bool DrawCaptureFrame (HudState& state, const CaptureDock& dock, ScreenshotSize view, float dpiScale,
                       const SunStudyOverlayStatus* shown)
{
    if (!view.width || !view.height)
        return false;
    CaptureFrame& frame = state.captureFrame;
    const float margin = 8.0f * dpiScale;
    const float minimum = 48.0f * dpiScale;
    if (!HasArea (frame.rect))
        frame.rect = ToNormalized (DefaultCaptureRect (dock, view, margin), view);
    else if (frame.aspect && (frame.view.width != view.width || frame.view.height != view.height))
        frame.rect = ToNormalized (
            FitCaptureAspect (ToPixels (frame.rect, view), view, minimum, CaptureAspect (frame.aspect, view)), view);
    frame.view = view;
    const bool dragging = !frame.builder && HasArea (frame.rect) && DragFrame (frame, view, 7.0f * dpiScale, minimum);
    const ScreenshotRect rect = ToPixels (frame.rect, view);
    if (!frame.builder)
        DrawMask (rect, view, dpiScale, frame, dragging);

    const float bottomSpace = float (view.height) - rect.bottom;
    const bool atBottom = bottomSpace > rect.top;
    ui::ScopedStyle style;
    UseCaptureLook (style, dpiScale);
    const float sidebar = hudshell::MenuPanelWidth (dpiScale * dock.fontScale, float (view.width));
    bool cameraTile = false;
    if (frame.builder) {
        {
            const auto page = ReportPageFormat (frame.pageFormat);
            const float s = std::max (.001f, std::min ((view.width - sidebar - 2 * margin) / page.width,
                                                       (view.height - 2 * margin) / page.height));
            ImVec2 a { (view.width + sidebar - page.width * s) / 2, (view.height - page.height * s) / 2 };
            ImVec2 b { a.x + page.width * s, a.y + page.height * s };
            auto* draw = ImGui::GetBackgroundDrawList ();
            draw->AddRectFilled ({ 0, 0 }, { float (view.width), float (view.height) }, IM_COL32 (47, 49, 50, 255));
            draw->AddRectFilled (a, b, IM_COL32_WHITE);
            if (frame.paperImage)
                draw->AddImage (ImTextureID (frame.paperImage), a, b);
            else
                draw->AddText ({ a.x + margin, a.y + margin }, IM_COL32_BLACK,
                               ReportTitle (ReportTemplate (frame.reportType)));
            const bool popup = ImGui::IsPopupOpen (nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
            for (const auto& tile : frame.paperTiles) {
                ImVec2 tl { a.x + tile.x * (b.x - a.x), a.y + tile.y * (b.y - a.y) };
                ImVec2 br { tl.x + tile.w * (b.x - a.x), tl.y + tile.h * (b.y - a.y) };
                if (frame.liveImage) {
                    const float sw = std::min (rect.Width (), rect.Height () * frame.tileAspect),
                                sh = sw / frame.tileAspect;
                    const float x = (rect.left + rect.right - sw) * .5f / view.width,
                                y = (rect.top + rect.bottom - sh) * .5f / view.height;
                    draw->AddImage (ImTextureID (frame.liveImage), tl, br, { x, y },
                                    { x + sw / view.width, y + sh / view.height });
                }
                if (popup || !ImGui::IsMouseHoveringRect (tl, br, false))
                    continue;
                cameraTile = frame.editCamera;
                if (cameraTile && (!frame.cameraNavHeld || frame.cameraTilePixels.Width () == 0)) {
                    frame.cameraTilePixels = { tl.x, tl.y, br.x, br.y };
                    const float sw = std::min (rect.Width (), rect.Height () * frame.tileAspect),
                                sh = sw / frame.tileAspect;
                    const float x = (rect.left + rect.right - sw) * .5f, y = (rect.top + rect.bottom - sh) * .5f;
                    frame.cameraTileSource = { x, y, x + sw, y + sh };
                }
                draw->AddRect (tl, br, IM_COL32 (82, 92, 105, 255));
                draw->AddRectFilled ({ br.x - margin, br.y - margin }, br, IM_COL32 (82, 92, 105, 255));
                if (ImGui::IsMouseHoveringRect ({ br.x - margin, br.y - margin }, br, false) &&
                    ImGui::IsMouseClicked (0)) {
                    frame.resizeX = tl.x;
                    frame.resizeY = tl.y;
                    frame.resizingTile = true;
                }
                else if (ImGui::IsMouseDoubleClicked (0)) {
                    frame.editCamera = true;
                    frame.paperDirty = true;
                }
            }
            if (frame.resizingTile) {
                auto mouse = ImGui::GetMousePos ();
                frame.tileAspect =
                    std::clamp ((mouse.x - frame.resizeX) / std::max (1.f, mouse.y - frame.resizeY), .4f, 3.f);
                ImGui::SetMouseCursor (ImGuiMouseCursor_ResizeNWSE);
                frame.resizingTile = ImGui::IsMouseDown (0);
            }
        }
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ sidebar, float (view.height) });
    }
    else
        ImGui::SetNextWindowPos ({ float (view.width) * 0.5f, atBottom ? float (view.height) - margin : margin },
                                 ImGuiCond_Always, { 0.5f, atBottom ? 1.0f : 0.0f });
    bool ownsMouse = dragging;
    if (ImGui::Begin ("##capture-bar", nullptr,
                      kCaptureWindowFlags | (frame.builder ? 0 : ImGuiWindowFlags_AlwaysAutoResize))) {
        if (frame.builder) {
            ImGui::TextUnformatted ("Layout Builder");
            ImGui::Separator ();
        }
        if (frame.kind == kShotSunStudy) {
            ImGui::SetNextItemWidth (220 * dpiScale);
            frame.paperDirty |= DrawAnalysisColourChoice (state,
                                                          frame.builder ? (frame.reportType == 2   ? 0
                                                                           : frame.reportType == 1 ? 9
                                                                                                   : 5)
                                                                        : frame.entryMode,
                                                          220 * dpiScale, !frame.builder);
        }
        if (frame.builder) {
            const char* templates[] = { "Shadow Analysis", "Shadow Patterns", "Sunlight Access" };
            const char* papers[] = { "A4 landscape", "A4 portrait", "A3 landscape", "A3 portrait" };
            ImGui::SetNextItemWidth (190 * dpiScale);
            frame.paperDirty |= ImGui::Combo ("Workflow output", &frame.reportType, templates, 3);
            if (frame.reportType != 2) {
                auto swatch = [&] (const char* label, uint32_t& rgb) {
                    ImGui::PushID (label);
                    frame.paperDirty |= gradientchoice::ColourChoice ("##colour", rgb, false);
                    ImGui::SameLine ();
                    ImGui::TextUnformatted (label);
                    ImGui::PopID ();
                };
                if (frame.reportType == 0) {
                    swatch ("Sunlit", state.sunSingleLitRgba);
                    ImGui::SameLine ();
                    swatch ("Shaded", state.sunSingleShadowedRgba);
                }
                else {
                    swatch ("Neither", state.sunAmPmSunlitRgba);
                    ImGui::SameLine ();
                    swatch ("AM only", state.sunAmPmMorningRgba);
                    ImGui::SameLine ();
                    swatch ("PM only", state.sunAmPmEveningRgba);
                }
            }
            ImGui::SetNextItemWidth (190 * dpiScale);
            frame.paperDirty |= ImGui::Combo ("Page format", &frame.pageFormat, papers, 4);
            ImGui::TextDisabled ("Equinox  /  Summer solstice  /  Winter solstice");
            const auto previousClocks = frame.clocks;
            if (frame.reportType == 0)
                ShadowClockRange (frame, dpiScale);
            else
                frame.clocks = { 720 };
            frame.paperDirty |= frame.clocks != previousClocks;
            frame.panelSteps.assign (frame.clocks.size (), -1);
            if (shown)
                for (size_t i = 0; i < frame.clocks.size (); ++i) {
                    auto it = std::find (shown->stepMinutes.begin (), shown->stepMinutes.end (), frame.clocks[i]);
                    if (it != shown->stepMinutes.end ())
                        frame.panelSteps[i] = int (it - shown->stepMinutes.begin ());
                }
            auto page = ReportPageFormat (frame.pageFormat);
            ImGui::Text ("Page 1   |   %u x %u px   |   150 dpi", page.width, page.height);
            ImGui::SliderFloat ("Tile W:H", &frame.tileAspect, .4f, 3.f, "%.2f");
            if (ImGui::Button (frame.editCamera ? "Finish camera edit" : "Edit shared camera")) {
                frame.editCamera = !frame.editCamera;
                frame.paperDirty = true;
            }
            ImGui::TextWrapped (
                frame.editCamera
                    ? "Over a tile: Shift + middle drag to orbit, middle drag to pan, wheel to zoom."
                    : "Drag a tile corner to resize the grid. Double-click a tile to edit its shared camera.");
            if (ImGui::Button ("Render preview")) {
                state.sunShotFailed = false;
                frame.previewOnly = true;
                frame.active = frame.editCamera = false;
                state.shotKind = frame.kind;
                state.sunShotPreset = frame.preset;
                state.sunShotRequested = true;
            }
            if (state.sunShotFailed)
                ImGui::TextWrapped ("%s", state.sunShotResult.c_str ());
        }
        const float barWidth = ImGui::CalcTextSize ("Capture   Cancel   16:9   1.5x   12345 x 12345").x +
                               14.0f * ImGui::GetStyle ().FramePadding.x;
        const bool inlineControls = float (view.width) >= barWidth + 2.0f * margin;
        const char* aspects[] = { "Free", "16:9", "4:3", "1:1", "Current view" };
        if (!frame.builder) {
            ImGui::SetNextItemWidth (ImGui::GetFontSize () * 4.0f);
            ImGui::Combo ("##capture-scale", &frame.preset, kViewerScreenshotScaleLabels, kViewerScreenshotScaleCount);
            hudshell::ItemTip ("Output scale");
            ImGui::SameLine ();
            ImGui::SetNextItemWidth (ImGui::GetFontSize () * 7.5f);
            if (ImGui::Combo ("##capture-aspect", &frame.aspect, aspects, IM_ARRAYSIZE (aspects)))
                frame.rect =
                    ToNormalized (FitCaptureAspect (rect, view, minimum, CaptureAspect (frame.aspect, view)), view);
            hudshell::ItemTip ("Frame aspect ratio");
        }
        const ScreenshotPixels crop = ViewerScreenshotCrop (
            frame.rect, ViewerScreenshotTargetSize (view.width, view.height, kViewerScreenshotScales[frame.preset]));
        if (inlineControls && !frame.builder)
            ImGui::SameLine ();
        if (frame.builder)
            ImGui::Text ("%u panels", unsigned (frame.panelSteps.size ()) * 3);
        else
            ImGui::Text ("%u x %u", crop.Width (), crop.Height ());
        ImGui::SameLine ();
        ImGui::BeginDisabled (ShotPending (state) || !crop.Width () || !crop.Height ());
        if (ImGui::Button ("Capture")) {
            frame.previewOnly = false;
            state.captureRegions[frame.viewSlot] = frame.rect;
            state.shotKind = frame.kind;
            state.sunShotPreset = frame.preset;
            state.sunShotRequested = true;
            state.sunShotFailed = false;
            state.sunShotResult.clear ();
            frame.active = false;
        }
        ImGui::EndDisabled ();
        ImGui::SameLine ();
        if (ImGui::Button ("Cancel")) {
            CancelStudyDay (frame);
            frame.active = false;
            frame.cancelled = true;
            state.exposedShot.reset ();
        }
        ownsMouse = ownsMouse || ImGui::IsWindowHovered (ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
                    ImGui::IsPopupOpen (nullptr, ImGuiPopupFlags_AnyPopupId);
        if (!ImGui::IsPopupOpen (nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
            ImGui::BringWindowToDisplayFront (ImGui::GetCurrentWindow ());
    }
    ImGui::End ();
    return ownsMouse || (frame.builder && (!cameraTile || frame.resizingTile));
}

void DrawCaptureRow (HudState& state, int kind)
{
    float w =
        kind == kShotSunStudy ? (ImGui::GetContentRegionAvail ().x - ImGui::GetStyle ().ItemSpacing.x) * .5f : -1.f;
    DrawShotButton (state, kind, kind == kShotVision ? "Capture screenshot" : "Screenshot##sunshot", { w, 0.0f });
    if (kind == kShotSunStudy) {
        hudshell::ItemTip ("Frame the view, then save a PNG with the sun study legend to Tapioca\\Export.");
        ImGui::SameLine ();
    }
    if (kind == kShotSunStudy && ImGui::Button ("Layout Builder", { -1, 0 }) && BeginCaptureFrame (state, kind)) {
        auto& f = state.captureFrame;
        f.builder = f.studyDay = f.seasonal = true;
        f.shadowStyle = 1;
        f.endMinute = 1080;
    }
    DrawResult (state, kind);
}

} // namespace geomsrv::archviz::viewerhud
