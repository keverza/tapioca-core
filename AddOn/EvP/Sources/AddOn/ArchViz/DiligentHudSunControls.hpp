#ifndef EVP_ARCHVIZ_DILIGENTHUDSUNCONTROLS_HPP
#define EVP_ARCHVIZ_DILIGENTHUDSUNCONTROLS_HPP

#include "ArchViz/GradientLibrary.hpp"

#include <imgui.h>

#include <span>

namespace geomsrv::archviz {
bool DrawSunStudyRange (const char* id, float& from, float& to, float minimum, float maximum, bool duration);
// `bands` 0 draws the smooth ramp; kSunBandsMin..kSunBandsMax draws that many
// flat bands split at `edges`, each with a grip above the bar to drag.
// `ramp` is the hours colours' stops (SunStudyRampStops); empty, the legacy bins.
void DrawSunStudyGradient (bool blue, float& threshold, int bands = 0, float* edges = nullptr,
                           unsigned lowSunHex = 0x1F5FD1, std::span<const overlaylayers::ColourStop> ramp = {});
// The hours ramp itself, smooth or stepped, and the translucent low-sun box
// below `blueBelow` hours (< 0: none). ⚠️ THE ONE PAINTER for the panel legend
// and the screenshot legend, so the two cannot show different colours.
// `labels` writes each stepped cell's start hour inside it (SunStudyBandLabel),
// left out of a cell too narrow for its text.
void DrawSunStudyRampBar (ImDrawList* draw, ImVec2 start, ImVec2 size, int bands, const float* edges, float blueBelow,
                          unsigned lowSunHex = 0x1F5FD1, bool labels = true,
                          std::span<const overlaylayers::ColourStop> ramp = {});
void DrawSunStudyInspectControl (int& mode, const char* first, const char* second);
} // namespace geomsrv::archviz
#endif
