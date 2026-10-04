#ifndef EVP_ARCHVIZ_OVERLAYHUDITEMS_HPP
#define EVP_ARCHVIZ_OVERLAYHUDITEMS_HPP

// ArchViz/OverlayHudItems -- what each kind of HUD panel item draws (OverlayLayers.hpp
// `PanelItem`), with Dear ImGui, inside the panel's window. The engine (OverlayHud.hpp)
// owns the context, the windows and what the user did to them; these only draw an item
// where the cursor is and say what the pointer is on.
//
// MAIN THREAD, inside an ImGui frame the engine began, the panel's window current.

#include "ArchViz/HudShell.hpp"
#include "ArchViz/OverlayLayers.hpp"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace geomsrv {
namespace archviz {
namespace overlayhud {
namespace items {

// Every HUD's colours (HudShell.hpp).
using hudshell::Colour;
using hudshell::Contrast;
using hudshell::Packed;
using hudshell::Unpacked;
using hudshell::WithAlpha;

std::string Number (double value, uint32_t decimals);

// What a ramp says at `t` along it -- the value, or the band it falls in, and its colour
// -- in the HUD's tip (HudShell.hpp), its arrow at `at`, on `side` of it. `band` is what its
// heatmap shows while the ramp is pointed at: the colormap's band, or a twentieth of the
// range either side.
void ValueTip (const overlaylayers::Colormap& colormap, float t, double low, double high, uint32_t decimals,
               const std::string& unit, ImVec2 at, hudshell::TipSide side, double band[2]);

// `panel.items[begin, end)`, rows, in two aligned columns.
void Rows (const overlaylayers::Panel& panel, size_t begin, size_t end, float scale);
// `width` is what an item without a width of its own spans.
void Text (const overlaylayers::Panel& panel, const overlaylayers::PanelItem& item, float width, float scale);
void Progress (const overlaylayers::PanelItem& item, float width, float scale);
void Swatch (const overlaylayers::Panel& panel, const overlaylayers::PanelItem& item, float scale);
void Plot (const overlaylayers::PanelItem& item, float width, float scale);
void Table (const overlaylayers::PanelItem& item);
// True, with the band under the pointer, when the pointer is on the bar.
bool Ramp (const overlaylayers::Panel& panel, const overlaylayers::PanelItem& item, float width, float scale,
           double band[2]);
// A section's header, `open` as the user left it and as they leave it now; its `info`
// behind a small (i) that says it when pointed at.
void Section (const overlaylayers::Panel& panel, const overlaylayers::PanelItem& item, bool& open, float scale);

// ---- the design's kinds: a card of figures ------------------------------------------------

// Figures in a grid, `perRow` to a row (2 unless said), rules between: each cell a muted
// label over its value, larger.
void Metrics (const overlaylayers::Panel& panel, const overlaylayers::PanelItem& item, float width, float scale);
// `panel.items[begin, end)`, swatches, as a key: `perRow` to a row, each its colour, its
// name and -- when it has one -- its value at the right.
void Keys (const overlaylayers::Panel& panel, size_t begin, size_t end, float width, float scale);
// Shares of a whole in one rounded bar, each its colour and its percentage inside it
// where it fits; pointed at, a segment says its label, value and share.
void Stack (const overlaylayers::Panel& panel, const overlaylayers::PanelItem& item, float width, float scale);
// A histogram: a bar per value in its colour, the value axis at the left with the grid
// behind, the labels under the bars and the caption under them; pointed at, a bar says
// its label and value.
void Bars (const overlaylayers::Panel& panel, const overlaylayers::PanelItem& item, float width, float scale);

// ---- the controls: their value is the caller's, held by the engine ------------------------------

// A box and `item.text` beside it; true when pressed, `on` flipped.
bool Checkbox (const overlaylayers::PanelItem& item, bool& on);
// `item.text` over a bar `width` wide from `item.min` to `item.max`, on `item.step`s from
// the min (0: any), its value inside it with `item.decimals` and `item.unit`. True while
// dragged to a new `value`; `released` when a drag that changed it ends.
bool Slider (const overlaylayers::Panel& panel, const overlaylayers::PanelItem& item, double& value, float width,
             bool& released);
// What a slider's value says: `item.decimals` of it and its unit.
std::string SliderText (const overlaylayers::PanelItem& item, double value);
// `item.text` over a dropdown `width` wide of `item.labels`; true when another is chosen.
bool Combo (const overlaylayers::Panel& panel, const overlaylayers::PanelItem& item, uint32_t& chosen, float width);
// A button saying `item.text`, `width` wide when the item gives one; true when pressed.
bool Button (const overlaylayers::PanelItem& item, float width);

// The colour of an item's `index`th segment or bar of `count`: its own `colors`, else
// its colormap's at its place, else a palette of ten.
uint32_t SegmentColour (const overlaylayers::PanelItem& item, size_t index, size_t count);

} // namespace items
} // namespace overlayhud
} // namespace archviz
} // namespace geomsrv

#endif
