// ArchViz/DiligentHudRenderTab -- the viewer HUD's Settings page (DiligentHudShell.hpp): the
// HUD's own rows, then HANDOFF-HudTabs.md's Render/Display sections in the order a user
// already scans them -- preview, surfaces, environment, sun and shadows, colour and post,
// camera, visibility, annotation. The sections' bodies are the ones the old panel held
// (DiligentHudSections.cpp), each in one place.

#include "ArchViz/DiligentHudShell.hpp"

#include "ArchViz/AnnotationHudControls.hpp"
#include "ArchViz/DiligentHudNames.hpp"
#include "ArchViz/DiligentHudSections.hpp"
#include "ArchViz/DiligentScene.hpp"

#include <imgui.h>

#include <cmath>

namespace geomsrv {
namespace archviz {
namespace viewerhud {

namespace {

// What a preset writes; -1 (or a negative intensity) leaves a knob as it is.
struct Knobs {
    int renderMode;
    int renderQuality;
    int environment;
    int ambientOcclusion;
    float ambientOcclusionIntensity;
    int shadows;
    int background;
};

// ⚠️ THE REFERENCE'S MODES, WHERE THIS RENDERER CAN HONOUR THEM (HANDOFF-HudTabs.md section 1):
// Shaded, Wireframe, Architecture and Rendered. Sketch is not here: it needs an outline pass
// this renderer does not have, and a preset that cannot take effect is worse than none.
// ⚠️ TWO AO SCALES (the handoff's caveat 8), CONVERTED IN THE OPEN: the reference's 0-1 blend
// is doubled onto this renderer's 0-2 intensity -- Architecture's 0.70 is 1.4, Rendered's 0.40
// is 0.8. Ground, edge angle and tone-map choice are not native; Architecture is told from
// Rendered by its AO and by drawing no sky behind the model.
constexpr Knobs kPresets[] = {
    { int (SceneRenderMode::Shaded), int (RenderQuality::Fast), -1, 0, -1.0f, 1, -1 },   // Shaded
    { int (SceneRenderMode::Wireframe), -1, -1, -1, -1.0f, 0, -1 },                      // Wireframe
    { int (SceneRenderMode::Shaded), int (RenderQuality::Realistic), 1, 1, 1.4f, 1, 0 }, // Architecture
    { int (SceneRenderMode::Shaded), int (RenderQuality::Realistic), 1, 1, 0.8f, 1, 1 }, // Rendered
};
constexpr const char* kPreviewNames[] = { "Shaded", "Wireframe", "Architecture", "Rendered", "Custom" };
static_assert (sizeof (kPresets) / sizeof (kPresets[0]) == size_t (Preview::Custom),
               "a preview preset for every Preview but Custom");

bool Matches (const Knobs& knobs, const HudState& state)
{
    return (knobs.renderMode < 0 || state.renderMode == knobs.renderMode) &&
           (knobs.renderQuality < 0 || state.renderQuality == knobs.renderQuality) &&
           (knobs.environment < 0 || state.environmentEnabled == (knobs.environment != 0)) &&
           (knobs.ambientOcclusion < 0 || state.ambientOcclusion == (knobs.ambientOcclusion != 0)) &&
           (knobs.ambientOcclusionIntensity < 0.0f ||
            std::fabs (state.ambientOcclusionIntensity - knobs.ambientOcclusionIntensity) < 1.0e-4f) &&
           (knobs.shadows < 0 || state.shadowsEnabled == (knobs.shadows != 0)) &&
           (knobs.background < 0 || state.environmentBackground == (knobs.background != 0));
}

} // namespace

const char* PreviewName (Preview preview)
{
    const int at = int (preview);
    return at >= 0 && at <= int (Preview::Custom) ? kPreviewNames[at] : kPreviewNames[int (Preview::Custom)];
}

Preview MatchPreview (const HudState& state)
{
    for (int k = 0; k < int (Preview::Custom); ++k)
        if (Matches (kPresets[k], state))
            return Preview (k);
    return Preview::Custom;
}

void ApplyPreview (HudState& state, Preview preview)
{
    if (preview == Preview::Custom)
        return;
    const Knobs& knobs = kPresets[int (preview)];
    if (knobs.renderMode >= 0)
        state.renderMode = knobs.renderMode;
    if (knobs.renderQuality >= 0)
        state.renderQuality = knobs.renderQuality;
    if (knobs.environment >= 0)
        state.environmentEnabled = knobs.environment != 0;
    if (knobs.ambientOcclusion >= 0)
        state.ambientOcclusion = knobs.ambientOcclusion != 0;
    if (knobs.ambientOcclusionIntensity >= 0.0f)
        state.ambientOcclusionIntensity = knobs.ambientOcclusionIntensity;
    if (knobs.shadows >= 0)
        state.shadowsEnabled = knobs.shadows != 0;
    if (knobs.background >= 0)
        state.environmentBackground = knobs.background != 0;
}

void SettingsPage (Shell& shell, HudState& state, const DiligentSceneStats& scene)
{
    bool reset = false;
    hudshell::HudSettings (shell.fontStep, shell.placement, reset);

    // ---- the plan: the viewer opened in the floor plan's place (ViewerPlanMode.hpp) ----------
    // ⚠️ THE USER, 2026-10-03: a way back to the top view once it was turned, or no orbit in the
    // plan view at all. Both: the orbit is held until freed here, and the top view is a press.
    if (state.planMode && hudshell::Section ("plan", true)) {
        if (ImGui::Button ("Top view##plan", ImVec2 (-FLT_MIN, 0.0f)))
            state.planTopView = true;
        hudshell::Tip ("Straight down again, at the plan's rotation, on the storey's cut");
        ImGui::Checkbox ("orbit##plan", &state.planOrbit);
        ImGui::TextDisabled (state.planOrbit ? "Shift + wheel-button drag turns the view"
                                             : "held: Shift + wheel-button drag pans, as without Shift");
        // The cut: the walls' outline at the storey's cut height (ViewerPlanMode.hpp `DrawPlanCut`).
        ImGui::Checkbox ("wall outlines (the cut)##plan", &state.planCutShown);
        if (state.planCutShown) {
            ImGui::Checkbox ("fill the cut walls##plan", &state.planCutFill);
            ImGui::SliderFloat ("line##plancut", &state.planCutWidthPixels, 1.0f, 6.0f, "%.1f px");
            if (!scene.planCutReceived)
                ImGui::TextDisabled ("the cut comes with the model's first full read");
            else if (scene.planCutVertices == 0)
                ImGui::TextDisabled ("the cut meets nothing at this storey's cut height");
            else
                ImGui::TextDisabled ("%.0f m\xC2\xB2 cut at the storey's cut height", scene.planCutAreaM2);
        }
    }

    ImGui::SeparatorText ("Display");
    // ---- 1. preview: the headline control --------------------------------------------------
    const Preview now = MatchPreview (state);
    ImGui::SetNextItemWidth (-FLT_MIN);
    if (ImGui::BeginCombo ("##preview", PreviewName (now))) {
        for (int k = 0; k < int (Preview::Custom); ++k)
            if (ImGui::Selectable (PreviewName (Preview (k)), now == Preview (k)) && now != Preview (k))
                ApplyPreview (state, Preview (k));
        ImGui::EndCombo ();
    }
    ImGui::TextDisabled ("preview -- a preset over the knobs below; an edit reads Custom");

    // ---- 2. surfaces -----------------------------------------------------------------------
    if (hudshell::Section ("surfaces", true)) {
        ImGui::SetNextItemWidth (-FLT_MIN);
        ImGui::Combo ("##rendermode", &state.renderMode, kRenderModeNames, kRenderModeCount);
        ImGui::TextDisabled ("surfaces -- wireframe is what makes the OVERLAY readable");
        if (state.renderMode != int (SceneRenderMode::Shaded)) {
            // 1 = outlines only, and the default. The floor is 1, not 0: a tessellation
            // factor of 0 culls the patch.
            ImGui::SliderInt ("wire subdivisions", &state.wireTessellation, 1, 16);
            ImGui::SliderFloat ("wire width", &state.wireLineWidth, 0.5f, 3.0f, "%.2f px");
        }
        // ⚠️ A SEPARATE COMBO, NOT MORE ENTRIES IN THE ONE ABOVE: quality and surfaces are
        // independent axes and every pairing is wanted (RenderQuality, ViewerSettings.hpp).
        ImGui::SetNextItemWidth (-FLT_MIN);
        ImGui::Combo ("##renderquality", &state.renderQuality, kRenderQualityNames, kRenderQualityCount);
        ImGui::TextDisabled ("quality -- realistic adds specular + tone mapping");
    }
    // ---- 3. environment; 4. sun and shadows; 5. colour and post ------------------------------
    DrawEnvironmentControls (state, scene);
    DrawLightInspector (state, scene);
    DrawShadowSettings (state, scene);
    DrawPostProcessingControls (state, scene);
    // ---- 6. camera -------------------------------------------------------------------------
    if (hudshell::Section ("camera")) {
        // A plan is parallel while it is held to the plan; freed, it can be turned to perspective.
        ImGui::BeginDisabled (state.planMode && !state.planOrbit);
        ImGui::Checkbox ("axonometric (parallel projection)", &state.orthographic);
        ImGui::EndDisabled ();
    }
    // ---- 7. visibility ---------------------------------------------------------------------
    if (hudshell::Section ("visibility")) {
        ImGui::Checkbox ("callout under the cursor", &state.showCallout);
        DrawStorySliceControls (state, scene);
        DrawGhPreviewControls (state);
    }
    // ---- annotation's style: the Annotation tab's, until it has one ----------------------------
    if (hudshell::Section ("annotation"))
        DrawAnnotationHudControls (state);
}

} // namespace viewerhud
} // namespace archviz
} // namespace geomsrv
