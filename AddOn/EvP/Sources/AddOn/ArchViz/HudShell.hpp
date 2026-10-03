#ifndef EVP_ARCHVIZ_HUDSHELL_HPP
#define EVP_ARCHVIZ_HUDSHELL_HPP

// ArchViz/HudShell -- what every Tapioca HUD looks like and how its frame behaves, in
// Dear ImGui: the look (the overlays' dense card, light or dark), the text size's steps,
// the dock's turned tab at the view's right edge, and the one floating panel whose tab row
// is its head. The overlays' HUD (OverlayHud.hpp) and the viewer's (DiligentHud.hpp) both
// draw through it, so the two are one design rather than two that drift.
//
// ⚠️ THE OVERLAYS' HUD IS THE TEMPLATE (the user, 2026-10-03). Everything here was that
// HUD's first and moved out unchanged; a change of look or behaviour is made here, once,
// and both surfaces take it.
//
// ⚠️ PURE IMGUI, ANY THREAD THAT HOLDS A CONTEXT. The overlays lay out on the main thread
// into triangles; the viewer draws on its render thread. Nothing here touches ACAPI, DG,
// D3D or a context of its own: the caller has made its context current under
// ImGuiContextLock and begun a frame. tests/cpp builds it with the vendored imgui.

#include "ArchViz/OverlayLayers.hpp"

#include <imgui.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct ImGuiWindow;

