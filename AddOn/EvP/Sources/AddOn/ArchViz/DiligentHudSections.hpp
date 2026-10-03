#ifndef EVP_ARCHVIZ_DILIGENTHUDSECTIONS_HPP
#define EVP_ARCHVIZ_DILIGENTHUDSECTIONS_HPP

// ArchViz/DiligentHudSections -- the viewer HUD's sections, each one block of the panel: the
// environment, post-processing, the storey slices, the Grasshopper preview, the light
// inspector, the shadow settings; and the two windows of its own, the hover callout and the
// selected element's facts. Drawn by DiligentHud::Draw (DiligentHud.hpp), which owns the
// context, the frame and the windows around them.
//
// ⚠️ RENDER THREAD, inside a frame DiligentHud began, under ImGuiContextLock -- like the rest
// of the viewer HUD. Nothing here reads ACAPI.

#include "ArchViz/DiligentHud.hpp"

#include <cstdint>

namespace geomsrv {
namespace archviz {

struct DiligentSceneStats;
struct InputSnapshot;

// Each draws its own collapsing header and what folds under it.
void DrawEnvironmentControls (HudState& state, const DiligentSceneStats& scene);
void DrawPostProcessingControls (HudState& state, const DiligentSceneStats& scene);
void DrawStorySliceControls (HudState& state, const DiligentSceneStats& scene);
void DrawGhPreviewControls (HudState& state);
// On the read-only overlay these two are readouts, their header always open.
void DrawLightInspector (HudState& state, const DiligentSceneStats& scene);
void DrawShadowSettings (HudState& state, const DiligentSceneStats& scene);

// Windows of their own: the callout follows the cursor; the selected element's facts.
void DrawHoverCallout (const HudState& state, const InputSnapshot& input, uint32_t width, uint32_t height);
void DrawSelectedElementWindow (const HudState& state, uint32_t height);

} // namespace archviz
} // namespace geomsrv

#endif
