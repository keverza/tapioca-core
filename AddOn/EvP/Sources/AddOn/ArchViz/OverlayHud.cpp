// ArchViz/OverlayHud -- see the header.

#include "ArchViz/OverlayHud.hpp"

#include "ArchViz/ImGuiContextLock.hpp"
#include "ArchViz/OverlayHudItems.hpp"

#include <imgui.h>
#include <imgui_internal.h> // ImGuiWindow: which draw list is whose

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>

namespace geomsrv {
namespace archviz {
namespace overlayhud {

namespace layers = overlaylayers;

using items::Colour;
using items::Unpacked;
using items::WithAlpha;

namespace {

// A set is laid out in this many frames: an auto-sized window measures itself in its
// first and settles in its second; the third is the one drawn. A press and a release
// take a frame each on top (ImGui spreads input events over frames).
constexpr int kFrames = 3;
// And the whole set again at most this often, while the atlas keeps growing.
constexpr int kAttempts = 3;
// A panel's title, a little larger than its items.
constexpr float kTitleScale = 1.2f;
// The frames after the first advance ImGui's clock by almost nothing: the first carries
// the time since the last layout, so a double click is timed as the user made it.
constexpr float kSettleSeconds = 1.0e-4f;
// The dock's tabs: their font, their padding round the title, the gap between them, and
// the gap between the dock and a panel on the view's right column.
constexpr float kDockFontPixels = 13.0f;
constexpr float kDockPadding[2] = { 12.0f, 7.0f };
constexpr float kDockSpacing = 4.0f;
constexpr float kDockGap = 8.0f;

// The style every panel starts from; each pushes its own colours and spacing over it.
void BaseStyle (float scale)
{
    ImGuiStyle& style = ImGui::GetStyle ();
    style = ImGuiStyle ();
    ImGui::StyleColorsDark (&style);
    style.ScaleAllSizes (scale);
    style.WindowMinSize = ImVec2 (1.0f, 1.0f);
    style.FrameRounding = 2.0f * scale;
    // The title centred, the collapse arrow before it.
    style.WindowTitleAlign = ImVec2 (0.5f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
}

constexpr int kStyleVars = 3;

// A panel's own look over the base; the number of colours pushed.
int PushPanelStyle (const layers::Panel& panel, float scale)
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
        // The title bar is the panel: a header, not a coloured strip.
        { ImGuiCol_TitleBg, panel.backgroundRgba },
        { ImGuiCol_TitleBgActive, panel.backgroundRgba },
        { ImGuiCol_TitleBgCollapsed, panel.backgroundRgba },
        // ⚠️ WHAT THE POINTER CAN PRESS IS TINTED WITH THE ACCENT (the user, 2026-09-29): a
        // button reads as one at rest -- a faint fill -- and plainly when pointed at and
        // pressed, as a section's row and the title bar's arrow do.
        { ImGuiCol_Header, 0x00000000u },
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
        // Its tooltips in its own colours, nearly opaque over the model.
        { ImGuiCol_PopupBg, (panel.backgroundRgba & 0xFFFFFF00u) | 0xF6u },
    };
    for (const auto& colour : colours)
        ImGui::PushStyleColor (colour.first, Colour (colour.second));
    return int (sizeof (colours) / sizeof (colours[0]));
}

} // namespace

void Place (const layers::Panel& panel, float width, float height, float scale, float fraction[2], float offset[2],
            float inset)
{
    const int column = int (panel.anchor) % 3, row = int (panel.anchor) / 3; // 0 near, 1 middle, 2 far
    fraction[0] = float (column) * 0.5f;
    fraction[1] = float (row) * 0.5f;
    const float inwardX = column == 2 ? -1.0f : 1.0f, inwardY = row == 2 ? -1.0f : 1.0f;
    offset[0] = -width * float (column) * 0.5f + inwardX * panel.offsetPixels[0] * scale - (column == 2 ? inset : 0.0f);
    offset[1] = -height * float (row) * 0.5f + inwardY * panel.offsetPixels[1] * scale;
}

// What the user did to each panel, by its key (the header's third note).
struct State {
    struct Panel {
        bool collapsed = false;            // in the dock
        std::map<uint32_t, bool> sections; // by item index
    };
    std::map<std::string, Panel> panels;
};

std::shared_ptr<State> NewState ()
{
    return std::make_shared<State> ();
}

void ClearState (State& state)
{
    state = State {};
}

struct Engine::Impl {
    ImGuiContext* context = nullptr;
    ImFont* font = nullptr;
    std::vector<uint8_t> fontBytes; // ImGui reads it for as long as the atlas lives
    // A panel's own fonts, by path, their bytes kept as long; a path that failed maps
    // to the bundled font, tried once.
    FontLoader loader;
    std::map<std::string, ImFont*> fonts;
    std::vector<std::unique_ptr<std::vector<uint8_t>>> fontData;
    std::string fontError;
    // One slot per ImGui texture, its TexID the slot's index plus one.
    std::vector<ImTextureData*> textures;
    std::vector<std::shared_ptr<const overlaytext::Page>> pages;
    uint64_t atlasVersion = 0;
    Stats stats;
    bool ready = false;