namespace geomsrv {
namespace archviz {
namespace hudshell {

// ---- colour -----------------------------------------------------------------------

// 0xRRGGBBAA as ImGui wants it, and back.
ImVec4 Colour (uint32_t rgba);
ImU32 Packed (uint32_t rgba);
uint32_t Unpacked (ImU32 col);
// `rgba` with its alpha multiplied by `factor`.
uint32_t WithAlpha (uint32_t rgba, float factor);
// Text that reads on `rgba`: dark on a light colour, white on a dark one.
uint32_t Contrast (uint32_t rgba);

// ---- the look -----------------------------------------------------------------------

// The style every window starts from at `scale` (the view's DPI times the text size);
// each pushes its own colours and spacing over it.
void BaseStyle (float scale);

// A card's own colours, rounding and padding over the base: what the pointer can press is
// tinted with its accent. Returns the number of colours pushed; `kLookVars` style vars
// are pushed too.
int PushLook (const overlaylayers::Panel& look, float scale);
constexpr int kLookVars = 3;

// The light card, small: the look of a HUD that shows no caller's panel.
const overlaylayers::Panel& PlainLook ();

// ⚠️ THE TEXT SIZE IS A FEW STEPS, NOT A NUMBER (the user, 2026-09-29: a control for the
// HUD's font size). Settings and the HUD's menu choose one; every size of the HUD -- text,
// padding, widths, the dock itself -- follows, the distances from the view's edges do not.
constexpr float kFontSteps[] = { 0.8f, 0.9f, 1.0f, 1.1f, 1.25f, 1.4f, 1.6f, 1.8f, 2.0f };
constexpr uint32_t kFontStepCount = uint32_t (sizeof (kFontSteps) / sizeof (kFontSteps[0]));
constexpr uint32_t kFontStepDefault = 2;
// A step's factor, the last for one past the end; and as the user reads it, "110 %".
float FontScaleOfStep (uint32_t step);
std::string Percent (float scale);

// ---- the dock -----------------------------------------------------------------------

// The dock's tab: its font, its padding across and along its turned title, and the gap
// between it and a panel on the view's right column.
constexpr float kDockFontPixels = 12.0f;
constexpr float kDockPadding[2] = { 5.0f, 12.0f };
constexpr float kDockGap = 6.0f;

// The dock's tab: a circle at its top -- filled while `shown` -- then `label` turned a
// quarter clockwise -- it reads top to bottom, as a tab on the right edge does -- `padding`
// round it across and along, rounded on the left where it comes out of the view's edge.
// Filled with `look`'s accent while `open`, in its card's colours otherwise; each part
// tinted when pointed at and pressed. True when the label is pressed; `toggled` when the
// circle is.
bool DockTab (const char* id, const std::string& label, const overlaylayers::Panel& look, bool open, bool shown,
              ImVec2 padding, float scale, bool& toggled);

// ---- the floating panel ---------------------------------------------------------------

// Where the user left the panel: `offset` logical pixels in from the edges of the view's
// `corner` nearest it (1 right, 2 bottom), so a resized view keeps it that far from them.
struct Placement {
    bool placed = false;
    uint8_t corner = 0;
    float offset[2] = { 0.0f, 0.0f };
};

// One tab of the panel: its key -- the window-wide identity the held tab is named by --
// and the title on it.
struct HostTab {
    std::string key;
    std::string title;
};

struct HostSpec {
    const char* window = "###tapioca.hud"; // one name in every context, whatever tab it shows
    const overlaylayers::Panel* look = nullptr;
    ImFont* font = nullptr;
    float scale = 1.0f; // the view's DPI scale: the distances from its edges
    float ui = 1.0f;    // that times the text size: everything else
    ImVec2 view = ImVec2 (0.0f, 0.0f);
    float inset = 0.0f; // how far the view's right column moves in: the dock's width and a gap
    std::vector<HostTab> tabs;
};

struct HostResult {
    bool drawn = false;
    bool closed = false; // the close button at the tab row's end
    std::string pressed; // the tab the user pressed in this frame; empty for none
    std::string shown;   // the tab whose page was drawn
    bool moved = false;  // dragged in this frame: `placement` says where to
    ImGuiWindow* window = nullptr;
};

// ⚠️ ONE FLOATING PANEL, ITS TABS ITS HEAD (the user, 2026-09-30: the STUDY panel's design as
// the main one; floating, for the user to place anywhere in the view; a small, dense
// inspection panel). No title bar: the tab row is its head, the close button at its end. It
// moves where it is dragged by anything that is not a control.
//
// `held` is the tab the caller's state holds; `shownLast` the one this context showed in its
// last frame, kept by the caller per context. ⚠️ THE HELD TAB IS ASKED FOR ONLY WHERE THIS
// CONTEXT SHOWED ANOTHER: asked every frame, ImGui applies it over the user's click. A press
// on another tab is `pressed` -- the caller decides whether to hold it. `page` draws a tab's
// page; `footer`, when given, draws under every page.
HostResult Host (const HostSpec& spec, const std::string& held, std::string& shownLast, Placement& placement,
                 const std::function<void (const std::string& key)>& page, const std::function<void ()>& footer);

// ---- the HUD's own tabs ----------------------------------------------------------------
// ⚠️ EVERY HUD HAS THEM, WITH OR WITHOUT A CALLER'S PANEL (the user, 2026-10-03): Stats -- the
// dashboard, Selection -- the selected elements and their Tapioca metadata, Settings -- the
// display, Debug -- what the surface costs. A caller's titled panels sit between Selection and
// Settings; one that asks for Stats (`Panel::tab`) is a card on the Stats page instead.

constexpr char kStatsKey[] = "tapioca.stats";
constexpr char kSelectionKey[] = "tapioca.selection";
constexpr char kSettingsKey[] = "tapioca.settings";
constexpr char kDebugKey[] = "tapioca.debug";
// What a caller's panel names to be a card on the Stats page rather than a tab of its own.
constexpr char kStatsTab[] = "stats";

// A figure: a muted label and its value; `rgba` alpha 0 is the card's text colour.
struct Figure {
    std::string label;
    std::string value;
    uint32_t rgba = 0;
};

// A card: a heading over figures in two aligned columns, then -- when `progress` is in
// [0, 1] -- a bar saying `progressText`, then `note` in a line of its own (`noteRgba` alpha 0:
// muted).
struct Card {
    std::string title;
    std::vector<Figure> figures;
    double progress = -1.0;
    std::string progressText;
    std::string note;
    uint32_t noteRgba = 0;
};

// The cards one after another. `scale` is the look's: what the bar's height and the gaps take.
void Cards (const std::vector<Card>& cards, const overlaylayers::Panel& look, float scale);

// One selected element as the HUD lists it.
struct SelectedElement {
    std::string guid;
    std::string type; // Archicad's name for its kind: "Slab", "Wall"
    std::string id;   // its element ID
    std::string layer;
    std::string storey; // its home storey
};

// What is selected in the surface the HUD is over: Archicad's selection under the overlay,
// the viewer's pick in the viewer. `known` false: the surface has not said yet.
struct SelectionPage {
    bool known = false;
    uint32_t count = 0;                    // how many are selected; `elements` holds the first few
    std::vector<SelectedElement> elements; // in selection order
    std::string note;                      // what the surface says beside them
};

// The Selection page's list: how many, then a row per element -- its kind and ID, its layer
// and storey -- and a line for the rest.
void SelectionList (const SelectionPage& page, const overlaylayers::Panel& look, float scale);

} // namespace hudshell
} // namespace archviz
} // namespace geomsrv

#endif
