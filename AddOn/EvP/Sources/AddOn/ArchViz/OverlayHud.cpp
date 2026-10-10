// ArchViz/OverlayHud -- see the header.

#include "ArchViz/OverlayHud.hpp"

#include "ArchViz/HudClip.hpp"
#include "ArchViz/ImGuiContextLock.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/OverlayHudItems.hpp"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>

namespace geomsrv {
namespace archviz {
namespace overlayhud {

using items::Unpacked;

namespace {

// A set is laid out in this many frames: an auto-sized window measures itself in its
// first and settles in its second; the third is the one drawn. A press and a release
// take a frame each on top (ImGui spreads input events over frames).
constexpr int kFrames = 3;
// And the whole set again at most this often, while the atlas keeps growing.
constexpr int kAttempts = 3;
// The frames after the first advance ImGui's clock by almost nothing: the first carries
// the time since the last layout, so a double click is timed as the user made it.
constexpr float kSettleSeconds = 1.0e-4f;

// ⚠️ PIXEL-SNAPPED, NOT OVERSAMPLED (the user, 2026-09-30: the light panel's text slightly
// blurry). ImGui's default rasterises glyphs twice as wide for sub-pixel placement and lets
// their advances fall between pixels; the HUD's text never slides, so whole-pixel glyphs
// rasterised at their size are the crisp ones.
ImFontConfig CrispFont ()
{
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    config.PixelSnapH = true;
    config.OversampleH = 1;
    config.OversampleV = 1;
    return config;
}

// ⚠️ WHERE IMGUI PUT A WINDOW, NOT WHERE IT WOULD BE RECOMPUTED: a size of half a pixel, or the
// middle of an odd view, put the text between pixels. Measured from the anchor rounded as the
// shader rounds it (GuestShaderSources.hpp), so the triangles land where ImGui laid them.
void Settle (Built& built, ImVec2 pos, const Input& input)
{
    built.offset[0] = pos.x - std::floor (built.fraction[0] * input.width + 0.5f);
    built.offset[1] = pos.y - std::floor (built.fraction[1] * input.height + 0.5f);
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

std::shared_ptr<State> NewState ()
{
    return std::make_shared<State> ();
}

void ClearState (State& state)
{
    // The revision goes on: a renderer following it sees the reset as a change. The console's
    // marks too: its entries are the process's, not the project's (HudConsole.hpp), and what
    // was shown or cleared before stays so.
    const uint64_t revision = state.revision + 1;
    const uint64_t consoleSeen = state.consoleSeen, consoleCleared = state.consoleCleared;
    state = State {};
    state.revision = revision;
    state.consoleSeen = consoleSeen;
    state.consoleCleared = consoleCleared;
}

float FontScaleOf (const State& state)
{
    return hudshell::FontScaleOfStep (state.fontStep);
}

void SetFontScale (State& state, float scale)
{
    uint32_t nearest = 0;
    for (uint32_t k = 1; k < kFontStepCount; ++k)
        if (std::fabs (kFontSteps[k] - scale) < std::fabs (kFontSteps[nearest] - scale))
            nearest = k;
    state.fontStep = nearest;
}

bool HudOpen (const State& state)
{
    return state.host.known && state.host.open;
}

std::string SelectedKey (const State& state)
{
    return state.host.selected;
}

void SetHudOpen (State& state, bool open)
{
    state.host.known = true;
    state.host.open = open;
}

void SelectKey (State& state, const std::string& key)
{
    state.host.selected = key;
}

bool ContentShown (const State& state)
{
    return state.shown;
}

bool LayerShown (const State& state, const std::string& layer)
{
    return state.hidden.count (layer) == 0;
}

std::vector<std::string> HiddenLayers (const State& state)
{
    return std::vector<std::string> (state.hidden.begin (), state.hidden.end ());
}

void SetContentShown (State& state, bool shown)
{
    if (state.shown != shown) {
        state.shown = shown;
        ++state.revision;
    }
}

void SetLayerShown (State& state, const std::string& layer, bool shown)
{
    const bool changed = shown ? state.hidden.erase (layer) > 0 : state.hidden.insert (layer).second;
    if (changed)
        ++state.revision;
}

uint64_t Revision (const State& state)
{
    return state.revision;
}

bool TakeViewerRequest (State& state)
{
    const bool requested = state.viewerRequested;
    state.viewerRequested = false;
    return requested;
}
bool TakeLogsRequest (State& state)
{
    return std::exchange (state.logsRequested, false);
}

std::string MassingStatsFunction (const State& state)
{
    return state.massingStatsFunction;
}

bool MassingCollapseZone (const State& state)
{
    return state.massingCollapseZone;
}
bool UniqueBuildings (const State& state)
{
    return state.uniqueBuildings;
}
bool MarkLargeFloors (const State& state)
{
    return state.markLargeFloors;
}
bool ShowLowHeadroom (const State& state)
{
    return state.showLowHeadroom;
}
std::vector<massingbake::Kind> TakeMassingBakes (State& state)
{
    auto requests = std::move (state.massingBakes);
    state.massingBakes.clear ();
    return requests;
}
std::string HighlightedBuilding (const State& state)
{
    return state.highlightedBuilding;
}
std::string BuildingFloorKey (const State& state)
{
    return state.hoveredFloors.Empty () ? state.pickedFloorBuilding : state.hoveredFloorBuilding;
}
hudsection::Run BuildingFloors (const State& state)
{
    return state.hoveredFloors.Empty () ? state.floors : state.hoveredFloors;
}

std::vector<hudmeta::Edit> TakeMetadataEdits (State& state)
{
    std::vector<hudmeta::Edit> edits;
    edits.swap (state.metadataEdits);
    return edits;
}

std::vector<hudmassing::Request> TakeMassingRequests (State& state)
{
    std::vector<hudmassing::Request> requests;
    requests.swap (state.massingRequests);
    return requests;
}

std::vector<massingrules::Edit> TakeMassingRuleEdits (State& state)
{
    std::vector<massingrules::Edit> edits;
    edits.swap (state.massingRuleEdits);
    return edits;
}

std::vector<massingcalculation::Request> TakeMassingCalculations (State& state)
{
    std::vector<massingcalculation::Request> requests;
    requests.swap (state.massingCalculations);
    return requests;
}

hudsection::Run PickedFloors (const State& state)
{
    return state.floors;
}

std::vector<hudmassingrules::NumberEdit> TakeMassingNumbers (State& state)
{
    std::vector<hudmassingrules::NumberEdit> edits;
    edits.swap (state.massingRules.numbers);
    return edits;
}

bool AnswerMassingNumber (State& state, const hudmassingrules::NumberEdit& edit, double number)
{
    if (!hudmassingrules::AnswerNumber (state.massingRules, edit, number))
        return false;
    // Modal answers queue a complete preview now, without waiting for a slider redraw.
    std::vector<massingrules::Page> pages;
    for (const auto& parcel : state.massingSite.parcels)
        pages.push_back (parcel.second.source);
    if (!pages.empty ()) {
        auto request = hudmassingrules::SiteInputs (pages, state.massingRules, state.massingSite);
        state.massingSite.lastRequested = request;
        state.massingCalculations.push_back (std::move (request));
    }
    else {
        auto request = edit.before;
        request.baseHeight = state.massingRules.calculation.baseHeight;
        request.runPerRise = state.massingRules.calculation.runPerRise;
        request.capZ = state.massingRules.calculation.capZ;
        request.baseDepth = state.massingRules.calculation.baseDepth;
        request.assignments = state.massingRules.assignments;
        state.massingRules.lastRequested = request;
        state.massingCalculations.push_back (std::move (request));
    }
    return true;
}

void SetPickedFloors (State& state, const hudsection::Run& run)
{
    state.floors = run;
}

bool HoverMode (const State& state)
{
    return state.hover;
}

void SetHoverMode (State& state, bool on)
{
    if (state.hover != on) {
        state.hover = on;
        ++state.revision;
    }
}

std::vector<std::pair<std::string, double>> Values (const State& state, const std::string& key)
{
    std::vector<std::pair<std::string, double>> out;
    const auto found = state.panels.find (key);
    if (found != state.panels.end ())
        for (const auto& held : found->second.values)
            out.emplace_back (held.first, held.second.current);
    return out;
}

Engine::Impl::PanelState& Engine::Impl::StateOf (const std::string& key, const layers::Panel&)
{
    return store->panels[key];
}

// Between frames, the context current and locked: ImGui adds a font to the atlas
// only then.
ImFont* Engine::Impl::FontFor (const std::string& path)
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
        ImFontConfig config = CrispFont ();
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
void Engine::Impl::Sync (ImDrawData* data)
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

void Engine::Impl::Items (const layers::Panel& panel, PanelState& state, float scale)
{
    // What an item without its own width spans: the panel's width when it has one,
    // otherwise a width that does not depend on the layout it is part of.
    const float font = ImGui::GetFontSize ();
    const float width = panel.widthPixels > 0.0f ? (std::max) (ImGui::GetContentRegionAvail ().x, 1.0f) : 14.0f * font;
    // Inside a closed section its items are not laid out at all, nor on a tab not shown.
    bool shown = true;
    // ⚠️ ONE TAB BAR PER PANEL, WHERE ITS FIRST TAB IS: every tab item is a tab of it, and
    // the items after a tab, up to the next, are its page.
    size_t firstTab = panel.items.size ();
    const std::string bar = TabBarId (panel, &firstTab);
    bool inBar = false, page = true, pageOpen = false;
    uint32_t tabNumber = 0;
    tabNow = -1;
    size_t i = 0;
    while (i < panel.items.size ()) {
        const layers::PanelItem& item = panel.items[i];
        if (item.kind == layers::ItemKind::Tab) {
            if (i == firstTab)
                inBar = ImGui::BeginTabBar ("##tabs");
            if (pageOpen)
                ImGui::EndTabItem ();
            pageOpen = inBar && TabItem (item, tabNumber++, bar, state);
            page = pageOpen;
            shown = true;
            ++i;
            continue;
        }
        if (!page || (!shown && item.kind != layers::ItemKind::Section)) {
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
                const bool was = open;
                items::Section (panel, item, open, scale);
                if (open != was)
                    changes.push_back ({ "section", key, panel.title, item.text, int32_t (i), open ? 1.0 : 0.0,
                                         open ? "open" : "folded", true });
                shown = open;
                break;
            }
            case layers::ItemKind::Checkbox:
            case layers::ItemKind::Slider:
            case layers::ItemKind::Combo:
            case layers::ItemKind::Button:
                Control (panel, item, i, state, width, scale);
                break;
            case layers::ItemKind::SitePlan:
                SitePlan (panel, item, i, state, width, scale);
                break;
            case layers::ItemKind::Tab:
                break; // above
        }
        ImGui::PopID ();
        ++i;
    }
    if (pageOpen)
        ImGui::EndTabItem ();
    if (inBar) {
        ImGui::EndTabBar ();
        TabsDone (panel, firstTab, bar, state);
    }
}

// A panel without a title, at its anchor on the view: its items. A titled one is a tab
// of the host (OverlayHudHost.cpp); a hidden layer's is not drawn.
// `scale` is the view's DPI scale, what the distances from its edges take; `ui` that
// times the text size, what everything else takes.
void Engine::Impl::Window (const layers::Panel& panel, const std::string& key, size_t index, float scale, float ui,
                           ImVec2 view)
{
    // A titled panel is a tab of the host (OverlayHudHost.cpp), not a window of its own, and
    // one that asked to be a Stats card is that wherever the HUD has a Stats page; a hidden
    // layer's panel is not drawn.
    if (!panel.title.empty () || (own.standalone && panel.tab == hudshell::kStatsTab) ||
        !LayerShown (*store, key.substr (0, key.rfind ('#'))))
        return;
    PanelState& state = StateOf (key, panel);
    this->key = key;
    layer = key.substr (0, key.rfind ('#'));
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
        const float w = panel.widthPixels * ui;
        ImGui::SetNextWindowSizeConstraints (ImVec2 (w, 0.0f), ImVec2 (w, FLT_MAX));
    }
    // ### keeps the window's identity -- and so its state.
    const std::string name = "###tapioca.panel." + key;
    const int colours = hudshell::PushLook (panel, ui);
    ImGui::PushFont (FontFor (panel.font), panel.sizePixels * ui);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoCollapse;
    if (ImGui::Begin (name.c_str (), nullptr, flags))
        Items (panel, state, ui);
    windows[index] = ImGui::GetCurrentWindow ();
    ImGui::PopFont ();
    ImGui::End ();
    ImGui::PopStyleColor (colours);
    ImGui::PopStyleVar (hudshell::kLookVars);
}

// A legend's bar pointed at: the value there, beside the bar at the pointer.
void Engine::Impl::LegendTips (const std::vector<LegendBar>& legends, float scale, ImVec2 view)
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
            items::ValueTip (legend.colormap, (mouse.x - a.x) / (b.x - a.x), legend.colormap.min, legend.colormap.max,
                             legend.decimals, legend.unit, ImVec2 (mouse.x, a.y - 4.0f * scale),
                             hudshell::TipSide::Above, band);
        }
        else {
            // Towards the middle of the view, away from the edge the legend sits at.
            const bool left = a.x > view.x * 0.5f;
            items::ValueTip (legend.colormap, (b.y - mouse.y) / (b.y - a.y), legend.colormap.min, legend.colormap.max,
                             legend.decimals, legend.unit,
                             ImVec2 (left ? a.x - 6.0f * scale : b.x + 6.0f * scale, mouse.y),
                             left ? hudshell::TipSide::Left : hudshell::TipSide::Right, band);
        }
        highlight = { true, bar.layer, band[0], band[1] };
        return;
    }
}