    // What the user did to each panel: this engine's own, or the views' shared one.
    using PanelState = State::Panel;
    std::shared_ptr<State> store = NewState ();
    // The layer of the panel being laid out, and what the pointer is on this frame.
    std::string layer;
    Layout::Highlight highlight;
    bool hand = false;
    // The panels' windows in the frame being laid out, by the panel's place in the set,
    // and the dock's after them.
    std::vector<ImGuiWindow*> windows;
    // The dock's width this frame: how far the view's right column moves in.
    float inset = 0.0f;
    std::chrono::steady_clock::time_point lastBuild {};

    PanelState& StateOf (const std::string& key, const layers::Panel& panel)
    {
        const auto made = store->panels.try_emplace (key);
        if (made.second)
            made.first->second.collapsed = panel.collapsed && !panel.title.empty ();
        return made.first->second;
    }

    // Between frames, the context current and locked: ImGui adds a font to the atlas
    // only then.
    ImFont* FontFor (const std::string& path)
    {
        if (path.empty () || !loader)
            return font;
        const auto found = fonts.find (path);
        if (found != fonts.end ())
            return found->second != nullptr ? found->second : font;
        auto bytes = std::make_unique<std::vector<uint8_t>> ();
        std::string error;
        ImFont* added = nullptr;
        if (loader (path, *bytes, error)) {
            ImFontConfig config;
            config.FontDataOwnedByAtlas = false;
            added = ImGui::GetIO ().Fonts->AddFontFromMemoryTTF (bytes->data (), int (bytes->size ()), 16.0f, &config);
            if (added == nullptr)
                error = "ImGui could not load \"" + path + "\"";
        }
        if (added != nullptr) {
            fontData.push_back (std::move (bytes));
            ++stats.fonts;
        }
        else
            fontError = error;
        fonts[path] = added;
        return added != nullptr ? added : font;
    }

    // ImGui's texture requests, honoured: every created or updated texture becomes a
    // page with a new id, whole.
    void Sync (ImDrawData* data)
    {
        if (data == nullptr || data->Textures == nullptr)
            return;
        for (ImTextureData* texture : *data->Textures) {
            if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates) {
                size_t slot = 0;
                while (slot < textures.size () && textures[slot] != texture)
                    ++slot;
                if (slot == textures.size ()) {
                    textures.push_back (texture);
                    pages.push_back (nullptr);
                }
                auto page = std::make_shared<overlaytext::Page> ();
                page->id = overlaytext::NewPageId ();
                page->width = texture->Width;
                page->height = texture->Height;
                const size_t count = size_t (texture->Width) * size_t (texture->Height);
                const unsigned char* source = static_cast<const unsigned char*> (texture->GetPixels ());
                if (texture->BytesPerPixel == 4) {
                    page->pixels.assign (source, source + count * 4);
                }
                else {
                    // Alpha8: white, with ImGui's coverage as alpha.
                    page->pixels.resize (count * 4);
                    for (size_t i = 0; i < count; ++i) {
                        page->pixels[i * 4] = page->pixels[i * 4 + 1] = page->pixels[i * 4 + 2] = 255;
                        page->pixels[i * 4 + 3] = source[i];
                    }
                }
                pages[slot] = std::move (page);
                texture->SetTexID (ImTextureID (slot + 1));
                texture->SetStatus (ImTextureStatus_OK);
                ++atlasVersion;
                ++stats.atlasVersions;
            }
            else if (texture->Status == ImTextureStatus_WantDestroy && texture->UnusedFrames > 0) {
                for (size_t slot = 0; slot < textures.size (); ++slot)
                    if (textures[slot] == texture) {
                        textures[slot] = nullptr;
                        pages[slot] = nullptr;
                    }
                texture->SetTexID (ImTextureID_Invalid);
                texture->SetStatus (ImTextureStatus_Destroyed);
            }
        }
    }

