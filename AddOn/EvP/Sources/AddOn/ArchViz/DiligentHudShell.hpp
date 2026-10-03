#ifndef EVP_ARCHVIZ_DILIGENTHUDSHELL_HPP
#define EVP_ARCHVIZ_DILIGENTHUDSHELL_HPP

// ArchViz/DiligentHudShell -- the viewer's HUD in the overlays' design (the user, 2026-10-03:
// "Overlay and 3d viewer should have unified hud layout and style. Current overlay hud is to be
// taken as a base design template."): the same dock at the view's right edge -- the overlay's
// circle at its top, the viewer's at its bottom -- and the same floating panel with the same
// own tabs, drawn through the same HudShell. What differs is only what the pages hold: the
// viewer's are its scene's.
//
//   Stats      the model the viewer holds, what it is still reading, the slices and previews
//   Selection  Archicad's selection's building section, then the element picked in the
//              viewer and its Tapioca metadata to edit
//   Sun study  while a study is on screen: its view, its range, its legend (a command's tab)
//   Settings   the HUD's own, then HANDOFF-HudTabs.md's Render/Display sections: the preview
//              presets, surfaces, environment, sun and shadows, colour and post, camera,
//              visibility, annotation
//   Debug      the frame, the GPU, the scene's counters, the debug view (its combo index is
//              its enum value), the scene-text checks, the graph interaction lab
//
// ⚠️ THE PRESSES ARE SAID, NOT DONE, HERE: the overlay's circle asks SurfaceSwitch, which acts
// on the main thread -- this is the render thread, which calls neither ACAPI nor the gate.
//
// ⚠️ RENDER THREAD, inside a frame DiligentHud began, under ImGuiContextLock. The read-only
// overlay surface (SurfaceMode::Overlay) never draws this: it keeps its flat readout (the
// handoff's caveat 4).

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/HudSection.hpp"
#include "ArchViz/HudShell.hpp"

#include <cstdint>
#include <string>

struct ImFont;

namespace geomsrv {
namespace archviz {

struct DiligentSceneStats;

namespace viewerhud {

// What the user did to the viewer's HUD: kept by DiligentHud across frames.
struct Shell {
    bool open = true;
    std::string held = hudshell::kStatsKey;
    std::string shownLast;
    hudshell::Placement placement;
    uint32_t fontStep = hudshell::kFontStepDefault;
    hudsection::Run floors; // the building section's floors picked on the Selection tab
};

// What one frame of the viewer costs, measured by DiligentHud.
struct Frame {
    float frameMs = 0.0f;
    float worstMs = 0.0f; // the worst of the last few seconds, held to be read
    float dpiScale = 1.0f;
    const char* fontNote = ""; // why the HUD is not in the bundled font, when it is not
};

// The tab a command's study opens while it is on screen.
constexpr char kSunStudyKey[] = "tapioca.sunstudy";

// The dock and the floating panel. `font` is the bundled font (null: ImGui's own).
void Draw (Shell& shell, HudState& state, const DiligentSceneStats& scene, uint32_t width, uint32_t height,
           ImFont* font, const Frame& frame);

// ---- the pages (DiligentHudTabs.cpp, DiligentHudRenderTab.cpp) --------------------------------
void StatsPage (HudState& state, const DiligentSceneStats& scene, float ui);
void SelectionPage (Shell& shell, const HudState& state, float ui);
void SunStudyPage (HudState& state, const DiligentSceneStats& scene);
void DebugPage (HudState& state, const DiligentSceneStats& scene, const Frame& frame, uint32_t width, uint32_t height,
                float ui);
// The HUD's own rows, then the Render/Display sections.
void SettingsPage (Shell& shell, HudState& state, const DiligentSceneStats& scene);

// ---- the preview presets (HANDOFF-HudTabs.md section 1) ----------------------------------------
// ⚠️ A PRESET WRITES THE ORTHOGONAL KNOBS, IT IS NOT A NEW ENUM: quality and mode stay
// independent axes. The combo reads which preset the knobs match now, so editing any of them
// reads "Custom" -- no flag to keep in step.
enum class Preview : int { Shaded = 0, Wireframe, Architecture, Rendered, Custom };
const char* PreviewName (Preview preview);
Preview MatchPreview (const HudState& state);
void ApplyPreview (HudState& state, Preview preview);

} // namespace viewerhud
} // namespace archviz
} // namespace geomsrv

#endif
