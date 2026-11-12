#ifndef TAPIOCA_ARCHVIZ_SUNPATHSCENE_HPP
#define TAPIOCA_ARCHVIZ_SUNPATHSCENE_HPP

// ArchViz/SunPathScene -- the sun path as depth-tested scene geometry (#179).
//
// Pure: world-space GhPreview buckets, no device, camera or ImGui. The HUD
// builds them from the shown result and DiligentScene draws them through three
// GhPreviewLayer instances, one per line width.
//
// ⚠️ WHY NOT THE HUD. The HUD paints after the scene with no depth test, so the
// model could never hide the path. And the palette viewport renders ImGui into
// an sRGB view using Diligent's gamma-blend approximation
// `G2L(src*A) + dst*G2L(1-A)`. At low alpha the colour term vanishes and only
// darkening is left, so a translucent yellow ribbon read as grey (#179,
// measured: yellow at alpha 20 gave (191,191,192) over (206,206,208)). Here
// the blend is a plain linear `SrcAlpha, InvSrcAlpha`, the line width is in
// screen pixels in the line shader, and the model occludes everything behind it.
//
// ⚠️ NO SOLAR FORMULA. Every direction is Archicad's capture or a stored
// analysis SunStep, exactly as SUNPATH-SYNC.md requires; this file only places
// those unit directions on the display dome.

#include "ArchViz/GhPreviewGeometry.hpp"
#include "ArchViz/SunPathBand.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::archviz {

// One GhPreviewLayer each: GhPreviewLayer's width is per draw, not per vertex.
constexpr float kSunPathFinePixels = 1.25f; // compass, analemmas, month lines
constexpr float kSunPathArcPixels = 2.0f;   // date arcs, analysis day lines, suns, surface
constexpr float kSunPathBoldPixels = 5.0f;  // the highlighted day's time slice

struct SunPathSceneGeometry {
    GhPreviewDrawables fine, arcs, bold;
    // What the analysis drawing contains, for the panel caption.
    size_t surfaceQuads = 0;
    size_t surfaceStride = 0; // traced days between joined surface rows; 0 = no surface
    size_t dayLines = 0;
};

// The display dome: unit directions are placed at `centre + radius * direction`;
// the compass lies at `groundZ`.
struct SunPathFrame {
    double centre[3] = {};
    double radius = 0.0;
    double groundZ = 0.0;
};

// The three analysis forms the runner produces, drawn differently (#179):
//   Day             one traced day at its timestep: its arc, discrete steps as suns;
//   EveryDay        a daily-resolved period ("average day"): a boundary surface
//                   across the traced days, first/last-day lines, month lines;
//   MonthlyPreview  the Preview estimate ("average month"): one line per
//                   representative day, a lighter surface between them.
enum class SunPathPreviewForm : uint8_t { None = 0, Day, EveryDay, MonthlyPreview };

SunPathPreviewForm SunPathPreviewFormOf (const SunPathBand& band, bool period, bool estimate);

// Display-only emphasis. `month`/`day` name the highlighted traced date (day 0
// is the whole month); `slice` is the interactive clock slice in minutes, which
// thickens the highlighted day's line but never changes the analysis set.
struct SunPathAnalysisView {
    int month = 0, day = 0;
    bool highlightAll = false;
    double sliceFrom = 0.0, sliceTo = 1440.0;
};

// RGBA8 0xRRGGBBAA from DISPLAY (sRGB) components. The palette target writes
// through an sRGB view and this layer's colours are linear, so the RGB is
// linearised here to show the colour named. Alpha is coverage and stays linear.
uint32_t SunPathDisplayRgba (int r, int g, int b, int a);

void SunPathWorld (const SunPathFrame& frame, const float direction[3], float out[3]);

// Rings, 32 ticks and a red north tick at `groundZ`, rotated to project north.
void AppendSunPathCompass (const SunPathFrame& frame, double northRadians, SunPathSceneGeometry& out);

// A polyline through unit directions; anything with a `direction[3]` member.
template <class Point>
void AppendSunPathDirections (const SunPathFrame& frame, const std::vector<Point>& points, uint32_t rgba,
                              GhPreviewBucket& bucket)
{
    for (size_t i = 1; i < points.size (); ++i) {
        float a[3], b[3];
        SunPathWorld (frame, points[i - 1].direction, a);
        SunPathWorld (frame, points[i].direction, b);
        AppendPreviewSegment (a, b, rgba, bucket);
    }
}

// The analysis geometry for `form`. Exact stored directions only; runs are
// never bridged across days, horizon gaps or missing timesteps.
void AppendSunPathAnalysis (const SunPathFrame& frame, const SunPathBand& band, SunPathPreviewForm form,
                            const SunPathAnalysisView& view, SunPathSceneGeometry& out);

// The selected sun: an opaque, depth-writing sphere on the dome.
void AppendSunPathSun (const SunPathFrame& frame, const float direction[3], SunPathSceneGeometry& out);

// A sphere of `radius` metres at `at`. With `shaded` false the normals are zero,
// which GhPreviewLayer draws fully lit (flat).
void AppendSunPathSphere (const float at[3], float radius, uint32_t rgba, bool shaded, int slices, int stacks,
                          GhPreviewBucket& bucket);

// The panel caption: what the scene shows, including what it joins or omits.
std::string SunPathSceneCaption (SunPathPreviewForm form, const SunPathBand& band, const SunPathSceneGeometry& scene,
                                 bool estimate);

} // namespace geomsrv::archviz

#endif