    void Items (const layers::Panel& panel, PanelState& state, float scale)
    {
        // What an item without its own width spans: the panel's width when it has one,
        // otherwise a width that does not depend on the layout it is part of.
        const float font = ImGui::GetFontSize ();
        const float width =
            panel.widthPixels > 0.0f ? (std::max) (ImGui::GetContentRegionAvail ().x, 1.0f) : 14.0f * font;
        // Inside a closed section its items are not laid out at all.
        bool shown = true;
        size_t i = 0;
        while (i < panel.items.size ()) {
            const layers::PanelItem& item = panel.items[i];
            if (!shown && item.kind != layers::ItemKind::Section) {
                ++i;
                continue;
            }
            ImGui::PushID (int (i));
            switch (item.kind) {
                case layers::ItemKind::Row: {
                    size_t end = i;
                    while (end < panel.items.size () && panel.items[end].kind == layers::ItemKind::Row)
                        ++end;
                    items::Rows (panel, i, end, scale);
                    ImGui::PopID ();
                    i = end;
                    continue;
                }
                case layers::ItemKind::Text:
                    items::Text (panel, item, width, scale);
                    break;
                case layers::ItemKind::Separator:
                    ImGui::Separator ();
                    break;
                case layers::ItemKind::Spacing:
                    ImGui::Dummy (ImVec2 (1.0f, (item.heightPixels > 0.0f ? item.heightPixels : 6.0f) * scale));
                    break;
                case layers::ItemKind::Progress:
                    items::Progress (item, width, scale);
                    break;
                case layers::ItemKind::Swatch: {
                    // A run of swatches is one key: a grid when it has values or columns.
                    size_t end = i;
                    while (end < panel.items.size () && panel.items[end].kind == layers::ItemKind::Swatch)
                        ++end;
                    items::Keys (panel, i, end, width, scale);
                    ImGui::PopID ();
                    i = end;
                    continue;
                }
                case layers::ItemKind::Metrics:
                    items::Metrics (panel, item, width, scale);
                    break;
                case layers::ItemKind::Stack:
                    items::Stack (panel, item, width, scale);
                    break;
                case layers::ItemKind::Bars:
                    items::Bars (panel, item, width, scale);
                    break;
                case layers::ItemKind::Ramp: {
                    double band[2] = {};
                    // A ramp in a panel describes its own layer's heatmaps.
                    if (items::Ramp (panel, item, width, scale, band))
                        highlight = { true, layer, band[0], band[1] };
                    break;
                }
                case layers::ItemKind::Plot:
                    items::Plot (item, width, scale);
                    break;
                case layers::ItemKind::Table:
                    items::Table (item);
                    break;
                case layers::ItemKind::Section: {
                    bool& open = state.sections.try_emplace (uint32_t (i), item.open).first->second;
                    items::Section (panel, item, open, scale);
                    shown = open;
                    break;
                }
            }
            ImGui::PopID ();
            ++i;
        }
    }

