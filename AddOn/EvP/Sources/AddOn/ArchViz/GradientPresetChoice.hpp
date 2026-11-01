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
#include <vector>

namespace geomsrv::archviz::gradientchoice {

// `stops` across the rectangle: linear between stops, or `bands` flat steps (0: smooth),
// each the colour at its centre.
void DrawSwatch (ImDrawList* draw, ImVec2 min, ImVec2 max, std::span<const overlaylayers::ColourStop> stops,
                 int bands = 0);

// The colour at `t` (0..1) along `stops`, 0xRRGGBBAA, interpolated per channel.
uint32_t ColourAt (std::span<const overlaylayers::ColourStop> stops, float t);

// A colour swatch that can always be rolled back (#102): pressed, a picker opens with
// the colour it opened on beside the current one; changes apply live, Cancel or
// Escape put the opening colour back, Done or a click outside keep the new one.
// `label` (text before "##") is drawn after the swatch. With `alpha` false the
// colour's own alpha is kept. True on the frame `rgba` changed.
bool ColourChoice (const char* id, uint32_t& rgba, bool alpha, const char* label = nullptr);
// ColourChoice's popup alone, for a caller with its own swatch: open it with the
// colour to roll back to, and draw it every frame with the same id.
void OpenColourPopup (const char* id, uint32_t rgba);
bool ColourPopup (const char* id, uint32_t& rgba, bool alpha);

// ---- a custom ramp's stops (#102) ------------------------------------------------
// COLOUR stops are the user's colours. A HINT stop (ColourStop::hint) sits between two
// of them where their blend is half way, coloured their 50/50 mix -- to everything that
// draws a ramp it is an ordinary stop, so a moved blend needs no renderer of its own.
// The ends are colour stops at 0 and 1; at most kMaxStops in all.
int ColourStopCount (const std::vector<overlaylayers::ColourStop>& stops);
// Every hint re-coloured from its two colours; a hint without both is dropped.
void RefreshHints (std::vector<overlaylayers::ColourStop>& stops);
// A colour at `t`, in the colour already shown there; its segment's hint goes. The
// new stop's index, or -1 (at an existing stop, or full).
int AddColourStop (std::vector<overlaylayers::ColourStop>& stops, float t);
// A colour removed with the hints either side; an end's neighbour becomes the end.
// False when only two colours would be left.
bool RemoveColourStop (std::vector<overlaylayers::ColourStop>& stops, int index);
// An inner colour moved between its neighbours, its hints keeping their place in
// their segments. The ends do not move.
bool MoveColourStop (std::vector<overlaylayers::ColourStop>& stops, int index, float t);
// The blend after colour stop `colour` put at `t`: a hint added, moved, or -- back
// at the middle -- removed.
bool SetBlendMidpoint (std::vector<overlaylayers::ColourStop>& stops, int colour, float t);

// The custom ramp editor: a bar with a circle per colour on its middle and an arrow
// per blend under it. Click a circle to pick its colour (ColourPopup), drag it along
// to move it or off the bar to remove it; click the bar to add a colour; drag an
// arrow to move that blend. True when the stops changed.
bool EditStops (std::vector<overlaylayers::ColourStop>& stops, float width);

// A button showing the current gradient's swatch and name. Pressed, a popup lists
// "Yours" first -- `none` (the caller's own colours, presetId empty) when given, and
// Custom..., the user's own stops in `custom` (presetId kCustomGradientId), edited
// under the list and seeded from what was shown -- then every preset grouped by
// category, `first` leading, each row its swatch, name and default range. Any pick or
// edit applies AT ONCE; Cancel or Escape restore the preset AND the custom stops the
// popup opened on; Done or a click outside keep them. True on the frame either changed.
// `id` is the widget's stable ImGui id within its window.
bool GradientPresetChoice (const char* id, std::string& presetId, std::vector<overlaylayers::ColourStop>& custom,
                           float width,
                           overlaylayers::GradientCategory first = overlaylayers::GradientCategory::General,
                           const char* none = nullptr);

} // namespace geomsrv::archviz::gradientchoice

#endif
