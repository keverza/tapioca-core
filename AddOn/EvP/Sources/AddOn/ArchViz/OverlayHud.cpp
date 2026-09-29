// ArchViz/OverlayHud -- see the header.

#include "ArchViz/OverlayHud.hpp"

#include "ArchViz/ImGuiContextLock.hpp"
#include "ArchViz/OverlayScene.hpp" // RampAt: the ramp the guest's pixel shader draws

#include <imgui.h>

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

namespace {

// A panel is laid out in this many frames: an auto-sized window measures itself in
// its first and settles in its second; the third is the one drawn.
constexpr int kFrames = 3;
// And the whole set again at most this often, while the atlas keeps growing.
constexpr int kAttempts = 3;

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

std::string Number (double value, uint32_t decimals)
{
    char buffer[64] = {};
    std::snprintf (buffer, sizeof (buffer), "%.*f", int ((std::min) (decimals, 6u)), value);
    return buffer;
}

} // namespace

void Place (const layers::Panel& panel, float width, float height, float scale, float fraction[2], float offset[2])
{
    const int column = int (panel.anchor) % 3, row = int (panel.anchor) / 3; // 0 near, 1 middle, 2 far
    fraction[0] = float (column) * 0.5f;
    fraction[1] = float (row) * 0.5f;
    const float inwardX = column == 2 ? -1.0f : 1.0f, inwardY = row == 2 ? -1.0f : 1.0f;
    offset[0] = -width * float (column) * 0.5f + inwardX * panel.offsetPixels[0] * scale;
    offset[1] = -height * float (row) * 0.5f + inwardY * panel.offsetPixels[1] * scale;
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
    // One slot per ImGui texture, its TexID the slot's index plus one.
    std::vector<ImTextureData*> textures;
    std::vector<std::shared_ptr<const overlaytext::Page>> pages;
    uint64_t atlasVersion = 0;
    Stats stats;
    bool ready = false;

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

    void Style (const layers::Panel& panel, float scale)
    {
        ImGuiStyle& style = ImGui::GetStyle ();
        style = ImGuiStyle ();
        ImGui::StyleColorsDark (&style);
        style.ScaleAllSizes (scale);
        style.WindowPadding = ImVec2 (panel.paddingPixels * scale, panel.paddingPixels * scale);
        style.WindowRounding = panel.roundingPixels * scale;
        style.WindowBorderSize = (panel.borderRgba & 0xFFu) != 0 ? (std::max) (1.0f, scale) : 0.0f;
        style.WindowMinSize = ImVec2 (1.0f, 1.0f);
        style.FrameRounding = 2.0f * scale;
        style.Colors[ImGuiCol_WindowBg] = Colour (panel.backgroundRgba);
        style.Colors[ImGuiCol_Border] = Colour (panel.borderRgba);
        style.Colors[ImGuiCol_Text] = Colour (panel.textRgba);
        style.Colors[ImGuiCol_Separator] = Colour (WithAlpha (panel.textRgba, 0.3f));
        style.Colors[ImGuiCol_FrameBg] = Colour (WithAlpha (panel.textRgba, 0.12f));
        style.Colors[ImGuiCol_TableHeaderBg] = Colour (WithAlpha (panel.textRgba, 0.12f));
        style.Colors[ImGuiCol_TableBorderLight] = Colour (WithAlpha (panel.textRgba, 0.2f));
        style.Colors[ImGuiCol_TableBorderStrong] = Colour (WithAlpha (panel.textRgba, 0.3f));
        style.Colors[ImGuiCol_TableRowBgAlt] = Colour (WithAlpha (panel.textRgba, 0.05f));
    }

    void Rows (const layers::Panel& panel, size_t begin, size_t end, float scale)
    {
        if (!ImGui::BeginTable ("##rows", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings))
            return;
        for (size_t i = begin; i < end; ++i) {
            const layers::PanelItem& item = panel.items[i];
            const bool sized = item.sizePixels > 0.0f;
            if (sized)
                ImGui::PushFont (nullptr, item.sizePixels * scale);
            ImGui::TableNextRow ();
            ImGui::TableSetColumnIndex (0);
            ImGui::PushStyleColor (ImGuiCol_Text, Colour (WithAlpha (panel.textRgba, 0.72f)));
            ImGui::TextUnformatted (item.text.c_str ());
            ImGui::PopStyleColor ();
            ImGui::TableSetColumnIndex (1);
            ImGui::PushStyleColor (ImGuiCol_Text, Colour ((item.rgba & 0xFFu) != 0 ? item.rgba : panel.textRgba));
            ImGui::TextUnformatted (item.value.c_str ());
            ImGui::PopStyleColor ();
            if (sized)
                ImGui::PopFont ();
        }
        ImGui::EndTable ();
    }

    void Ramp (const layers::Panel& panel, const layers::PanelItem& item, float width, float scale)
    {
        if (!item.text.empty ())
            ImGui::TextUnformatted (item.text.c_str ());
        const float w = item.widthPixels > 0.0f ? item.widthPixels * scale : width;
        const float h = (item.heightPixels > 0.0f ? item.heightPixels : 12.0f) * scale;
        const ImVec2 p = ImGui::GetCursorScreenPos ();
        ImDrawList* draw = ImGui::GetWindowDrawList ();
        const std::vector<layers::ColourStop>& stops = item.colormap.stops;
        if (item.colormap.bands > 0) {
            const uint32_t bands = item.colormap.bands;
            for (uint32_t k = 0; k < bands; ++k) {
                const float t = bands > 1 ? float (k) / float (bands - 1) : 0.5f;
                const ImU32 c = Packed (overlayscene::RampAt (stops, t));
                draw->AddRectFilled (ImVec2 (p.x + w * float (k) / float (bands), p.y),
                                     ImVec2 (p.x + w * float (k + 1) / float (bands), p.y + h), c);
            }
        }
        else if (!stops.empty ()) {
            draw->AddRectFilled (p, ImVec2 (p.x + w * stops.front ().at, p.y + h), Packed (stops.front ().rgba));
            for (size_t s = 1; s < stops.size (); ++s) {
                const ImU32 a = Packed (stops[s - 1].rgba), b = Packed (stops[s].rgba);
                draw->AddRectFilledMultiColor (ImVec2 (p.x + w * stops[s - 1].at, p.y),
                                               ImVec2 (p.x + w * stops[s].at, p.y + h), a, b, b, a);
            }
            draw->AddRectFilled (ImVec2 (p.x + w * stops.back ().at, p.y), ImVec2 (p.x + w, p.y + h),
                                 Packed (stops.back ().rgba));
        }
        draw->AddRect (p, ImVec2 (p.x + w, p.y + h), Packed (WithAlpha (panel.textRgba, 0.45f)));

        // The ticks under the bar, each label centred on its tick and kept inside the bar.
        const double low = item.colormap.min, high = item.colormap.max;
        std::vector<double> values = item.tickValues;
        if (values.empty ()) {
            const uint32_t ticks = (std::max) (item.ticks, 2u);
            for (uint32_t k = 0; k < ticks; ++k)
                values.push_back (low + (high - low) * double (k) / double (ticks - 1));
        }
        const float font = ImGui::GetFontSize ();
        const ImU32 textColour = Packed ((item.rgba & 0xFFu) != 0 ? item.rgba : panel.textRgba);
        for (size_t k = 0; k < values.size (); ++k) {
            const double t = high > low ? (values[k] - low) / (high - low) : 0.0;
            if (t < -1e-9 || t > 1.0 + 1e-9)
                continue;
            const float x = p.x + w * float (t);
            draw->AddLine (ImVec2 (x, p.y + h), ImVec2 (x, p.y + h + 3.0f * scale), textColour,
                           (std::max) (1.0f, scale));
            std::string label = k < item.tickLabels.size () ? item.tickLabels[k] : Number (values[k], item.decimals);
            if (!item.unit.empty () && k + 1 == values.size () && item.tickLabels.empty ())
                label += " " + item.unit;
            const ImVec2 size = ImGui::CalcTextSize (label.c_str ());
            const float left = (std::min) ((std::max) (x - size.x * 0.5f, p.x), p.x + w - size.x);
            draw->AddText (ImVec2 (left, p.y + h + 4.0f * scale), textColour, label.c_str ());
        }
        ImGui::Dummy (ImVec2 (w, h + 4.0f * scale + font));
    }

    void Items (const layers::Panel& panel, float scale)
    {
        if (!panel.title.empty ()) {
            ImGui::PushFont (nullptr, panel.sizePixels * scale * 1.2f);
            ImGui::TextUnformatted (panel.title.c_str ());
            ImGui::PopFont ();
            ImGui::Separator ();
        }
        // What an item without its own width spans: the panel's width when it has one,
        // otherwise a width that does not depend on the layout it is part of.
        const float font = ImGui::GetFontSize ();
        const float width =
            panel.widthPixels > 0.0f ? (std::max) (ImGui::GetContentRegionAvail ().x, 1.0f) : 14.0f * font;
        size_t i = 0;
        while (i < panel.items.size ()) {
            const layers::PanelItem& item = panel.items[i];
            ImGui::PushID (int (i));
            const uint32_t colour = (item.rgba & 0xFFu) != 0 ? item.rgba : panel.textRgba;
            switch (item.kind) {
                case layers::ItemKind::Row: {
                    size_t end = i;
                    while (end < panel.items.size () && panel.items[end].kind == layers::ItemKind::Row)
                        ++end;
                    Rows (panel, i, end, scale);
                    ImGui::PopID ();
                    i = end;
                    continue;
                }
                case layers::ItemKind::Text: {
                    const bool sized = item.sizePixels > 0.0f;
                    if (sized)
                        ImGui::PushFont (nullptr, item.sizePixels * scale);
                    ImGui::PushStyleColor (ImGuiCol_Text, Colour (colour));
                    if (item.wrap)
                        ImGui::PushTextWrapPos (ImGui::GetCursorPosX () + width);
                    ImGui::TextUnformatted (item.text.c_str ());
                    if (item.wrap)
                        ImGui::PopTextWrapPos ();
                    ImGui::PopStyleColor ();
                    if (sized)
                        ImGui::PopFont ();
                    break;
                }
                case layers::ItemKind::Separator:
                    ImGui::Separator ();
                    break;
                case layers::ItemKind::Spacing:
                    ImGui::Dummy (ImVec2 (1.0f, (item.heightPixels > 0.0f ? item.heightPixels : 6.0f) * scale));
                    break;
                case layers::ItemKind::Progress: {
                    const float w = item.widthPixels > 0.0f ? item.widthPixels * scale : width;
                    const float h = (item.heightPixels > 0.0f ? item.heightPixels : 14.0f) * scale;
                    ImGui::PushStyleColor (ImGuiCol_PlotHistogram,
                                           Colour ((item.rgba & 0xFFu) != 0 ? item.rgba : 0x3D8BFDFFu));
                    const float fraction = float ((std::min) ((std::max) (item.fraction, 0.0), 1.0));
                    ImGui::ProgressBar (fraction, ImVec2 (w, h), item.text.empty () ? nullptr : item.text.c_str ());
                    ImGui::PopStyleColor ();
                    break;
                }
                case layers::ItemKind::Swatch: {
                    const float s = ImGui::GetFontSize ();
                    const ImVec2 p = ImGui::GetCursorScreenPos ();
                    ImGui::GetWindowDrawList ()->AddRectFilled (p, ImVec2 (p.x + s, p.y + s), Packed (colour),
                                                                2.0f * scale);
                    ImGui::Dummy (ImVec2 (s, s));
                    ImGui::SameLine ();
                    ImGui::TextUnformatted (item.text.c_str ());
                    break;
                }
                case layers::ItemKind::Ramp:
                    Ramp (panel, item, width, scale);
                    break;
                case layers::ItemKind::Plot: {
                    std::vector<float> values;
                    values.reserve (item.values.size ());
                    for (const double value : item.values)
                        values.push_back (float (value));
                    const float w = item.widthPixels > 0.0f ? item.widthPixels * scale : width;
                    const float h = (item.heightPixels > 0.0f ? item.heightPixels : 48.0f) * scale;
                    ImGui::PushStyleColor (ImGuiCol_PlotLines,
                                           Colour ((item.rgba & 0xFFu) != 0 ? item.rgba : 0x3D8BFDFFu));
                    ImGui::PlotLines ("##plot", values.data (), int (values.size ()), 0,
                                      item.text.empty () ? nullptr : item.text.c_str (),
                                      item.autoRange ? FLT_MAX : float (item.min),
                                      item.autoRange ? FLT_MAX : float (item.max), ImVec2 (w, h));
                    ImGui::PopStyleColor ();
                    break;
                }
                case layers::ItemKind::Table: {
                    size_t columns = item.columns.size ();
                    for (const std::vector<std::string>& row : item.rows)
                        columns = (std::max) (columns, row.size ());
                    if (columns == 0 ||
                        !ImGui::BeginTable ("##table", int (columns),
                                            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings |
                                                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
                        break;
                    if (!item.columns.empty ()) {
                        for (size_t c = 0; c < columns; ++c)
                            ImGui::TableSetupColumn (c < item.columns.size () ? item.columns[c].c_str () : "");
                        ImGui::TableHeadersRow ();
                    }
                    for (const std::vector<std::string>& row : item.rows) {
                        ImGui::TableNextRow ();
                        for (size_t c = 0; c < row.size (); ++c) {
                            ImGui::TableSetColumnIndex (int (c));
                            ImGui::TextUnformatted (row[c].c_str ());
                        }
                    }
                    ImGui::EndTable ();
                    break;
                }
            }
            ImGui::PopID ();
            ++i;
        }
    }

    // One frame of one panel; its size when the frame ended.
    ImVec2 Frame (const layers::Panel& panel, float scale, size_t index)
    {
        ImGuiIO& io = ImGui::GetIO ();
        io.DisplaySize = ImVec2 (16384.0f, 16384.0f);
        io.DeltaTime = 1.0f / 60.0f;
        Style (panel, scale);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos (ImVec2 (0.0f, 0.0f));
        if (panel.widthPixels > 0.0f) {
            const float w = panel.widthPixels * scale;
            ImGui::SetNextWindowSizeConstraints (ImVec2 (w, 0.0f), ImVec2 (w, FLT_MAX));
        }
        char name[48] = {};
        std::snprintf (name, sizeof (name), "##tapioca.panel.%zu", index);
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav;
        ImGui::Begin (name, nullptr, flags);
        ImGui::PushFont (FontFor (panel.font), panel.sizePixels * scale);
        Items (panel, scale);
        ImGui::PopFont ();
        const ImVec2 size = ImGui::GetWindowSize ();
        ImGui::End ();
        ImGui::Render ();
        Sync (ImGui::GetDrawData ());
        ++stats.frames;
        return size;
    }

    // The last frame's triangles, against the pages as they stand.
    void Collect (Built& out, uint32_t& unsampled)
    {
        const ImDrawData* data = ImGui::GetDrawData ();
        if (data == nullptr)
            return;
        for (const ImDrawList* list : data->CmdLists) {
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
                    out.vertices.push_back ({ v.pos.x, v.pos.y, v.uv.x, v.uv.y, Unpacked (v.col), uint32_t (slot) });
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

bool Engine::Build (const std::vector<const layers::Panel*>& panels, float scale, std::vector<Built>& out,
                    std::string& error)
{
    out.clear ();
    if (!impl_->ready) {
        error = "the overlay HUD is not ready";
        return false;
    }
    const auto started = std::chrono::steady_clock::now ();
    const float at = scale > 0.25f && scale < 8.0f ? scale : 1.0f;
    std::lock_guard<std::mutex> lock (ImGuiContextMutex ());
    ImGuiContext* previous = ImGui::GetCurrentContext ();
    ImGui::SetCurrentContext (impl_->context);
    uint32_t unsampled = 0;
    // Every panel's font in the atlas before the first frame, not in the middle of one.
    impl_->fontError.clear ();
    for (const layers::Panel* panel : panels)
        impl_->FontFor (panel->font);
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        const uint64_t before = impl_->atlasVersion;
        out.assign (panels.size (), Built {});
        unsampled = 0;
        for (size_t i = 0; i < panels.size (); ++i) {
            ImVec2 size;
            for (int frame = 0; frame < kFrames; ++frame)
                size = impl_->Frame (*panels[i], at, i);
            out[i].width = size.x;
            out[i].height = size.y;
            impl_->Collect (out[i], unsampled);
        }
        if (impl_->atlasVersion == before)
            break;
    }
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

const std::vector<std::shared_ptr<const overlaytext::Page>>& Engine::Pages () const
{
    return impl_->pages;
}

Stats Engine::GetStats () const
{
    return impl_->stats;
}

} // namespace overlayhud
} // namespace archviz
} // namespace geomsrv