    // One panel's window, at its anchor on the view: its title bar -- whose close button
    // sends it to the dock -- when it has a title, then its items. Nothing while it is in
    // the dock.
    void Window (const layers::Panel& panel, const std::string& key, size_t index, float scale, ImVec2 view)
    {
        PanelState& state = StateOf (key, panel);
        layer = key.substr (0, key.rfind ('#'));
        const bool titled = !panel.title.empty ();
        if (titled && state.collapsed)
            return;
        const int column = int (panel.anchor) % 3, row = int (panel.anchor) / 3;
        const ImVec2 pivot (float (column) * 0.5f, float (row) * 0.5f);
        const float inwardX = column == 2 ? -1.0f : 1.0f, inwardY = row == 2 ? -1.0f : 1.0f;
        // The panel's own anchor point on the view's, `offsetPixels` inwards (Place), the
        // right column beside the dock.
        ImGui::SetNextWindowPos (
            ImVec2 (pivot.x * view.x + inwardX * panel.offsetPixels[0] * scale - (column == 2 ? inset : 0.0f),
                    pivot.y * view.y + inwardY * panel.offsetPixels[1] * scale),
            ImGuiCond_Always, pivot);
        if (panel.widthPixels > 0.0f) {
            const float w = panel.widthPixels * scale;
            ImGui::SetNextWindowSizeConstraints (ImVec2 (w, 0.0f), ImVec2 (w, FLT_MAX));
        }
        // ### keeps the window's identity -- and so its state -- whatever its title says.
        const std::string name = (titled ? panel.title : std::string ()) + "###tapioca.panel." + key;
        const int colours = PushPanelStyle (panel, scale);
        ImFont* const face = FontFor (panel.font);
        ImGui::PushFont (face, panel.sizePixels * scale * (titled ? kTitleScale : 1.0f));
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoCollapse;
        if (!titled)
            flags |= ImGuiWindowFlags_NoTitleBar;
        // The close button: pressed, the panel goes to the dock -- drawn this frame still,
        // not from the next.
        bool kept = true;
        const bool open = ImGui::Begin (name.c_str (), titled ? &kept : nullptr, flags);
        ImGui::PopFont (); // the title bar is drawn
        windows[index] = ImGui::GetCurrentWindow ();
        if (!kept)
            state.collapsed = true;
        if (open && titled) {
            // A hairline under the title bar: the design's header, above the first item.
            ImGuiWindow* const window = ImGui::GetCurrentWindow ();
            const ImRect bar = window->TitleBarRect ();
            window->DrawList->AddLine (ImVec2 (bar.Min.x, bar.Max.y), bar.Max, ImGui::GetColorU32 (ImGuiCol_Separator),
                                       (std::max) (1.0f, scale));
        }
        if (open) {
            ImGui::PushFont (face, panel.sizePixels * scale);
            Items (panel, state, scale);
            ImGui::PopFont ();
        }
        ImGui::End ();
        ImGui::PopStyleColor (colours);
        ImGui::PopStyleVar (kStyleVars);
    }

    // A legend's bar pointed at: the value there, beside the bar at the pointer.
    void LegendTips (const std::vector<LegendBar>& legends, float scale, ImVec2 view)
    {
        const ImVec2 mouse = ImGui::GetIO ().MousePos;
        for (const LegendBar& bar : legends) {
            if (bar.legend == nullptr)
                continue;
            const ImVec2 a (bar.rect[0], bar.rect[1]), b (bar.rect[2], bar.rect[3]);
            if (b.x <= a.x || b.y <= a.y || !ImGui::IsMouseHoveringRect (a, b, false))
                continue;
            const layers::Legend& legend = *bar.legend;
            double band[2] = {};
            if (legend.horizontal) {
                items::ValueTip (legend.colormap, (mouse.x - a.x) / (b.x - a.x), legend.colormap.min,
                                 legend.colormap.max, legend.decimals, legend.unit,
                                 ImVec2 (mouse.x, a.y - 4.0f * scale), ImVec2 (0.5f, 1.0f), scale, band);
            }
            else {
                // Towards the middle of the view, away from the edge the legend sits at.
                const bool left = a.x > view.x * 0.5f;
                items::ValueTip (legend.colormap, (b.y - mouse.y) / (b.y - a.y), legend.colormap.min,
                                 legend.colormap.max, legend.decimals, legend.unit,
                                 ImVec2 (left ? a.x - 6.0f * scale : b.x + 6.0f * scale, mouse.y),
                                 ImVec2 (left ? 1.0f : 0.0f, 0.5f), scale, band);
            }
            highlight = { true, bar.layer, band[0], band[1] };
            return;
        }
    }

