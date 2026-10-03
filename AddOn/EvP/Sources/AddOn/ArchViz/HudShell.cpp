// ArchViz/HudShell -- see the header.

#include "ArchViz/HudShell.hpp"

#include <imgui_internal.h> // ImGuiWindow: where the panel is, and whether it is being dragged

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <utility>

namespace geomsrv {
namespace archviz {
namespace hudshell {

namespace layers = overlaylayers;

namespace {

// The close button at the end of the tab row.
constexpr char kClose[] = "\xC3\x97##tapioca.close";

} // namespace

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

void BaseStyle (float scale)
{
    ImGuiStyle& style = ImGui::GetStyle ();
    style = ImGuiStyle ();
    ImGui::StyleColorsDark (&style);
    // ⚠️ DENSE (the user, 2026-09-30: a small, content-dense inspection panel, not unused
    // space): a pixel less round every frame and between items than ImGui's own.
    style.FramePadding = ImVec2 (4.0f, 2.0f);
    style.ItemSpacing = ImVec2 (6.0f, 3.0f);
    style.ItemInnerSpacing = ImVec2 (4.0f, 3.0f);
    style.CellPadding = ImVec2 (4.0f, 1.0f);
    style.ScaleAllSizes (scale);
    style.WindowMinSize = ImVec2 (1.0f, 1.0f);
    style.FrameRounding = 2.0f * scale;
}

int PushLook (const layers::Panel& panel, float scale)
{
    ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding,
                         ImVec2 (panel.paddingPixels * scale, panel.paddingPixels * scale));
    ImGui::PushStyleVar (ImGuiStyleVar_WindowRounding, panel.roundingPixels * scale);
    ImGui::PushStyleVar (ImGuiStyleVar_WindowBorderSize,
                         (panel.borderRgba & 0xFFu) != 0 ? (std::max) (1.0f, scale) : 0.0f);
    const uint32_t text = panel.textRgba;
    const uint32_t accent = panel.accentRgba;
    const std::pair<ImGuiCol, uint32_t> colours[] = {
        { ImGuiCol_WindowBg, panel.backgroundRgba },
        { ImGuiCol_Border, panel.borderRgba },
        { ImGuiCol_Text, text },
        { ImGuiCol_Separator, WithAlpha (text, 0.3f) },
        { ImGuiCol_FrameBg, WithAlpha (text, 0.12f) },
        { ImGuiCol_TableHeaderBg, WithAlpha (text, 0.12f) },
        { ImGuiCol_TableBorderLight, WithAlpha (text, 0.2f) },
        { ImGuiCol_TableBorderStrong, WithAlpha (text, 0.3f) },
        { ImGuiCol_TableRowBgAlt, WithAlpha (text, 0.05f) },
        // ⚠️ WHAT THE POINTER CAN PRESS IS TINTED WITH THE ACCENT (the user, 2026-09-29): a
        // button reads as one at rest -- a faint fill -- and plainly when pointed at and
        // pressed, as a section's row does.
        { ImGuiCol_Header, WithAlpha (accent, 0.14f) }, // a dropdown's chosen option
        { ImGuiCol_HeaderHovered, WithAlpha (accent, 0.16f) },
        { ImGuiCol_HeaderActive, WithAlpha (accent, 0.28f) },
        { ImGuiCol_Button, WithAlpha (text, 0.07f) },
        { ImGuiCol_ButtonHovered, WithAlpha (accent, 0.30f) },
        { ImGuiCol_ButtonActive, WithAlpha (accent, 0.48f) },
        { ImGuiCol_FrameBgHovered, WithAlpha (accent, 0.20f) },
        { ImGuiCol_FrameBgActive, WithAlpha (accent, 0.32f) },
        { ImGuiCol_CheckMark, accent },
        { ImGuiCol_SliderGrab, accent },
        { ImGuiCol_SliderGrabActive, accent },
        // A tab bar: the tab shown tinted, a line of the accent over it. The same whether
        // ImGui thinks the panel focused or not -- the HUD takes no focus the user sees.
        { ImGuiCol_Tab, WithAlpha (text, 0.06f) },
        { ImGuiCol_TabHovered, WithAlpha (accent, 0.30f) },
        { ImGuiCol_TabSelected, WithAlpha (accent, 0.20f) },
        { ImGuiCol_TabSelectedOverline, accent },
        { ImGuiCol_TabDimmed, WithAlpha (text, 0.06f) },
        { ImGuiCol_TabDimmedSelected, WithAlpha (accent, 0.20f) },
        { ImGuiCol_TabDimmedSelectedOverline, accent },
        // Its tooltips in its own colours, nearly opaque over the model.
        { ImGuiCol_PopupBg, (panel.backgroundRgba & 0xFFFFFF00u) | 0xF6u },
    };
    for (const auto& colour : colours)
        ImGui::PushStyleColor (colour.first, Colour (colour.second));
    return int (sizeof (colours) / sizeof (colours[0]));
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

float FontScaleOfStep (uint32_t step)
{
    return kFontSteps[(std::min) (step, kFontStepCount - 1)];
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
            return circle.progress >= 0.0f ? Mix (kBusyRgba, ink, circle.progress) : kBusyRgba;
        case Phase::Attention:
            // Once a second, half of it faint: a still layout taken twice a second alternates.
            return std::fmod (seconds, 1.0) < 0.5 ? ink : WithAlpha (ink, 0.25f);
        case Phase::Error:
            return kErrorRgba;
    }
    return ink;
}

// ⚠️ ONE TAB, ITS TITLE TURNED, AND ITS SURFACES' CIRCLES (the user, 2026-09-30: one tab, its
// text rotated 90 degrees, that opens and closes the panel; 2026-10-03: the overlay's circle at
// its top, the separate viewer's at its bottom -- a switch between the two).
DockPress DockTab (const char* id, const std::string& label, const layers::Panel& panel, bool open, const Circle& top,
                   const Circle* bottom, ImVec2 padding, float scale)
{
    DockPress press;
    const ImVec2 text = ImGui::CalcTextSize (label.c_str ());
    const float across = std::ceil (text.y + 2.0f * padding.x);
    const float r = (std::min) (6.0f * scale, across * 0.5f);
    const uint32_t fill = open ? panel.accentRgba : panel.backgroundRgba;
    const uint32_t ink = open ? Contrast (panel.accentRgba) : panel.textRgba;
    const double seconds = ImGui::GetTime ();
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    // One part: its ground, its tint when pointed at and pressed, its edge while closed.
    const auto part = [&] (ImVec2 a, ImVec2 b, ImDrawFlags corners) {
        const bool hovered = ImGui::IsItemHovered (), held = ImGui::IsItemActive ();
        draw->AddRectFilled (a, b, Packed (fill), r, corners);
        if (hovered || held) {
            // White over the open tab, the accent over the closed one.
            const uint32_t tint = open ? WithAlpha (0xFFFFFFFFu, held ? 0.30f : 0.18f)
                                       : WithAlpha (panel.accentRgba, held ? 0.48f : 0.30f);
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
        const float radius = std::floor (across * 0.22f) + 0.5f;
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
                draw->PathStroke (Packed (kBusyRgba), 0, stroke);
            }
        }
        if (!c.tip.empty () && ImGui::IsItemHovered ())
            ImGui::SetTooltip ("%s", c.tip.c_str ());
        return pressed;
    };
    ImGui::PushID (id);
    ImVec2 topFrom, topTo, bottomFrom, bottomTo;
    press.top = circle ("##shown", top, ImDrawFlags_RoundCornersTopLeft, topFrom, topTo);
    // The title, under it.
    press.title = ImGui::InvisibleButton ("##title", ImVec2 (across, std::ceil (text.x + 2.0f * padding.y)));
    const ImVec2 a = ImGui::GetItemRectMin (), b = ImGui::GetItemRectMax ();
    part (a, b, bottom != nullptr ? ImDrawFlags_RoundCornersNone : ImDrawFlags_RoundCornersBottomLeft);
    ImVec2 last = b;
    if (bottom != nullptr) {
        press.bottom = circle ("##viewer", *bottom, ImDrawFlags_RoundCornersBottomLeft, bottomFrom, bottomTo);
        last = bottomTo;
    }
    ImGui::PopID ();
    if (!open) {
        const uint32_t edge = (panel.borderRgba & 0xFFu) != 0 ? panel.borderRgba : WithAlpha (panel.textRgba, 0.25f);
        draw->AddRect (topFrom, last, Packed (edge), r, ImDrawFlags_RoundCornersLeft, (std::max) (1.0f, scale));
    }
    // Laid out across, round the title's middle, then turned: every corner stays on a
    // whole pixel. Unclipped while across -- the window is as narrow as the text is tall.
    const ImVec2 middle (std::floor ((a.x + b.x) * 0.5f), std::floor ((a.y + b.y) * 0.5f));
    const int first = draw->VtxBuffer.Size;
    draw->PushClipRect (ImVec2 (-32768.0f, -32768.0f), ImVec2 (32768.0f, 32768.0f), false);
    draw->AddText (ImVec2 (middle.x - std::floor (text.x * 0.5f), middle.y - std::floor (text.y * 0.5f)), Packed (ink),
                   label.c_str ());
    draw->PopClipRect ();
    for (int k = first; k < draw->VtxBuffer.Size; ++k) {
        ImDrawVert& v = draw->VtxBuffer[k];
        const float dx = v.pos.x - middle.x, dy = v.pos.y - middle.y;
        v.pos = ImVec2 (middle.x - dy, middle.y + dx);
    }
    return press;
}

HostResult Host (const HostSpec& spec, const std::string& held, std::string& shownLast, Placement& placement,
                 const std::function<void (const std::string& key)>& page, const std::function<void ()>& footer)
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
    if (!moving) {
        if (placement.placed) {
            const ImVec2 size = existing != nullptr ? existing->Size : ImVec2 (0.0f, 0.0f);
            const bool right = (placement.corner & 1u) != 0, bottom = (placement.corner & 2u) != 0;
            float x = right ? view.x - placement.offset[0] * scale - size.x : placement.offset[0] * scale;
            float y = bottom ? view.y - placement.offset[1] * scale - size.y : placement.offset[1] * scale;
            // Inside the view, however it was resized.
            x = (std::min) ((std::max) (x, 0.0f), (std::max) (view.x - size.x, 0.0f));
            y = (std::min) ((std::max) (y, 0.0f), (std::max) (view.y - size.y, 0.0f));
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
    ImGui::PushFont (spec.font, panel.sizePixels * ui);
    // As wide as its tab row needs, or the look's width when that is wider.
    const ImGuiStyle& style = ImGui::GetStyle ();
    float row = 2.0f * style.WindowPadding.x;
    for (const HostTab& tab : spec.tabs)
        row += ImGui::CalcTextSize (tab.title.c_str ()).x + 2.0f * style.FramePadding.x + style.ItemInnerSpacing.x;
    row += ImGui::CalcTextSize (kClose, nullptr, true).x + 2.0f * style.FramePadding.x + style.ItemInnerSpacing.x;
    const float width = (std::max) (std::ceil (row), panel.widthPixels * ui);
    ImGui::SetNextWindowSizeConstraints (ImVec2 (width, 0.0f),
                                         ImVec2 (panel.widthPixels > 0.0f ? width : FLT_MAX, FLT_MAX));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoCollapse;
    result.drawn = ImGui::Begin (spec.window, nullptr, flags);
    result.window = ImGui::GetCurrentWindow ();
    if (result.drawn && ImGui::BeginTabBar ("##hud")) {
        std::string now;
        const auto asked = [&] (const std::string& key) {
            return key == held && shownLast != held ? ImGuiTabItemFlags_SetSelected : 0;
        };
        for (const HostTab& tab : spec.tabs) {
            if (ImGui::BeginTabItem ((tab.title + "###" + tab.key).c_str (), nullptr, asked (tab.key))) {
                now = tab.key;
                if (page)
                    page (tab.key);
                ImGui::EndTabItem ();
            }
        }
        result.closed = ImGui::TabItemButton (kClose, ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip);
        ImGui::EndTabBar ();
        // The user's press on another tab: one this context did not show last frame, and
        // not the held one it was asked to show.
        if (!now.empty () && !shownLast.empty () && now != shownLast && now != held)
            result.pressed = now;
        shownLast = now;
        result.shown = now;
    }
    if (result.drawn && footer)
        footer ();
    ImGui::End ();
    ImGui::PopFont ();
    ImGui::PopStyleColor (colours);
    ImGui::PopStyleVar (kLookVars);
    // Dragged: where to, from the view's corner nearest it, in logical pixels.
    if (moving) {
        const ImVec2 pos = result.window->Pos, size = result.window->Size;
        const bool right = pos.x + size.x * 0.5f > view.x * 0.5f, bottom = pos.y + size.y * 0.5f > view.y * 0.5f;
        placement.placed = true;
        placement.corner = uint8_t ((right ? 1u : 0u) | (bottom ? 2u : 0u));
        placement.offset[0] = (std::max) (right ? view.x - pos.x - size.x : pos.x, 0.0f) / scale;
        placement.offset[1] = (std::max) (bottom ? view.y - pos.y - size.y : pos.y, 0.0f) / scale;
        result.moved = true;
    }
    return result;
}

namespace {

// A line of text in the look's muted colour, wrapped at a width that does not depend on the
// window it is in: an auto-sized panel would otherwise grow to fit the line.
void Muted (const std::string& text, const layers::Panel& look, uint32_t rgba = 0)
{
    ImGui::PushStyleColor (ImGuiCol_Text, Colour ((rgba & 0xFFu) != 0 ? rgba : WithAlpha (look.textRgba, 0.6f)));
    ImGui::PushTextWrapPos (ImGui::GetCursorPosX () + 18.0f * ImGui::GetFontSize ());
    ImGui::TextUnformatted (text.c_str ());
    ImGui::PopTextWrapPos ();
    ImGui::PopStyleColor ();
}

} // namespace

void Cards (const std::vector<Card>& cards, const layers::Panel& look, float scale)
{
    for (size_t c = 0; c < cards.size (); ++c) {
        const Card& card = cards[c];
        ImGui::PushID (int (c));
        if (!card.title.empty ())
            ImGui::SeparatorText (card.title.c_str ());
        if (!card.figures.empty () &&
            ImGui::BeginTable ("##figures", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
            for (const Figure& figure : card.figures) {
                ImGui::TableNextRow ();
                ImGui::TableSetColumnIndex (0);
                ImGui::PushStyleColor (ImGuiCol_Text, Colour (WithAlpha (look.textRgba, 0.72f)));
                ImGui::TextUnformatted (figure.label.c_str ());
                ImGui::PopStyleColor ();
                ImGui::TableSetColumnIndex (1);
                ImGui::PushStyleColor (ImGuiCol_Text,
                                       Colour ((figure.rgba & 0xFFu) != 0 ? figure.rgba : look.textRgba));
                ImGui::TextUnformatted (figure.value.c_str ());
                ImGui::PopStyleColor ();
            }
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
