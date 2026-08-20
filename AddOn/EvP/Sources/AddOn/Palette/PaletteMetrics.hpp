#ifndef GEOMETRYSERVER_PALETTE_PALETTEMETRICS_HPP
#define GEOMETRYSERVER_PALETTE_PALETTEMETRICS_HPP

// Layout metrics shared by the palette shell and its sub-objects (ParamPanel,
// ResultsTable). Standard Archicad spacing: multiples of 4/8.
//
// ONLY constants with more than one consumer live here. A constant used by a
// single file stays file-local in that file's anonymous namespace, and enters
// this header on its SECOND consumer — never speculatively. (Same rule as
// NativeCommands/CommandUtils in phase 1 of cpp-architecture-plan.md.)

namespace evp::palette {

constexpr short Margin = 14;
constexpr short RowHeight = 22;
constexpr short RowGap = 6;
constexpr short ButtonWidth = 120;

// Feature E — the draggable bar itself. Both splitters are shell-owned, but the
// sub-objects size their bands around the bar, so the height is shared.
constexpr short SplitterBarHeight = 7;

// These two were file-local in ControlPalette.cpp until the band layout moved to
// ControlPaletteLayout.cpp, which gave each of them its second consumer.
//
// F4 — the gap left below the scrolled band, so its last row does not sit on the
// panel's bottom edge.
constexpr short BottomMargin = 8;

// One text line. It was briefly 40, to fit the address inside the server button;
// that address is its own item above the row now — ServerBand.hpp says why.
constexpr short ActionButtonHeight = 28;

// ⚠️ WRAP AGAINST THIS, NOT THE LIVE PANEL WIDTH.
//
// The palette's DESIGN width — 'GDLG' 32510 in RINT/AddOn.grc opens 440 x 600,
// which is also the narrowest it is normally used at.
//
// Wrapped text used to be re-flowed from panel.GetWidth () on every Rebuild, and
// dynamic flow HAS NEVER WORKED here: a resize does not rebuild the text, so the
// breaks were simply whatever the panel happened to be when the command was
// selected — widening left the old narrow breaks in place, and the reflow that
// was supposed to fix that never arrived. Wrapping to a fixed width is worse in
// theory and right in practice: the breaks are identical every time, and a wider
// panel just leaves slack on the right, which is what a document does.
//
// A panel narrowed below this clips the ends of lines. That is the accepted
// trade: stable text at the width the palette is actually used at, rather than
// text that rearranges itself and still gets it wrong.
constexpr short DefaultPanelWidth = 440;

// The width wrapped text targets: the designed panel less both margins. Shared,
// so two stacked wrapped blocks cannot disagree about where a line ends.
constexpr short WrapWidth = DefaultPanelWidth - 2 * Margin;

} // namespace evp::palette

#endif