    // ⚠️ THE DOCK: a tab per titled panel down the view's right edge, half-way down, all
    // as wide as the widest title (the user, 2026-09-29). Filled with the panel's accent
    // while the panel is open; its card's own colours while the panel is in the dock. A
    // press on it opens or closes the panel -- in this frame, as the dock is laid out
    // before the panels.
    void Dock (const std::vector<const layers::Panel*>& panels, const std::vector<std::string>& keys, float scale,
               ImVec2 view)
    {
        inset = 0.0f;
        std::vector<size_t> titled;
        for (size_t i = 0; i < panels.size (); ++i)
            if (!panels[i]->title.empty ())
                titled.push_back (i);
        if (titled.empty ())
            return;
        ImGui::SetNextWindowPos (ImVec2 (view.x, view.y * 0.5f), ImGuiCond_Always, ImVec2 (1.0f, 0.5f));
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (0.0f, 0.0f));
        ImGui::PushStyleVar (ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar (ImGuiStyleVar_ItemSpacing, ImVec2 (0.0f, kDockSpacing * scale));
        ImGui::PushFont (font, kDockFontPixels * scale);
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground;
        const bool shown = ImGui::Begin ("###tapioca.dock", nullptr, flags);
        windows.back () = ImGui::GetCurrentWindow ();
        if (shown) {
            float widest = 0.0f;
            for (const size_t i : titled)
                widest = (std::max) (widest, ImGui::CalcTextSize (panels[i]->title.c_str ()).x);
            const ImVec2 size (widest + 2.0f * kDockPadding[0] * scale,
                               ImGui::GetFontSize () + 2.0f * kDockPadding[1] * scale);
            for (const size_t i : titled) {
                PanelState& state = StateOf (keys[i], *panels[i]);
                ImGui::PushID (keys[i].c_str ());
                if (items::DockTab (*panels[i], !state.collapsed, size, scale))
                    state.collapsed = !state.collapsed;
                ImGui::PopID ();
            }
        }
        ImGui::End ();
        ImGui::PopFont ();
        ImGui::PopStyleVar (3);
        if (windows.back () != nullptr)
            inset = windows.back ()->Size.x + kDockGap * scale;
    }

    // One frame of the whole set.
    void Frame (const std::vector<const layers::Panel*>& panels, const std::vector<std::string>& keys, float scale,
                const Input& input, const std::vector<LegendBar>& legends, float delta)
    {
        ImGuiIO& io = ImGui::GetIO ();
        const bool known = input.width >= 1.0f && input.height >= 1.0f;
        const ImVec2 view = known ? ImVec2 (input.width, input.height) : ImVec2 (16384.0f, 16384.0f);
        io.DisplaySize = view;
        io.DeltaTime = delta;
        BaseStyle (scale);
        ImGui::NewFrame ();
        windows.assign (panels.size () + 1, nullptr);
        highlight = Layout::Highlight {};
        inset = 0.0f;
        if (known)
            Dock (panels, keys, scale, view);
        for (size_t i = 0; i < panels.size (); ++i)
            Window (*panels[i], keys[i], i, scale, view);
        if (known)
            LegendTips (legends, scale, view);
        // A hand over what ImGui calls an item -- a button, a section's row, a dock's tab,
        // the title bar's close button -- and while one is held. Not while the background is held: ImGui
        // makes a window's move id active there even when the window cannot move.
        const ImGuiContext& g = *ImGui::GetCurrentContext ();
        const bool held = g.ActiveId != 0 && (g.ActiveIdWindow == nullptr || g.ActiveId != g.ActiveIdWindow->MoveId);
        hand = g.HoveredId != 0 || held;
        ImGui::Render ();
        Sync (ImGui::GetDrawData ());
        ++stats.frames;
    }

    // Which of the set's windows a draw list belongs to -- the dock's is the last -- and
    // -1 for what floats over them.
    int PanelOf (const ImDrawList* list) const
    {
        for (ImGuiWindow* window : ImGui::GetCurrentContext ()->Windows) {
            if (window->DrawList != list)
                continue;
            for (size_t i = 0; i < windows.size (); ++i)
                if (windows[i] != nullptr && window->RootWindow == windows[i])
                    return int (i);
            return -1;
        }
        return -1;
    }

