// ArchViz/HudShell -- see the header.

#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudShellTabs.hpp"
#include "ArchViz/OverlayHitMap.hpp"
#include "ArchViz/GraphicsSettingsUi.hpp"
#include "ArchViz/GraphicsSettings.hpp"

#include <imgui_internal.h> // ImGuiWindow: where the panel is, and whether it is being dragged

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <utility>

namespace geomsrv {
namespace archviz {
namespace hudshell {

namespace layers = overlaylayers;

ImVec4 Colour (uint32_t rgba)
{
    return ImVec4 (float ((rgba >> 24) & 0xFFu) / 255.0f, float ((rgba >> 16) & 0xFFu) / 255.0f,
                   float ((rgba >> 8) & 0xFFu) / 255.0f, float (rgba & 0xFFu) / 255.0f);
}

ImU32 Packed (uint32_t rgba)
{
    return IM_COL32 ((rgba >> 24) & 0xFFu, (rgba >> 16) & 0xFFu, (rgba >> 8) & 0xFFu, rgba & 0xFFu);
}

// ImGui's packed colour (R in the low byte) back to the caller's 0xRRGGBBAA.
uint32_t Unpacked (ImU32 col)
{
    const uint32_t r = col & 0xFFu, g = (col >> 8) & 0xFFu, b = (col >> 16) & 0xFFu, a = (col >> 24) & 0xFFu;
    return (r << 24) | (g << 16) | (b << 8) | a;
}

uint32_t WithAlpha (uint32_t rgba, float factor)
{
    const uint32_t a = uint32_t (std::lround (float (rgba & 0xFFu) * factor));
    return (rgba & 0xFFFFFF00u) | (std::min) (a, 255u);
}

uint32_t Contrast (uint32_t rgba)
{
    const float r = float ((rgba >> 24) & 0xFFu), g = float ((rgba >> 16) & 0xFFu), b = float ((rgba >> 8) & 0xFFu);
    return (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f > 0.62f ? 0x1F2328FFu : 0xFFFFFFFFu;
}

const layers::Panel& PlainLook ()
{
    static const layers::Panel panel = [] () {
        layers::Panel plain;
        layers::ApplyTheme (plain, layers::PanelTheme::Light);
        plain.title = "Overlay";
        plain.sizePixels = 13.0f;
        plain.widthPixels = 220.0f;
        return plain;
    }();
    return panel;
}

void ErrorNotice (const std::string& message, float scale)
{
    if (message.empty ())
        return;
    const float pad = 7.0f * scale;
    const float width = (std::max) (2.0f * pad + 1.0f, ImGui::GetContentRegionAvail ().x);
    const ImVec2 words = ImGui::CalcTextSize (message.c_str (), nullptr, false, width - 2.0f * pad);
    const ImVec2 from = ImGui::GetCursorScreenPos ();
    const ImVec2 to (from.x + width, from.y + words.y + 2.0f * pad);
    ImGui::Dummy (ImVec2 (width, to.y - from.y));
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    draw->AddRectFilled (from, to, Packed (0xFFF1EFFFu), 3.0f * scale);
    draw->AddRect (from, to, Packed (0xB74741FFu), 3.0f * scale, 0, (std::max) (1.0f, scale));
    draw->AddText (ImGui::GetFont (), ImGui::GetFontSize (), ImVec2 (from.x + pad, from.y + pad), Packed (0x34201FFFu),
                   message.c_str (), nullptr, width - 2.0f * pad);
}

namespace {

// A tip's measures, in its font's em: padding across and down, the arrow's half-width (and
// length), the gap between its point and what it names, the rounding, the view margin, the
// widest line, the swatch and the space after it.
constexpr float kTipPadX = 0.62f, kTipPadY = 0.36f, kTipArrow = 0.42f, kTipGap = 0.18f, kTipRound = 0.3f,
                kTipMargin = 0.3f, kTipWrap = 22.0f, kTipSwatch = 0.85f, kTipSwatchGap = 0.4f;

float Within (float value, float low, float high)
{
    return (std::max) (low, (std::min) (value, high));
}

TipSide Opposite (TipSide side)
{
    switch (side) {
        case TipSide::Left:
            return TipSide::Right;
        case TipSide::Right:
            return TipSide::Left;
        case TipSide::Above:
            return TipSide::Below;
        case TipSide::Below:
            return TipSide::Above;
    }
    return side;
}

// The bubble's top-left for an arrow `arrow` long whose point is at `at`, the bubble on `side`.
ImVec2 BubbleAt (ImVec2 at, TipSide side, ImVec2 size, float arrow)
{
    switch (side) {
        case TipSide::Left:
            return ImVec2 (at.x - arrow - size.x, at.y - size.y * 0.5f);
        case TipSide::Right:
            return ImVec2 (at.x + arrow, at.y - size.y * 0.5f);
        case TipSide::Above:
            return ImVec2 (at.x - size.x * 0.5f, at.y - arrow - size.y);
        case TipSide::Below:
            return ImVec2 (at.x - size.x * 0.5f, at.y + arrow);
    }
    return at;
}

// Whether a bubble at `min` stays in the view along its side's axis.
bool Fits (ImVec2 min, ImVec2 size, TipSide side, ImVec2 view, float margin)
{
    switch (side) {
        case TipSide::Left:
            return min.x >= margin;
        case TipSide::Right:
            return min.x + size.x <= view.x - margin;
        case TipSide::Above:
            return min.y >= margin;
        case TipSide::Below:
            return min.y + size.y <= view.y - margin;
    }
    return true;
}

// The bubble at `at` on `side`, or at `opposite` on the other side when only that fits.
void PlaceTip (ImVec2 at, ImVec2 opposite, TipSide side, const std::string& text, uint32_t swatch)
{
    if (text.empty ())
        return;
    ImFont* const font = ImGui::GetFont ();
    const float em = ImGui::GetFontSize ();
    const bool swatched = (swatch & 0xFFu) != 0;
    const float lead = swatched ? (kTipSwatch + kTipSwatchGap) * em : 0.0f;
    const ImVec2 words = font->CalcTextSizeA (em, FLT_MAX, kTipWrap * em, text.c_str ());
    const ImVec2 size (std::ceil (2.0f * kTipPadX * em + lead + words.x),
                       std::ceil (2.0f * kTipPadY * em + (std::max) (words.y, em)));
    const ImVec2 view = ImGui::GetIO ().DisplaySize;
    const float margin = kTipMargin * em, arrow = std::floor (kTipArrow * em);
    ImVec2 min = BubbleAt (at, side, size, arrow);
    if (!Fits (min, size, side, view, margin)) {
        const TipSide other = Opposite (side);
        const ImVec2 flipped = BubbleAt (opposite, other, size, arrow);
        if (Fits (flipped, size, other, view, margin)) {
            side = other;
            at = opposite;
            min = flipped;
        }
    }
    // In the view, however it was placed; on whole pixels, so its edges are crisp.
    min.x = std::floor (Within (min.x, margin, view.x - margin - size.x));
    min.y = std::floor (Within (min.y, margin, view.y - margin - size.y));
    const ImVec2 max (min.x + size.x, min.y + size.y);
    const float round = std::floor (float (graphicssettings::Number ("ui.tip.rounding", kTipRound)) * em);

    ImDrawList* const draw = ImGui::GetForegroundDrawList ();
    // A soft shadow: three widening rounds a pixel down, fainter outwards.
    for (int k = graphicssettings::Number ("ui.tip.shadow", 1) != 0 ? 3 : 0; k >= 1; --k) {
        const float grow = float (k);
        draw->AddRectFilled (ImVec2 (min.x - grow, min.y - grow + 1.0f), ImVec2 (max.x + grow, max.y + grow + 1.0f),
                             Packed (uint32_t (5 * (4 - k))), round + grow);
    }
    const ImU32 ground = Packed (graphicssettings::Colour ("ui.tip.background", kTipGroundRgba)),
                edge = Packed (graphicssettings::Colour ("ui.tip.border", kTipEdgeRgba));
    draw->AddRectFilled (min, max, ground, round);
    draw->AddRect (min, max, edge, round, 0, 1.0f);
    // The arrow: its base on the bubble's edge facing `at`, a pixel in so it covers the edge
    // there, its point at `at`. ⚠️ CLOCKWISE ON THE SCREEN, or ImGui fringes it inwards.
    const float half = arrow;
    ImVec2 first, last; // the base's ends: the top one, or the left one
    const bool across = side == TipSide::Left || side == TipSide::Right;
    if (across) {
        const float y = Within (at.y, min.y + round + half, max.y - round - half);
        const float x = side == TipSide::Left ? max.x - 1.0f : min.x + 1.0f;
        first = ImVec2 (x, y - half);
        last = ImVec2 (x, y + half);
    }
    else {
        const float x = Within (at.x, min.x + round + half, max.x - round - half);
        const float y = side == TipSide::Above ? max.y - 1.0f : min.y + 1.0f;
        first = ImVec2 (x - half, y);
        last = ImVec2 (x + half, y);
    }
    if (side == TipSide::Left || side == TipSide::Below)
        draw->AddTriangleFilled (first, at, last, ground);
    else
        draw->AddTriangleFilled (last, at, first, ground);
    draw->AddLine (first, at, edge, 1.0f);
    draw->AddLine (at, last, edge, 1.0f);

    ImVec2 pen (min.x + std::floor (kTipPadX * em), min.y + std::floor (kTipPadY * em));
    if (swatched) {
        const float s = std::floor (kTipSwatch * em);
        const ImVec2 a (pen.x, pen.y + std::floor ((em - s) * 0.5f));
        draw->AddRectFilled (a, ImVec2 (a.x + s, a.y + s), Packed (swatch), 0.18f * em);
        draw->AddRect (a, ImVec2 (a.x + s, a.y + s), edge, 0.18f * em);
        pen.x += lead;
    }
    draw->AddText (font, em, pen, Packed (graphicssettings::Colour ("ui.tip.text", kTipInkRgba)), text.c_str (),
                   nullptr, kTipWrap * em);
}

} // namespace

void TipAt (ImVec2 at, TipSide side, const std::string& text, uint32_t swatch)
{
    PlaceTip (at, at, side, text, swatch);
}

void TipBeside (ImVec2 min, ImVec2 max, TipSide side, const std::string& text, uint32_t swatch)
{
    const float gap = kTipGap * ImGui::GetFontSize ();
    const ImVec2 middle ((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    const auto point = [&] (TipSide s) {
        switch (s) {
            case TipSide::Left:
                return ImVec2 (min.x - gap, middle.y);
            case TipSide::Right:
                return ImVec2 (max.x + gap, middle.y);
            case TipSide::Above:
                return ImVec2 (middle.x, min.y - gap);
            case TipSide::Below:
                return ImVec2 (middle.x, max.y + gap);
        }
        return middle;
    };
    PlaceTip (point (side), point (Opposite (side)), side, text, swatch);
}

bool Tip (const std::string& text, TipSide side, uint32_t swatch)
{
    if (text.empty () || !ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
        return false;
    TipBeside (ImGui::GetItemRectMin (), ImGui::GetItemRectMax (), side, text, swatch);
    return true;
}

float FontScaleOfStep (uint32_t step)
{
    return kFontSteps[(std::min) (step, kFontStepCount - 1)] * float (graphicssettings::Number ("ui.fontScale", 1));
}

std::string Percent (float scale)
{
    return std::to_string (int (std::lround (scale * 100.0f))) + " %";
}

namespace {

// `a` towards `b` by `t`, channel by channel.
uint32_t Mix (uint32_t a, uint32_t b, float t)
{
    t = (std::min) ((std::max) (t, 0.0f), 1.0f);
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const float x = float ((a >> shift) & 0xFFu), y = float ((b >> shift) & 0xFFu);
        out |= uint32_t (std::lround (x + (y - x) * t)) << shift;
    }
    return out;
}

constexpr float kPi = 3.14159265358979f;

} // namespace

uint32_t CircleColour (const Circle& circle, uint32_t ink, double seconds)
{
    switch (circle.phase) {
        case Phase::Off:
            return WithAlpha (ink, 0.55f);
        case Phase::Ready:
            return ink;
        case Phase::Busy:
            return circle.progress >= 0.0f
                       ? Mix (graphicssettings::Colour ("ui.dock.busy", kBusyRgba), ink, circle.progress)
                       : graphicssettings::Colour ("ui.dock.busy", kBusyRgba);
        case Phase::Attention:
            // Once a second, half of it faint: a still layout taken twice a second alternates.
            return std::fmod (seconds, 1.0) < 0.5 ? ink : WithAlpha (ink, 0.25f);
        case Phase::Error:
            return graphicssettings::Colour ("ui.dock.error", kErrorRgba);
    }
    return ink;
}

namespace {

// A control opens on press; the HUD's fallback menu opens on release, potentially many frames later.
const ImGuiContext* s_claimedBy = nullptr;
double s_claimedPress = -1;
// Who claimed the pointer, the same way.
const ImGuiContext* g_cursorBy = nullptr;
int g_cursorFrame = -1;
bool g_crosshair = false;
uint8_t g_cursorAs = 0; // an input-layer cursor asked for by number; 0: none

} // namespace

void ClaimRightClick ()
{
    s_claimedBy = ImGui::GetCurrentContext ();
    s_claimedPress = ImGui::GetIO ().MouseClickedTime[ImGuiMouseButton_Right];
}

bool RightClickClaimed ()
{
    return s_claimedBy == ImGui::GetCurrentContext () &&
           s_claimedPress == ImGui::GetIO ().MouseClickedTime[ImGuiMouseButton_Right] &&
           (ImGui::IsMouseDown (ImGuiMouseButton_Right) || ImGui::IsMouseReleased (ImGuiMouseButton_Right));
}

void OwnCursor (bool crosshair)
{
    g_cursorBy = ImGui::GetCurrentContext ();
    g_cursorFrame = ImGui::GetFrameCount ();
    g_crosshair = crosshair;
    g_cursorAs = 0;
}

void OwnCursorAs (uint8_t cursor)
{
    OwnCursor ();
    g_cursorAs = cursor;
}

bool CursorOwned ()
{
    return g_cursorBy == ImGui::GetCurrentContext () && g_cursorFrame == ImGui::GetFrameCount ();
}

uint8_t CursorOf (bool hand)
{
    using overlayinput::Cursor;
    if (!CursorOwned ())
        return uint8_t (hand ? Cursor::Hand : Cursor::Arrow);
    if (g_cursorAs != 0)
        return g_cursorAs;
    if (g_crosshair)
        return uint8_t (Cursor::Cross);
    switch (ImGui::GetMouseCursor ()) {
        case ImGuiMouseCursor_TextInput:
            return uint8_t (Cursor::Text);
        case ImGuiMouseCursor_Hand:
            return uint8_t (Cursor::Hand);
        case ImGuiMouseCursor_ResizeAll:
        case ImGuiMouseCursor_ResizeNESW:
        case ImGuiMouseCursor_ResizeNWSE:
            return uint8_t (Cursor::Move);
        case ImGuiMouseCursor_ResizeEW:
            return uint8_t (Cursor::SizeWE);
        case ImGuiMouseCursor_ResizeNS:
            return uint8_t (Cursor::SizeNS);
        default:
            return uint8_t (Cursor::Arrow);
    }
}

namespace {

// The panel's height at most (HudShell.hpp, Host): hanging from the view's top or bottom edge, all
// of the view but its offset from that edge and a margin at the other; centred, or dragged --
// the drag is kept inside the view -- all but a margin at each.
float TallestPanel (const layers::Panel& panel, const Placement& placement, float scale, float viewHeight)
{
    const float margin = std::floor (kViewMargin * scale);
    const bool middleRow = int (panel.anchor) / 3 == 1;
    if (!placement.placed && !middleRow)
        return viewHeight - std::floor (panel.offsetPixels[1] * scale) - margin;
    return viewHeight - 2.0f * margin;
}

// ⚠️ THE PAGE IN A REGION OF ITS OWN, AS TALL AS IT IS UP TO WHAT THE PANEL HAS LEFT: under the
// tab row (where the cursor is), over the footer (`footer`, its height in the last frame) and
// the window's padding. Past that it scrolls; the tab row stays. ⚠️ ITS OWN WRAP EDGE: a child
// window starts without its parent's. True when the page scrolls.
//
// A panel of a set width holds its page to it. One sized to its content lets its page grow with
// what it holds, never narrower than `least` -- the tab row's width, which a control that fills
// its row takes, as the panel's own did; never the panel's current width either, or a wide page
// once shown would hold every later one as wide.
bool ScrollingPage (const std::string& key, const std::function<void (const std::string& key)>& page, float tallest,
                    float footer, bool setWidth, float least)
{
    const ImGuiStyle& style = ImGui::GetStyle ();
    const float used = ImGui::GetCursorScreenPos ().y - ImGui::GetCurrentWindow ()->Pos.y;
    const float below = style.WindowPadding.y + (footer > 0.0f ? footer + style.ItemSpacing.y : 0.0f);
    const float most = (std::max) (std::floor (tallest - used - below), kLeastPageEm * ImGui::GetFontSize ());
    ImGui::SetNextWindowSizeConstraints (ImVec2 (setWidth ? 0.0f : least, 0.0f), ImVec2 (FLT_MAX, most));
    const ImGuiChildFlags sizing = ImGuiChildFlags_AutoResizeY | (setWidth ? 0 : ImGuiChildFlags_AutoResizeX);
    bool scrolls = false;
    ImGui::PushID (key.c_str ());
    if (ImGui::BeginChild ("##page", ImVec2 (0.0f, 0.0f), sizing, ImGuiWindowFlags_NoNav)) {
        if (setWidth)
            ImGui::PushTextWrapPos (ImGui::GetCursorPosX () + ImGui::GetContentRegionAvail ().x);
        page (key);
        if (setWidth)
            ImGui::PopTextWrapPos ();
        scrolls = ImGui::GetScrollMaxY () > 0.0f;
    }
    ImGui::EndChild ();
    ImGui::PopID ();
    return scrolls;
}

} // namespace

HostResult Host (const HostSpec& spec, const std::string& held, std::string& shownLast, Placement& placement,
                 const std::function<void (const std::string& key)>& page, const std::function<void ()>& footer,
                 NavigationState* navigation)
{
    HostResult result;
    const layers::Panel& panel = spec.look != nullptr ? *spec.look : PlainLook ();
    const float scale = spec.scale, ui = spec.ui;
    const ImVec2 view = spec.view;
    ImGuiWindow* const existing = ImGui::FindWindowByName (spec.window);
    const ImGuiContext& g = *ImGui::GetCurrentContext ();
    const bool moving = existing != nullptr && g.MovingWindow != nullptr && g.MovingWindow->RootWindow == existing;
    // ⚠️ NOT PLACED WHILE IT IS DRAGGED: ImGui moved it before this Begin, and a place set
    // now would put it back under the pointer's start.
    // ⚠️ ITS TOP STAYS (the user, 2026-10-10: a switch from a very long tab to a short one moved the
    // panel as if centred). Where the user left it is kept from its top, never its bottom.
    if (!moving && placement.placed && (placement.corner & 2u) != 0) {
        // Kept from a bottom corner before: the same place, from the top.
        const float height = existing != nullptr ? existing->Size.y : 0.0f;
        placement.offset[1] = (std::max) (view.y / scale - placement.offset[1] - height / scale, 0.0f);
        placement.corner &= 1u;
    }
    if (!moving && !spec.attached) {
        if (placement.placed) {
            const ImVec2 size = existing != nullptr ? existing->Size : ImVec2 (0.0f, 0.0f);
            const bool right = (placement.corner & 1u) != 0;
            float x = right ? view.x - placement.offset[0] * scale - size.x : placement.offset[0] * scale;
            float y = placement.offset[1] * scale;
            // Inside the view, however it was resized.
            x = (std::min) ((std::max) (x, 0.0f), (std::max) (view.x - size.x, 0.0f));
            y = (std::min) ((std::max) (y, 0.0f), (std::max) (view.y - size.y, 0.0f));
            // A taller page pushed it up: that top is where it is now, kept when the page shortens.
            if (existing != nullptr && std::abs (y - placement.offset[1] * scale) > 0.5f)
                placement.offset[1] = y / scale;
            ImGui::SetNextWindowPos (ImVec2 (std::floor (x), std::floor (y)), ImGuiCond_Always);
        }
        else {
            // Where its look asks to be, as an untitled panel is placed.
            const int column = int (panel.anchor) % 3, row = int (panel.anchor) / 3;
            const ImVec2 pivot (float (column) * 0.5f, float (row) * 0.5f);
            const float inwardX = column == 2 ? -1.0f : 1.0f, inwardY = row == 2 ? -1.0f : 1.0f;
            ImGui::SetNextWindowPos (
                ImVec2 (pivot.x * view.x + inwardX * panel.offsetPixels[0] * scale - (column == 2 ? spec.inset : 0.0f),
                        pivot.y * view.y + inwardY * panel.offsetPixels[1] * scale),
                ImGuiCond_Always, pivot);
        }
    }
    const int colours = PushLook (panel, ui);
    if (spec.attached)
        ImGui::PushStyleVar (ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushFont (spec.font, panel.sizePixels * ui);
    const ImGuiStyle& style = ImGui::GetStyle ();
    const bool pageWidth = !spec.attached && spec.pageWidthPixels > 0.0f;
    const float panelWidth =
        pageWidth ? spec.pageWidthPixels
                  : float (graphicssettings::Number ("ui.panelWidth", spec.attached ? 320.0f : panel.widthPixels));
    float width = (std::max) (320.0f * ui, panelWidth * ui);
    width = (std::min) (width, view.x);
    float tallest = TallestPanel (panel, placement, scale, view.y);
    if (spec.attached) {
        const auto bounds = PanelAtDock (view, spec.dockMin, spec.dockMax, spec.dockEdge, spec.dockHorizontal,
                                         ImVec2 (width, view.y), kDockGap * ui);
        width = bounds.size.x;
        tallest = bounds.size.y;
        const float height = existing ? (std::min) (existing->Size.y, tallest) : (std::min) (250.0f * ui, tallest);
        const auto rect = PanelAtDock (view, spec.dockMin, spec.dockMax, spec.dockEdge, spec.dockHorizontal,
                                       ImVec2 (width, height), kDockGap * ui);
        ImGui::SetNextWindowPos (rect.position);
    }
    ImGui::SetNextWindowSizeConstraints (ImVec2 (width, 0.0f),
                                         ImVec2 (pageWidth || panel.widthPixels > 0.0f ? width : FLT_MAX, tallest));
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoCollapse | (spec.attached ? ImGuiWindowFlags_NoMove : 0);
    result.drawn = ImGui::Begin (spec.window, nullptr, flags);
    result.window = ImGui::GetCurrentWindow ();
    // The footer's height in the last frame, kept with the window: what the page leaves for it.
    ImGuiStorage* const storage = ImGui::GetStateStorage ();
    const ImGuiID footerId = ImGui::GetID ("##tapioca.footer");
    const float footerLast = storage->GetFloat (footerId, 0.0f);
    // ⚠️ TEXT FLOWS INSIDE THE PADDING (the user, 2026-10-03: text overflowed the panel and was
    // cut off). A panel of a set width wraps every line of its pages at its content's right
    // edge. The WINDOW's edge, not a cell's: a column sized to its content measures its text,
    // and text wrapped at that column's own edge would never let it grow.
    const bool setWidth = pageWidth || panel.widthPixels > 0.0f;
    const bool wrapped = result.drawn && setWidth;
    if (wrapped)
        ImGui::PushTextWrapPos (ImGui::GetCursorPosX () + ImGui::GetContentRegionAvail ().x);
    NavigationState fallback;
    NavigationState& nav = navigation ? *navigation : fallback;
    const bool commandAbove =
        spec.dockEdge == DockEdge::Top ||
        (spec.dockEdge == DockEdge::Floating && spec.dockHorizontal && spec.dockMin.y < view.y - spec.dockMax.y);
    if (result.drawn) {
        if (commandAbove)
            result.pressed = NavigationInput (spec, nav, result.textActive);
        const auto chosen = NavigationHeader (spec, held, nav, result.closed);
        if (!chosen.empty ())
            result.pressed = chosen;
        result.shown = held;
        shownLast = held;
        const bool available =
            std::any_of (spec.tabs.begin (), spec.tabs.end (), [&] (const auto& tab) { return tab.key == held; });
        if (available && page)
            result.scrolls = ScrollingPage (
                held, page, tallest,
                (std::max) (footerLast, (commandAbove ? 0.0f : 3.0f) * ImGui::GetFrameHeightWithSpacing ()), true,
                width - 2 * style.WindowPadding.x);
        else
            ImGui::TextWrapped ("This workflow is no longer available. Choose a workflow to continue.");
    }
    if (result.drawn) {
        const float before = ImGui::GetCursorPosY ();
        if (footer)
            footer ();
        const std::string command = commandAbove ? std::string () : NavigationInput (spec, nav, result.textActive);
        if (!command.empty ())
            result.pressed = command;
        storage->SetFloat (footerId, ImGui::GetCursorPosY () - before);
    }
    if (wrapped)
        ImGui::PopTextWrapPos ();
    ImGui::End ();
    ImGui::PopFont ();
    ImGui::PopStyleColor (colours);
    if (spec.attached)
        ImGui::PopStyleVar ();
    ImGui::PopStyleVar (kLookVars);
    // Dragged: where to, from the view's nearer side and its top, in logical pixels.
    if (moving && !spec.attached) {
        const ImVec2 pos = result.window->Pos, size = result.window->Size;
        const bool right = pos.x + size.x * 0.5f > view.x * 0.5f;
        placement.placed = true;
        placement.corner = right ? 1u : 0u;
        placement.offset[0] = (std::max) (right ? view.x - pos.x - size.x : pos.x, 0.0f) / scale;
        placement.offset[1] = (std::max) (pos.y, 0.0f) / scale;
        result.moved = true;
    }
    return result;
}

bool Section (const char* label, bool defaultOpen)
{
    return ImGui::CollapsingHeader (label, defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
}

namespace {

// The colours a display is drawn in: drafting greys first, then a few that read on a model.
struct Named {
    const char* name;
    uint32_t rgb; // 0xRRGGBB
};
constexpr Named kPalette[] = {
    { "Black", 0x1E1E1Eu }, { "Graphite", 0x3C3C3Cu }, { "Grey", 0x8C8C8Cu }, { "Light grey", 0xC8C8C8u },
    { "White", 0xFFFFFFu }, { "Blue", 0x2F6FD0u },     { "Teal", 0x1F9E89u }, { "Green", 0x3A9A3Au },
    { "Amber", 0xE8A33Du }, { "Orange", 0xE4572Eu },   { "Red", 0xD64545u },  { "Violet", 0x7B4FD0u },
};

// A swatch a line high, from `at`, in `rgba` made opaque -- a translucent fill shows its hue.
void Swatch (ImVec2 at, float height, uint32_t rgba)
{
    const float side = std::floor (height * 0.7f);
    const ImVec2 a (at.x, at.y + std::floor ((height - side) * 0.5f));
    ImDrawList* const draw = ImGui::GetWindowDrawList ();
    draw->AddRectFilled (a, ImVec2 (a.x + side, a.y + side), Packed (rgba | 0xFFu), 0.15f * side);
    draw->AddRect (a, ImVec2 (a.x + side, a.y + side), Packed (kTipEdgeRgba), 0.15f * side);
}

} // namespace

bool ColourChoice (const char* id, uint32_t& rgba, bool keepAlpha)
{
    const uint32_t rgb = rgba >> 8;
    const Named* shown = nullptr;
    for (const Named& named : kPalette)
        if (named.rgb == rgb)
            shown = &named;
    const float h = ImGui::GetTextLineHeight ();
    const float lead = std::floor (h * 0.7f) + ImGui::GetStyle ().ItemInnerSpacing.x;
    bool chosen = false;
    ImGui::PushID (id);
    const bool open = ImGui::BeginCombo ("##colour", nullptr, ImGuiComboFlags_CustomPreview);
    if (ImGui::BeginComboPreview ()) {
        const ImVec2 at = ImGui::GetCursorScreenPos ();
        Swatch (at, h, rgba);
        ImGui::SetCursorScreenPos (ImVec2 (at.x + lead, at.y));
        ImGui::TextUnformatted (shown != nullptr ? shown->name : "custom");
        ImGui::EndComboPreview ();
    }
    if (open) {
        for (const Named& named : kPalette) {
            const ImVec2 at = ImGui::GetCursorScreenPos ();
            ImGui::PushID (named.name);
            if (ImGui::Selectable ("##named", &named == shown) && &named != shown) {
                rgba = (named.rgb << 8) | (keepAlpha ? (rgba & 0xFFu) : 0xFFu);
                chosen = true;
            }
            ImGui::PopID ();
            Swatch (at, h, named.rgb << 8);
            ImGui::GetWindowDrawList ()->AddText (ImVec2 (at.x + lead, at.y), ImGui::GetColorU32 (ImGuiCol_Text),
                                                  named.name);
        }
        ImGui::EndCombo ();
    }
    ImGui::PopID ();
    return chosen;
}

bool HudSettings (uint32_t& fontStep, Placement& placement, bool& reset)
{
    bool changed = false;
    reset = false;
    ImGui::SeparatorText ("HUD");
    if (!ImGui::BeginTable ("##hudsettings", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings))
        return false;
    ImGui::TableSetupColumn ("##label", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn ("##value", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableNextRow ();
    ImGui::TableSetColumnIndex (0);
    ImGui::AlignTextToFramePadding ();
    ImGui::TextUnformatted ("Text size");
    ImGui::TableSetColumnIndex (1);
    const uint32_t step = (std::min) (fontStep, kFontStepCount - 1);
    ImGui::SetNextItemWidth (-FLT_MIN);
    if (ImGui::BeginCombo ("##textsize", Percent (kFontSteps[step]).c_str (), ImGuiComboFlags_HeightLargest)) {
        for (uint32_t k = 0; k < kFontStepCount; ++k)
            if (ImGui::Selectable (Percent (kFontSteps[k]).c_str (), k == step) && k != step) {
                fontStep = k;
                changed = true;
            }
        ImGui::EndCombo ();
    }
    ImGui::TableNextRow ();
    ImGui::TableSetColumnIndex (0);
    ImGui::AlignTextToFramePadding ();
    ImGui::TextUnformatted ("Position");
    ImGui::TableSetColumnIndex (1);
    ImGui::BeginDisabled (!placement.placed);
    if (ImGui::Button ("Reset##position", ImVec2 (-FLT_MIN, 0.0f)) && placement.placed) {
        placement.placed = false;
        reset = true;
        changed = true;
    }
    ImGui::EndDisabled ();
    ImGui::EndTable ();
    return changed;
}

namespace {

// A line of text in the look's muted colour, wrapped at a width that does not depend on the
// window it is in: an auto-sized panel would otherwise grow to fit the line. Never past the
// edge a panel of a set width wraps its pages at (Host).
void Muted (const std::string& text, const layers::Panel& look, uint32_t rgba = 0)
{
    ImGui::PushStyleColor (ImGuiCol_Text, Colour ((rgba & 0xFFu) != 0 ? rgba : WithAlpha (look.textRgba, 0.6f)));
    float wrap = ImGui::GetCursorPosX () + 18.0f * ImGui::GetFontSize ();
    if (const float edge = ImGui::GetCurrentWindow ()->DC.TextWrapPos; edge > 0.0f)
        wrap = (std::min) (wrap, edge);
    ImGui::PushTextWrapPos (wrap);
    ImGui::TextUnformatted (text.c_str ());
    ImGui::PopTextWrapPos ();
    ImGui::PopStyleColor ();
}

} // namespace

namespace {
size_t NumericAnchor (const std::string& value)
{
    size_t i = 0;
    if (!value.empty () && (value[0] == '-' || value[0] == '+'))
        ++i;
    const size_t first = i;
    while (i < value.size () && value[i] >= '0' && value[i] <= '9')
        ++i;
    if (i == first || (i < value.size () && value[i] != '.' && value[i] != ' '))
        return std::string::npos;
    return i;
}

std::string FigureText (const std::string& text, float width)
{
    std::string prefix = text;
    for (char& c : prefix)
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
    if (ImGui::CalcTextSize (prefix.c_str ()).x <= width)
        return prefix;
    while (!prefix.empty ()) {
        size_t end = prefix.size () - 1;
        while (end > 0 && (static_cast<unsigned char> (prefix[end]) & 0xC0) == 0x80)
            --end;
        prefix.resize (end);
        if (ImGui::CalcTextSize ((prefix + "...").c_str ()).x <= width)
            return prefix + "...";
    }
    return {};
}
} // namespace

std::string Cards (const std::vector<Card>& cards, const layers::Panel& look, float scale)
{
    std::string hovered;
    for (size_t c = 0; c < cards.size (); ++c) {
        const Card& card = cards[c];
        ImGui::PushID (int (c));
        if (!card.title.empty ())
            ImGui::SeparatorText (card.title.c_str ());
        float valueWidth = 0;
        float prefixWidth = 0, suffixWidth = 0;
        for (const auto& figure : card.figures) {
            valueWidth = (std::max) (valueWidth, ImGui::CalcTextSize (figure.value.c_str ()).x);
            const auto anchor = NumericAnchor (figure.value);
            if (card.alignDecimals && anchor != std::string::npos) {
                prefixWidth =
                    (std::max) (prefixWidth,
                                ImGui::CalcTextSize (figure.value.c_str (), figure.value.c_str () + anchor).x);
                suffixWidth = (std::max) (suffixWidth, ImGui::CalcTextSize (figure.value.c_str () + anchor).x);
            }
        }
        valueWidth = (std::max) (valueWidth, prefixWidth + suffixWidth);
        valueWidth = (std::min) (valueWidth, (std::max) (1.0f, ImGui::GetContentRegionAvail ().x -
                                                                   4 * ImGui::GetStyle ().CellPadding.x -
                                                                   ImGui::CalcTextSize ("...").x));
        if (!card.figures.empty () &&
            ImGui::BeginTable ("##figures", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
            ImGui::TableSetupColumn ("##name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn ("##value", ImGuiTableColumnFlags_WidthFixed, valueWidth);
            ImGui::PushTextWrapPos (-1); // Values and units stay together regardless of the shell's wrap setting.
            for (const Figure& figure : card.figures) {
                ImGui::TableNextRow ();
                ImGui::TableSetColumnIndex (0);
                ImGui::PushStyleColor (ImGuiCol_Text, Colour (WithAlpha (look.textRgba, 0.72f)));
                const auto label = FigureText (figure.label, ImGui::GetContentRegionAvail ().x);
                ImGui::TextUnformatted (label.c_str ());
                if (ImGui::IsItemHovered () && !figure.hoverKey.empty ())
                    hovered = figure.hoverKey;
                if (label != figure.label)
                    Tip (figure.label, TipSide::Above);
                ImGui::PopStyleColor ();
                ImGui::TableSetColumnIndex (1);
                ImGui::PushStyleColor (ImGuiCol_Text,
                                       Colour ((figure.rgba & 0xFFu) != 0 ? figure.rgba : look.textRgba));
                const auto anchor = NumericAnchor (figure.value);
                const auto value = FigureText (figure.value, ImGui::GetContentRegionAvail ().x);
                if (card.alignDecimals && anchor != std::string::npos &&
                    prefixWidth + suffixWidth <= ImGui::GetContentRegionAvail ().x + 0.5f) {
                    // Separate runs keep the dot on the same pixel after ImGui's text-origin snapping,
                    // even with proportional fonts and fractional font scaling.
                    const auto start = ImGui::GetCursorScreenPos ();
                    const char *begin = figure.value.c_str (), *dot = begin + anchor;
                    const float prefix =
                        ImGui::GetFont ()->CalcTextSizeA (ImGui::GetFontSize (), FLT_MAX, 0, begin, dot).x;
                    auto* draw = ImGui::GetWindowDrawList ();
                    const auto colour = ImGui::GetColorU32 (ImGuiCol_Text);
                    draw->AddText ({ start.x + prefixWidth - prefix, start.y }, colour, begin, dot);
                    draw->AddText ({ start.x + prefixWidth, start.y }, colour, dot);
                    ImGui::Dummy ({ prefixWidth + suffixWidth, ImGui::GetTextLineHeight () });
                }
                else
                    ImGui::TextUnformatted (value.c_str ());
                if (ImGui::IsItemHovered () && !figure.hoverKey.empty ())
                    hovered = figure.hoverKey;
                if (value != figure.value)
                    Tip (figure.value, TipSide::Above);
                ImGui::PopStyleColor ();
            }
            ImGui::PopTextWrapPos ();
            ImGui::EndTable ();
        }
        if (card.progress >= 0.0) {
            const float fraction = float ((std::min) (card.progress, 1.0));
            ImGui::PushStyleColor (ImGuiCol_PlotHistogram, Colour (look.accentRgba));
            ImGui::ProgressBar (fraction, ImVec2 (14.0f * ImGui::GetFontSize (), 12.0f * scale),
                                card.progressText.empty () ? "" : card.progressText.c_str ());
            ImGui::PopStyleColor ();
        }
        if (!card.note.empty ())
            Muted (card.note, look, card.noteRgba);
        ImGui::PopID ();
    }
    return hovered;
}

void SelectionList (const SelectionPage& page, const layers::Panel& look, float scale)
{
    (void) scale;
    if (!page.known) {
        Muted ("The selection has not been read yet", look);
        return;
    }
    if (page.count == 0) {
        Muted ("Nothing selected", look);
        if (!page.note.empty ())
            Muted (page.note, look);
        return;
    }
    ImGui::Text ("%u selected", page.count);
    if (!page.elements.empty () && ImGui::BeginTable ("##selection", 3,
                                                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings |
                                                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn ("Element");
        ImGui::TableSetupColumn ("Layer");
        ImGui::TableSetupColumn ("Storey");
        ImGui::TableHeadersRow ();
        for (const SelectedElement& element : page.elements) {
            ImGui::TableNextRow ();
            ImGui::TableSetColumnIndex (0);
            const std::string name = element.id.empty ()
                                         ? element.type
                                         : (element.type.empty () ? element.id : element.type + "  " + element.id);
            ImGui::TextUnformatted (name.empty () ? element.guid.c_str () : name.c_str ());
            ImGui::TableSetColumnIndex (1);
            ImGui::TextUnformatted (element.layer.c_str ());
            ImGui::TableSetColumnIndex (2);
            ImGui::TextUnformatted (element.storey.c_str ());
        }
        ImGui::EndTable ();
    }
    if (page.count > page.elements.size ())
        Muted ("and " + std::to_string (page.count - uint32_t (page.elements.size ())) + " more", look);
    if (!page.note.empty ())
        Muted (page.note, look);
}

} // namespace hudshell
} // namespace archviz
} // namespace geomsrv
