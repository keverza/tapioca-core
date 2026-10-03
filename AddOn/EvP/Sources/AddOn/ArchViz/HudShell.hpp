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

// ---- tips -----------------------------------------------------------------------------------

// ⚠️ A TIP IS SAID BESIDE WHAT IT IS ABOUT, NOT AT THE POINTER (the user, 2026-10-03: the dock's
// hover text on a light ground, to the circle's left, as the system's own tooltip with its arrow
// at what it names -- and so elsewhere on the HUD). One look whatever the card's: a near-white
// bubble, dark text, a hairline edge, a soft shadow. On the side asked while the view has room
// there, on the opposite side otherwise, never off the view. Drawn last, on the foreground
// list: no window, so it never takes the pointer and never counts as the HUD under it.
enum class TipSide : uint8_t { Left, Right, Above, Below };

constexpr uint32_t kTipGroundRgba = 0xF9F9F9FAu;
constexpr uint32_t kTipEdgeRgba = 0x00000030u;
constexpr uint32_t kTipInkRgba = 0x1B1B1BFFu;

// `text` in the bubble, its arrow's point at `at` and the bubble on `side` of it, in the current
// font; `swatch` (alpha not 0) a square of that colour before the text. Lines past 22 em wrap.
void TipAt (ImVec2 at, TipSide side, const std::string& text, uint32_t swatch = 0);

// Beside the rectangle `min`-`max`: its middle on `side`, a hair away.
void TipBeside (ImVec2 min, ImVec2 max, TipSide side, const std::string& text, uint32_t swatch = 0);

// The last item's tip while the pointer is on it, disabled or not. True when it was said.
bool Tip (const std::string& text, TipSide side = TipSide::Left, uint32_t swatch = 0);

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

// ⚠️ A CIRCLE IS A SURFACE'S STATE (the user, 2026-10-03): the overlay's at the top of the dock,
// the separate viewer's at its bottom. Off -- a faint ring; Ready -- neutral; Busy -- composing,
// from amber to neutral as `progress` goes from 0 to 1, an arc round it saying how far (a
// turning one when it cannot say); Attention -- neutral and blinking: the user must do
// something (orbit the view, say); Error -- red. Filled while its surface is the one shown,
// a ring otherwise.
enum class Phase : uint8_t { Off = 0, Ready, Busy, Attention, Error };

struct Circle {
    Phase phase = Phase::Off;
    bool active = false;    // its surface is the one shown: filled
    float progress = -1.0f; // Busy: 0 to 1; negative: it cannot say
    std::string tip;        // what it says when pointed at
};

// The colours a circle is drawn in over a ground whose text colour is `ink`, `seconds` into
// ImGui's clock (Attention blinks once a second).
constexpr uint32_t kBusyRgba = 0xE8A33DFFu;
constexpr uint32_t kErrorRgba = 0xE5484DFFu;
uint32_t CircleColour (const Circle& circle, uint32_t ink, double seconds);

// What the user pressed on the dock: its title, its top circle, its bottom circle.
struct DockPress {
    bool title = false;
    bool top = false;
    bool bottom = false;
};

// The dock's tab: `top` at its top, then `label` turned a quarter clockwise -- it reads top to
// bottom, as a tab on the right edge does -- then `bottom` when given; `padding` round it
// across and along, rounded on the left where it comes out of the view's edge. Filled with
// `look`'s accent while `open`, in its card's colours otherwise; each part tinted when pointed
// at and pressed, a circle's `tip` said by it.
DockPress DockTab (const char* id, const std::string& label, const overlaylayers::Panel& look, bool open,
                   const Circle& top, const Circle* bottom, ImVec2 padding, float scale);

// ⚠️ A RIGHT CLICK A CONTROL ANSWERS IS THE CONTROL'S: the HUD's own menu opens on a right click
// anywhere on the HUD (OverlayHudHost.cpp `Menu`) unless a control -- the building section's
// floors -- claimed the click in this frame. Per ImGui context and frame; under its lock.
void ClaimRightClick ();
bool RightClickClaimed ();

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
    bool closed = false;  // the close button at the tab row's end
    std::string pressed;  // the tab the user pressed in this frame; empty for none
    std::string shown;    // the tab whose page was drawn
    bool moved = false;   // dragged in this frame: `placement` says where to
    bool scrolls = false; // the page shown is taller than the room it has: it scrolls
    ImGuiWindow* window = nullptr;
};

// The panel's margin from the view's edge where its look gives none -- the far edge from the
// one it hangs from -- in logical pixels; and the least of a page it shows however short the
// view, in its font's em.
constexpr float kViewMargin = 8.0f;
constexpr float kLeastPageEm = 4.0f;

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
//
// ⚠️ NEVER TALLER THAN THE VIEW; THE PAGE SCROLLS, THE TAB ROW STAYS (the user, 2026-10-03: the
// Settings page ran off the view and could not be read). The panel takes at most the view's
// height but its offset from the edge it hangs from and a margin at the other (`kViewMargin`;
// a margin at each when it is centred or was dragged); a page taller than what is left under
// the tab row and over the footer scrolls inside it, by the wheel and by its bar, each tab's
// at its own place.
HostResult Host (const HostSpec& spec, const std::string& held, std::string& shownLast, Placement& placement,
                 const std::function<void (const std::string& key)>& page, const std::function<void ()>& footer);

// ---- widgets every page uses (HANDOFF-HudTabs.md's shared helpers) -----------------------

// A collapsing section; false while it is folded, so a page can `if (!Section (...)) return;`.
// ImGui keeps its open state by its label's id.
bool Section (const char* label, bool defaultOpen = false);

// ⚠️ A COLOUR IS CHOSEN FROM A FEW, NOT MIXED (the user, 2026-10-03: style controls for what
// the HUD displays; editing by dropdowns, not dialogs). A dropdown of named colours, each with
// its swatch, the one set shown in it -- by name, or as "custom" with its swatch when it is none
// of them. `keepAlpha`: a choice keeps `rgba`'s own alpha (a fill's translucency). True when the
// user chose another.
bool ColourChoice (const char* id, uint32_t& rgba, bool keepAlpha = false);

// The HUD's own settings, the same in every HUD: its text size's step and its position --
// a Reset that puts the floating panel back where its look asks. True when either was changed
// by the user in this frame; `reset` says which.
bool HudSettings (uint32_t& fontStep, Placement& placement, bool& reset);

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
