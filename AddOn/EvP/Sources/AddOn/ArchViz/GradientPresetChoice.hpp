#ifndef EVP_ARCHVIZ_GRADIENTPRESETCHOICE_HPP
#define EVP_ARCHVIZ_GRADIENTPRESETCHOICE_HPP

// ArchViz/GradientPresetChoice -- the one picker for the gradient library (#102), drawn
// by the overlay HUD's panel ramps and the viewer's analysis tabs alike. Pure Dear
// ImGui over ArchViz/GradientLibrary: it owns no state but what ImGui keeps for an open
// popup, and it changes only the id it is handed -- the caller recolours.

#include "ArchViz/GradientLibrary.hpp"

#include <imgui.h>

#include <span>
#include <string>

namespace geomsrv::archviz::gradientchoice {

// `stops` across the rectangle: linear between stops, or `bands` flat steps (0: smooth),
// each the colour at its centre.
void DrawSwatch (ImDrawList* draw, ImVec2 min, ImVec2 max, std::span<const overlaylayers::ColourStop> stops,
                 int bands = 0);

// The colour at `t` (0..1) along `stops`, 0xRRGGBBAA, interpolated per channel.
uint32_t ColourAt (std::span<const overlaylayers::ColourStop> stops, float t);

// A button showing the current preset's swatch and name. Pressed, a popup lists every
// preset grouped by category -- `first` leads -- each row its swatch, name and default
// range. A row picked (click, or arrows and Enter) applies AT ONCE; Cancel or Escape
// restores the preset the popup opened on; Done or a click outside keep the current one.
// True on the frame `presetId` changed, the Cancel included. `id` is the widget's
// stable ImGui id within its window.
bool GradientPresetChoice (const char* id, std::string& presetId, float width,
                           overlaylayers::GradientCategory first = overlaylayers::GradientCategory::General);

} // namespace geomsrv::archviz::gradientchoice

#endif
