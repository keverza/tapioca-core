#ifndef EVP_ARCHVIZ_OVERLAYHUDITEMS_HPP
#define EVP_ARCHVIZ_OVERLAYHUDITEMS_HPP

// ArchViz/OverlayHudItems -- what each kind of HUD panel item draws (OverlayLayers.hpp
// `PanelItem`), with Dear ImGui, inside the panel's window. The engine (OverlayHud.hpp)
// owns the context, the windows and what the user did to them; these only draw an item
// where the cursor is and say what the pointer is on.
//
// MAIN THREAD, inside an ImGui frame the engine began, the panel's window current.

#include "ArchViz/OverlayLayers.hpp"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace geomsrv {
namespace archviz {
namespace overlayhud {
namespace items {

// 0xRRGGBBAA as ImGui wants it, and back.
ImVec4 Colour (uint32_t rgba);
ImU32 Packed (uint32_t rgba);
uint32_t Unpacked (ImU32 col);
// `rgba` with its alpha multiplied by `factor`.
uint32_t WithAlpha (uint32_t rgba, float factor);
// Text that reads on `rgba`: dark on a light colour, white on a dark one.
uint32_t Contrast (uint32_t rgba);
std::string Number (double value, uint32_t decimals);

// What a ramp says at `t` along it -- the value, or the band it falls in, and its colour
// -- in a tooltip at `at` (its `pivot` there). `band` is what its heatmap shows while
// the ramp is pointed at: the colormap's band, or a twentieth of the range either side.
void ValueTip (const overlaylayers::Colormap& colormap, float t, double low, double high, uint32_t decimals,
               const std::string& unit, ImVec2 at, ImVec2 pivot, float scale, double band[2]);

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

// ---- the dock ------------------------------------------------------------------------------

// A button of the dock, `size` big, saying `label` in the middle: filled with `colours`'
// accent when `filled`, in its card's colours otherwise, tinted when pointed at and pressed,
// its `corners` rounded. A panel's tab is one, filled while the panel is open. True when
// pressed.
bool DockButton (const char* id, const std::string& label, const overlaylayers::Panel& colours, bool filled,
                 ImVec2 size, ImDrawFlags corners, float scale);

// The colour of an item's `index`th segment or bar of `count`: its own `colors`, else
// its colormap's at its place, else a palette of ten.
uint32_t SegmentColour (const overlaylayers::PanelItem& item, size_t index, size_t count);

} // namespace items
} // namespace overlayhud
} // namespace archviz
} // namespace geomsrv

#endif