    // The last frame's triangles, each panel's from its top-left, against the pages as
    // they stand.
    void Collect (Layout& out, uint32_t& unsampled)
    {
        const ImDrawData* data = ImGui::GetDrawData ();
        if (data == nullptr)
            return;
        for (const ImDrawList* list : data->CmdLists) {
            const int panel = PanelOf (list);
            Built& into = panel < 0                             ? out.overlay
                          : size_t (panel) < out.panels.size () ? out.panels[size_t (panel)]
                                                                : out.dock;
            const ImVec2 origin = panel >= 0 ? windows[size_t (panel)]->Pos : ImVec2 (0.0f, 0.0f);
            for (const ImDrawCmd& command : list->CmdBuffer) {
                if (command.UserCallback != nullptr || command.ElemCount == 0)
                    continue;
                const ImTextureID id = command.GetTexID ();
                const size_t slot = id == ImTextureID_Invalid ? pages.size () : size_t (id - 1);
                if (slot >= pages.size () || pages[slot] == nullptr) {
                    ++unsampled;
                    continue;
                }
                for (unsigned int e = 0; e < command.ElemCount; ++e) {
                    const ImDrawVert& v =
                        list->VtxBuffer[int (command.VtxOffset + list->IdxBuffer[int (command.IdxOffset + e)])];
                    into.vertices.push_back (
                        { v.pos.x - origin.x, v.pos.y - origin.y, v.uv.x, v.uv.y, Unpacked (v.col), uint32_t (slot) });
                }
            }
        }
    }
};

Engine::Engine () : impl_ (new Impl ())
{
}

Engine::~Engine ()
{
    if (impl_->context == nullptr)
        return;
    std::lock_guard<std::mutex> lock (ImGuiContextMutex ());
    ImGuiContext* previous = ImGui::GetCurrentContext ();
    ImGui::SetCurrentContext (impl_->context);
    ImGui::DestroyContext (impl_->context);
    ImGui::SetCurrentContext (previous == impl_->context ? nullptr : previous);
}

bool Engine::Init (std::vector<uint8_t> fontBytes, std::string& error)
{
    if (impl_->ready)
        return true;
    if (fontBytes.empty ()) {
        error = "the overlay HUD font is empty";
        return false;
    }
    std::lock_guard<std::mutex> lock (ImGuiContextMutex ());
    ImGuiContext* previous = ImGui::GetCurrentContext ();
    impl_->context = ImGui::CreateContext ();
    if (impl_->context == nullptr) {
        error = "ImGui::CreateContext returned nothing";
        return false;
    }
    ImGui::SetCurrentContext (impl_->context);
    ImGuiIO& io = ImGui::GetIO ();
    // No files: evp.paths owns where files go, and Archicad's working directory is
    // inside Program Files.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.BackendRendererName = "tapioca-overlay-hud";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.AddMousePosEvent (-FLT_MAX, -FLT_MAX);
    io.Fonts->TexDesiredFormat = ImTextureFormat_RGBA32;
    impl_->fontBytes = std::move (fontBytes);
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    impl_->font =
        io.Fonts->AddFontFromMemoryTTF (impl_->fontBytes.data (), int (impl_->fontBytes.size ()), 16.0f, &config);
    ImGui::SetCurrentContext (previous);
    if (impl_->font == nullptr) {
        error = "ImGui could not load the overlay HUD font";
        return false;
    }
    impl_->ready = true;
    impl_->stats.fonts = 1;
    return true;
}

bool Engine::Ready () const
{
    return impl_->ready;
}

void Engine::SetFontLoader (FontLoader loader)
{
    impl_->loader = std::move (loader);
}

bool Engine::Build (const std::vector<const layers::Panel*>& panels, const std::vector<std::string>& keys, float scale,
                    const Input& input, const std::vector<LegendBar>& legends, Layout& out, std::string& error)
{
    out = Layout {};
    if (!impl_->ready) {
        error = "the overlay HUD is not ready";
        return false;
    }
    if (keys.size () != panels.size ()) {
        error = "every HUD panel needs its key";
        return false;
    }
    const auto started = std::chrono::steady_clock::now ();
    const float at = scale > 0.25f && scale < 8.0f ? scale : 1.0f;
    // The time since the last layout, for the first frame: a double click is timed as made.
    float delta = 1.0f / 60.0f;
    if (impl_->lastBuild != std::chrono::steady_clock::time_point {})
        delta =
            (std::min) ((std::max) (std::chrono::duration<float> (started - impl_->lastBuild).count (), kSettleSeconds),
                        1.0f);
    impl_->lastBuild = started;
    std::lock_guard<std::mutex> lock (ImGuiContextMutex ());
    ImGuiContext* previous = ImGui::GetCurrentContext ();
    ImGui::SetCurrentContext (impl_->context);
    uint32_t unsampled = 0;
    // Every panel's font in the atlas before the first frame, not in the middle of one.
    impl_->fontError.clear ();
    for (const layers::Panel* panel : panels)
        impl_->FontFor (panel->font);
    // The pointer and its buttons, queued once: ImGui takes a press and its release a
    // frame apart, and a press after a move a frame after the move.
    ImGuiIO& io = ImGui::GetIO ();
    io.AddMousePosEvent (input.pointer ? input.x : -FLT_MAX, input.pointer ? input.y : -FLT_MAX);
    for (const Input::Button& button : input.buttons)
        io.AddMouseButtonEvent (button.button, button.down);
    int frames = kFrames + int (input.buttons.size ()) + (input.buttons.empty () ? 0 : 1);
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        const uint64_t before = impl_->atlasVersion;
        for (int frame = 0; frame < frames; ++frame)
            impl_->Frame (panels, keys, at, input, legends, attempt == 0 && frame == 0 ? delta : kSettleSeconds);
        out = Layout {};
        out.panels.assign (panels.size (), Built {});
        unsampled = 0;
        impl_->Collect (out, unsampled);
        out.highlight = impl_->highlight;
        out.hand = impl_->hand;
        for (size_t i = 0; i < panels.size (); ++i) {
            Built& built = out.panels[i];
            if (impl_->windows[i] != nullptr) {
                built.width = impl_->windows[i]->Size.x;
                built.height = impl_->windows[i]->Size.y;
            }
            Place (*panels[i], built.width, built.height, at, built.fraction, built.offset, impl_->inset);
        }
        if (const ImGuiWindow* const dock = impl_->windows.back (); dock != nullptr) {
            out.dock.width = dock->Size.x;
            out.dock.height = dock->Size.y;
            out.dock.fraction[0] = 1.0f;
            out.dock.fraction[1] = 0.5f;
            out.dock.offset[0] = -dock->Size.x;
            out.dock.offset[1] = -dock->Size.y * 0.5f;
        }
        if (impl_->atlasVersion == before)
            break;
        frames = kFrames;
    }
    impl_->windows.clear ();
    ImGui::SetCurrentContext (previous);
    ++impl_->stats.builds;
    impl_->stats.lastMilliseconds = uint32_t (
        std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now () - started).count ());
    if (unsampled > 0) {
        error = std::to_string (unsampled) + " ImGui draw command(s) sample a texture the HUD has no page for";
        return false;
    }
    if (!impl_->fontError.empty ()) {
        error = impl_->fontError + " -- the panel is in the bundled font";
        return false;
    }
    return true;
}