// ⚠️ HOVER MODE (the user's stage 3): whether the item under the pointer is tinted; what it
// says is read out in the host (Readout). Not while the pointer is on the HUD -- that is the
// HUD's -- nor on a legend, whose own tip says its value. ⚠️ NOTHING IS DRAWN HERE: the
// tint is the item's own triangles in model metres, which the stream hands the view's
// renderer (`Layout::hoverTint`) -- it follows the view as it moves, which pixels laid out
// at the last pointer move did not.
void Engine::Impl::HoverTint (const Hover& hover)
{
    tinted = hover.active && !hover.tintModel.empty () && !highlight.active &&
             ImGui::GetCurrentContext ()->HoveredWindow == nullptr;
}

// One frame of the whole set.
void Engine::Impl::Frame (const std::vector<const layers::Panel*>& panels, const std::vector<std::string>& keys,
                          float scale, const Input& input, const std::vector<LegendBar>& legends, float delta)
{
    ImGuiIO& io = ImGui::GetIO ();
    const bool known = input.width >= 1.0f && input.height >= 1.0f;
    const ImVec2 view = known ? ImVec2 (input.width, input.height) : ImVec2 (16384.0f, 16384.0f);
    io.DisplaySize = view;
    io.DeltaTime = delta;
    const float ui = scale * hudshell::FontScaleOfStep (store->fontStep);
    hudshell::BaseStyle (ui);
    ImGui::NewFrame ();
    windows.assign (panels.size () + 2, nullptr);
    highlight = Layout::Highlight {};
    inset = 0.0f;
    Gather (panels, keys);
    // ⚠️ WHAT THE HOST READS OUT IS HELD WHILE THE POINTER IS ON THE HUD OR A LEGEND: gone to
    // the panel to read it, the pointer crosses what is under the panel; a legend says its own.
    bool onLegend = false;
    for (const LegendBar& bar : legends)
        onLegend = onLegend || (input.pointer && input.x >= bar.rect[0] && input.x <= bar.rect[2] &&
                                input.y >= bar.rect[1] && input.y <= bar.rect[3]);
    if (!store->hover)
        readout = Hover {};
    else if (ImGui::GetCurrentContext ()->HoveredWindow == nullptr && !onLegend)
        readout = input.hover;
    Dock (panels, ui, view);
    // Hidden by the dock's circle: the dock alone.
    if (store->shown) {
        Host (panels, keys, scale, ui, view);
        for (size_t i = 0; i < panels.size (); ++i)
            Window (*panels[i], keys[i], i, scale, ui, view);
    }
    // The dock over everything: a panel dragged onto it never hides it.
    if (ImGuiWindow* const dock = windows[panels.size ()]; dock != nullptr)
        ImGui::BringWindowToDisplayFront (dock);
    if (known && store->shown)
        LegendTips (legends, ui, view);
    tinted = false;
    if (known && store->shown && store->hover)
        HoverTint (input.hover);
    Menu (ui);
    // A dropdown's list is a popup: while one is open, the whole view is the HUD's.
    popup = ImGui::IsPopupOpen ("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    // A hand over what ImGui calls an item -- a button, a section's row, a tab, the dock's
    // tab -- and while one is held. Not while the background is held: ImGui makes a
    // window's move id active there, a drag moving the host.
    const ImGuiContext& g = *ImGui::GetCurrentContext ();
    const bool held = g.ActiveId != 0 && (g.ActiveIdWindow == nullptr || g.ActiveId != g.ActiveIdWindow->MoveId);
    hand = g.HoveredId != 0 || held;
    ImGui::Render ();
    Sync (ImGui::GetDrawData ());
    ++stats.frames;
}

// Which of the set's windows a draw list belongs to -- the panels', then the dock's and the
// host's -- and -1 for what floats over them.
int Engine::Impl::PanelOf (const ImDrawList* list) const
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
void Engine::Impl::Collect (Layout& out, uint32_t& unsampled)
{
    const ImDrawData* data = ImGui::GetDrawData ();
    if (data == nullptr)
        return;
    std::vector<hudclip::Corner> cut;
    for (const ImDrawList* list : data->CmdLists) {
        const int panel = PanelOf (list);
        Built& into = panel < 0                              ? out.overlay
                      : size_t (panel) < out.panels.size ()  ? out.panels[size_t (panel)]
                      : size_t (panel) == out.panels.size () ? out.dock
                                                             : out.host;
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
            // ⚠️ CUT TO THE COMMAND'S RECTANGLE, as the scissor a renderer of ImGui's would set
            // (HudClip.hpp): a scrolled page's rows past its edges are not drawn.
            const hudclip::Rect clip { command.ClipRect.x, command.ClipRect.y, command.ClipRect.z, command.ClipRect.w };
            const auto corner = [&] (unsigned int e) {
                const ImDrawVert& v =
                    list->VtxBuffer[int (command.VtxOffset + list->IdxBuffer[int (command.IdxOffset + e)])];
                return hudclip::Corner { v.pos.x, v.pos.y, v.uv.x, v.uv.y, v.col };
            };
            for (unsigned int e = 0; e + 2 < command.ElemCount; e += 3) {
                cut.clear ();
                hudclip::Clip (corner (e), corner (e + 1), corner (e + 2), clip, cut);
                for (const hudclip::Corner& c : cut)
                    into.vertices.push_back (
                        { c.x - origin.x, c.y - origin.y, c.u, c.v, Unpacked (c.col), uint32_t (slot) });
            }
        }
    }
}

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
    ImFontConfig config = CrispFont ();
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
    impl_->changes.clear ();
    impl_->nextStatsHover.clear ();
    impl_->nextFloorBuilding.clear ();
    impl_->nextFloorHover = {};
    std::unique_lock<std::mutex> lock (ImGuiContextMutex ());
    ImGuiContext* previous = ImGui::GetCurrentContext ();
    ImGui::SetCurrentContext (impl_->context);
    uint32_t unsampled = 0;
    // Each control's value as its layer says it now.
    impl_->Reconcile (panels, keys);
    // Every panel's font in the atlas before the first frame, not in the middle of one.
    impl_->fontError.clear ();
    for (const layers::Panel* panel : panels)
        impl_->FontFor (panel->font);
    // The pointer and its buttons, queued once: ImGui takes a press and its release a
    // frame apart, and a press after a move a frame after the move.
    ImGuiIO& io = ImGui::GetIO ();
    io.AddMousePosEvent (input.pointer ? input.x : -FLT_MAX, input.pointer ? input.y : -FLT_MAX);
    io.AddKeyEvent (ImGuiMod_Shift, input.shift);
    for (const Input::Button& button : input.buttons)
        io.AddMouseButtonEvent (button.button, button.down);
    // The wheel the input layer took over a page that scrolls: ImGui scrolls the page under the
    // pointer by it.
    if (input.wheel != 0.0f)
        io.AddMouseWheelEvent (0.0f, input.wheel);
    int frames = kFrames + int (input.buttons.size ()) + (input.buttons.empty () ? 0 : 1);
    const bool known = input.width >= 1.0f && input.height >= 1.0f;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        const uint64_t before = impl_->atlasVersion;
        for (int frame = 0; frame < frames; ++frame) {
            impl_->nextFloorBuilding.clear ();
            impl_->nextFloorHover = {};
            impl_->Frame (panels, keys, at, input, legends, attempt == 0 && frame == 0 ? delta : kSettleSeconds);
        }
        out = Layout {};
        out.panels.assign (panels.size (), Built {});
        unsampled = 0;
        impl_->Collect (out, unsampled);
        out.highlight = impl_->highlight;
        out.hand = impl_->hand;
        out.popup = impl_->popup;
        out.hoverTint = impl_->tinted;
        for (size_t i = 0; i < panels.size (); ++i) {
            Built& built = out.panels[i];
            if (impl_->windows[i] != nullptr) {
                built.width = impl_->windows[i]->Size.x;
                built.height = impl_->windows[i]->Size.y;
            }
            Place (*panels[i], built.width, built.height, at, built.fraction, built.offset, impl_->inset);
            if (known && impl_->windows[i] != nullptr)
                Settle (built, impl_->windows[i]->Pos, input);
        }
        if (const ImGuiWindow* const dock = impl_->windows[panels.size ()]; dock != nullptr) {
            out.dock.width = dock->Size.x;
            out.dock.height = dock->Size.y;
            out.dock.fraction[0] = 1.0f;
            out.dock.fraction[1] = 0.5f;
            out.dock.offset[0] = -dock->Size.x;
            out.dock.offset[1] = -std::floor (dock->Size.y * 0.5f);
            if (known)
                Settle (out.dock, dock->Pos, input);
        }
        // The host, anchored at the corner it was dragged nearest -- or where the tab it
        // shows asks to be -- so a view resized before the next layout keeps it there.
        if (const ImGuiWindow* const host = impl_->windows.back (); host != nullptr && impl_->present) {
            Built& built = out.host;
            built.width = host->Size.x;
            built.height = host->Size.y;
            built.scrolls = impl_->hostScrolls;
            const State::Host& state = impl_->store->host;
            if (state.placement.placed) {
                built.fraction[0] = (state.placement.corner & 1u) != 0 ? 1.0f : 0.0f;
                built.fraction[1] = (state.placement.corner & 2u) != 0 ? 1.0f : 0.0f;
            }
            else {
                Place (*impl_->look, built.width, built.height, at, built.fraction, built.offset, impl_->inset);
            }
            if (known)
                Settle (built, host->Pos, input);
            out.hostKey = state.selected;
        }
        if (impl_->atlasVersion == before)
            break;
        frames = kFrames;
    }
    impl_->windows.clear ();
    ImGui::SetCurrentContext (previous);
    lock.unlock ();
    // ⚠️ SAID OUTSIDE IMGUI'S LOCK: the sink takes the event ring's.
    if (impl_->statsHover != impl_->nextStatsHover ||
        (!impl_->nextStatsHover.empty () && impl_->store->massingStatsFunction.empty ())) {
        impl_->statsHover = impl_->nextStatsHover;
        impl_->store->massingStatsFunction = impl_->statsHover;
        impl_->changes.push_back ({ "massingStatsHover", {}, "Stats", impl_->statsHover, -1, 0, {}, true });
    }
    if (impl_->store->hoveredFloors != impl_->nextFloorHover ||
        impl_->store->hoveredFloorBuilding != impl_->nextFloorBuilding) {
        impl_->store->hoveredFloors = impl_->nextFloorHover;
        impl_->store->hoveredFloorBuilding = impl_->nextFloorBuilding;
        impl_->changes.push_back ({ "buildingFloorHover", {}, "Selection", impl_->nextFloorBuilding, -1, 0, {}, true });
    }
    out.changes = impl_->changes;
    if (impl_->sink)
        for (const Change& change : out.changes)
            impl_->sink (change);
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
    // Each alone, as it would stand: a titled one as the host showing it.
    out.clear ();
    bool ok = true;
    for (size_t i = 0; i < panels.size (); ++i) {
        Layout layout;
        ok = Build ({ panels[i] }, { keys[i] }, scale, Input (), {}, layout, error) && ok;
        out.push_back (panels[i]->title.empty () ? std::move (layout.panels[0]) : std::move (layout.host));
    }
    return ok;
}

const std::vector<std::shared_ptr<const overlaytext::Page>>& Engine::Pages () const
{
    return impl_->pages;
}

float Engine::FontScale () const
{
    return FontScaleOf (*impl_->store);
}

void Engine::SetFontScale (float scale)
{
    overlayhud::SetFontScale (*impl_->store, scale);
}

void Engine::SetChangeSink (ChangeSink sink)
{
    impl_->sink = std::move (sink);
}

void Engine::SetLayers (std::vector<std::string> names)
{
    impl_->layerNames = std::move (names);
}

void Engine::UseState (std::shared_ptr<State> state)
{
    if (state != nullptr)
        impl_->store = std::move (state);
}

bool Engine::Open () const
{
    return HudOpen (*impl_->store);
}

std::string Engine::Selected () const
{
    return SelectedKey (*impl_->store);
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