bool Engine::Build (const std::vector<const layers::Panel*>& panels, float scale, std::vector<Built>& out,
                    std::string& error)
{
    std::vector<std::string> keys;
    for (size_t i = 0; i < panels.size (); ++i)
        keys.push_back ("#" + std::to_string (i));
    Layout layout;
    const bool ok = Build (panels, keys, scale, Input (), {}, layout, error);
    out = std::move (layout.panels);
    return ok;
}

const std::vector<std::shared_ptr<const overlaytext::Page>>& Engine::Pages () const
{
    return impl_->pages;
}

void Engine::UseState (std::shared_ptr<State> state)
{
    if (state != nullptr)
        impl_->store = std::move (state);
}

bool Engine::Collapsed (const std::string& key) const
{
    const auto found = impl_->store->panels.find (key);
    return found != impl_->store->panels.end () && found->second.collapsed;
}

bool Engine::SectionOpen (const std::string& key, uint32_t item, bool& open) const
{
    const auto found = impl_->store->panels.find (key);
    if (found == impl_->store->panels.end ())
        return false;
    const auto section = found->second.sections.find (item);
    if (section == found->second.sections.end ())
        return false;
    open = section->second;
    return true;
}

Stats Engine::GetStats () const
{
    return impl_->stats;
}

} // namespace overlayhud
} // namespace archviz
} // namespace geomsrv
