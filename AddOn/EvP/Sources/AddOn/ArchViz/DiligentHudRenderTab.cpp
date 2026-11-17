// ArchViz/DiligentHudRenderTab -- the viewer HUD's Settings page (DiligentHudShell.hpp): the
// HUD's own rows, then HANDOFF-HudTabs.md's Render/Display sections in the order a user
// already scans them -- preview, surfaces, environment, sun and shadows, colour and post,
// camera, visibility, annotation. The sections' bodies are the ones the old panel held
// (DiligentHudSections.cpp), each in one place.

#include "ArchViz/DiligentHudShell.hpp"
#include "ArchViz/GraphicsSettingsUi.hpp"

#include "ArchViz/AnnotationHudControls.hpp"
#include "ArchViz/DiligentHudNames.hpp"
#include "ArchViz/DiligentHudSections.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/OrbitAnchor.hpp"
#include "ArchViz/SectionPlane.hpp"
#include "ArchViz/TechnicalEdges.hpp"
#include "ArchViz/ViewPresets.hpp"

#include <imgui.h>

#include <cmath>
#include <cstdio>

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
    int clay;
    int edges;
};

// ⚠️ THE REFERENCE'S MODES, WHERE THIS RENDERER CAN HONOUR THEM (HANDOFF-HudTabs.md section 1):
// Shaded, Wireframe, Architecture and Rendered, plus Clay (issue #14 P2): Architecture's light
// over one neutral material (ClayLook.hpp) -- the reference's arctic look. Sketch is not here: it
// needs an outline pass this renderer does not have, and a preset that cannot take effect is
// worse than none. Every preset but Wireframe states `clay`, so Architecture and Clay stay apart.
// Technical (also #14 P2) is clay under the model's shape-defining edges in the flat Fast light,
// without shadows or AO: a line drawing with its faces filled. Clay leaves the edges as they are.
// ⚠️ TWO AO SCALES (the handoff's caveat 8), CONVERTED IN THE OPEN: the reference's 0-1 blend
// is doubled onto this renderer's 0-2 intensity -- Architecture's 0.70 is 1.4, Rendered's 0.40
// is 0.8. Ground, edge angle and tone-map choice are not native; Architecture is told from
// Rendered by its AO and by drawing no sky behind the model.
constexpr Knobs kPresets[] = {
    { int (SceneRenderMode::Shaded), int (RenderQuality::Fast), -1, 0, -1.0f, 1, -1, 0, 0 },    // Shaded
    { int (SceneRenderMode::Wireframe), -1, -1, -1, -1.0f, 0, -1, -1, -1 },                     // Wireframe
    { int (SceneRenderMode::Shaded), int (RenderQuality::Realistic), 1, 1, 1.4f, 1, 0, 0, 0 },  // Architecture
    { int (SceneRenderMode::Shaded), int (RenderQuality::Realistic), 1, 1, 0.8f, 1, 1, 0, 0 },  // Rendered
    { int (SceneRenderMode::Shaded), int (RenderQuality::Realistic), 1, 1, 1.4f, 1, 0, 1, -1 }, // Clay
    { int (SceneRenderMode::Shaded), int (RenderQuality::Fast), -1, 0, -1.0f, 0, -1, 1, 1 },    // Technical
};
constexpr const char* kPreviewNames[] = { "Shaded", "Wireframe", "Architecture", "Rendered",
                                          "Clay",   "Technical", "Custom" };
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
           (knobs.background < 0 || state.environmentBackground == (knobs.background != 0)) &&
           (knobs.clay < 0 || state.clay == (knobs.clay != 0)) &&
           (knobs.edges < 0 || state.technicalEdges == (knobs.edges != 0));
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
    if (knobs.clay >= 0)
        state.clay = knobs.clay != 0;
    if (knobs.edges >= 0)
        state.technicalEdges = knobs.edges != 0;
}

void SettingsPage (Shell& shell, HudState& state, const DiligentSceneStats& scene)
{
    bool reset = false;
    hudshell::HudSettings (shell.fontStep, shell.placement, reset);
    graphicssettingsui::Draw ();

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
            ImGui::SetNextItemWidth (-FLT_MIN);
            hudshell::ColourChoice ("##plancutline", state.planCutRgba);
            ImGui::TextDisabled ("line colour");
            ImGui::SliderFloat ("line##plancut", &state.planCutWidthPixels, 1.0f, 6.0f, "%.1f px");
            ImGui::Checkbox ("fill the cut walls##plan", &state.planCutFill);
            if (state.planCutFill) {
                ImGui::SetNextItemWidth (-FLT_MIN);
                hudshell::ColourChoice ("##plancutfill", state.planCutFillRgba, true);
                ImGui::TextDisabled ("fill colour");
            }
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
        // ---- clay: one neutral material over every surface (ClayLook.hpp) --------------------
        ImGui::Checkbox ("clay##surfaces", &state.clay);
        hudshell::Tip ("Every surface in one neutral material -- the scheme reads by form, shadow and AO");
        if (state.clay) {
            ImGui::SetNextItemWidth (-FLT_MIN);
            hudshell::ColourChoice ("##claycolour", state.clayRgba);
            ImGui::TextDisabled ("clay colour");
            ImGui::SliderFloat ("roughness##clay", &state.clayRoughness, 0.0f, 1.0f, "%.2f");
            ImGui::Checkbox ("keep glass##clay", &state.clayKeepGlass);
            hudshell::Tip (state.clayKeepGlass ? "Panes stay translucent and lose only their tint"
                                               : "Panes turn to solid clay -- the massing reading");
        }
        // ---- technical edges: borders and creases, never the triangulation (TechnicalEdges.hpp)
        ImGui::Checkbox ("edges##technical", &state.technicalEdges);
        hudshell::Tip ("The edges that define the shape -- borders and creases sharper than the angle");
        if (state.technicalEdges) {
            ImGui::SetNextItemWidth (-FLT_MIN);
            hudshell::ColourChoice ("##edgecolour", state.technicalEdgeRgba);
            ImGui::TextDisabled ("edge colour");
            ImGui::SliderFloat ("width##edges", &state.technicalEdgePixels, 0.5f, 4.0f, "%.2f px");
            // The angle snaps to the tiers built with the model (kTechnicalEdgeAngles).
            const size_t tier = NearestCreaseTier (state.technicalEdgeAngle);
            char current[16];
            std::snprintf (current, sizeof (current), "%.0f\xC2\xB0", kCreaseTierDegrees[tier]);
            if (ImGui::BeginCombo ("angle##edges", current)) {
                for (size_t k = 0; k < kTechnicalEdgeAngleCount; ++k) {
                    char label[16];
                    std::snprintf (label, sizeof (label), "%.0f\xC2\xB0", kTechnicalEdgeAngles[k]);
                    if (ImGui::Selectable (label, NearestCreaseTier (kTechnicalEdgeAngles[k]) == tier))
                        state.technicalEdgeAngle = kTechnicalEdgeAngles[k];
                }
                ImGui::EndCombo ();
            }
            hudshell::Tip ("Faces meeting at more than this angle draw their edge; flatter ones merge");
            ImGui::Checkbox ("hidden edges##technical", &state.technicalEdgeHidden);
            hudshell::Tip ("Show the edges behind surfaces, faint -- the hidden-line reading");
        }
    }
    // ---- section: one movable cut through the model (SectionPlane.hpp) ---------------------
    if (hudshell::Section ("section")) {
        ImGui::Checkbox ("cut##section", &state.sectionEnabled);
        hudshell::Tip ("Remove everything beyond a plane -- image, shadows, AO and picking alike");
        if (state.sectionEnabled) {
            for (int a = 0; a < 3; ++a) {
                if (a > 0)
                    ImGui::SameLine ();
                char label[16];
                std::snprintf (label, sizeof (label), "%s##sectionaxis", SectionAxisName (SectionAxis (a)));
                ImGui::RadioButton (label, &state.sectionAxis, a);
            }
            ImGui::SameLine ();
            ImGui::TextDisabled (state.sectionAxis == 2 ? "horizontal" : "vertical");
            ImGui::SetNextItemWidth (-FLT_MIN);
            float percent = state.sectionPosition * 100.0f;
            if (ImGui::SliderFloat ("##sectionposition", &percent, 0.0f, 100.0f, "%.1f%% of the model"))
                state.sectionPosition = percent / 100.0f;
            ImGui::Checkbox ("flip##section", &state.sectionFlipped);
            hudshell::Tip (state.sectionFlipped ? "Keeping the high side" : "Keeping the low side");
            ImGui::SameLine ();
            if (ImGui::Button ("reset##section")) {
                state.sectionPosition = 0.5f;
                state.sectionFlipped = false;
            }
            ImGui::TextDisabled ("open cut: no cap fill yet");
        }
    }
    // ---- 3. environment; 4. sun and shadows; 5. colour and post ------------------------------
    DrawEnvironmentControls (state, scene);
    DrawLightInspector (state, scene);
    DrawShadowSettings (state, scene);
    DrawPostProcessingControls (state, scene);
    // ---- 6. camera -------------------------------------------------------------------------
    if (hudshell::Section ("camera")) {
        // A plan is parallel while it is held to the plan; freed, it can be turned to perspective.
        // ⚠️ ONE SOURCE OF TRUTH: both buttons write `state.orthographic` and nothing else; the frame
        // loop applies it (ViewerPlanMode.cpp `FollowProjectionToggle`). The named views raise a
        // one-shot the same loop consumes (`state.viewPreset`).
        const bool heldToPlan = state.planMode && !state.planOrbit;
        ImGui::BeginDisabled (heldToPlan);
        if (ImGui::RadioButton ("Perspective", !state.orthographic))
            state.orthographic = false;
        ImGui::SameLine ();
        if (ImGui::RadioButton ("Orthographic", state.orthographic))
            state.orthographic = true;
        hudshell::Tip ("Switches the projection only: the direction, the target and the size on screen are kept");
        ImGui::SetNextItemWidth (-FLT_MIN);
        if (ImGui::BeginCombo ("##viewpreset", "Axonometric view...")) {
            for (int k = 1; k < int (ViewPreset::Count); ++k)
                if (ImGui::Selectable (ViewPresetName (ViewPreset (k))))
                    state.viewPreset = k;
            ImGui::EndCombo ();
        }
        hudshell::Tip ("Orthographic from a named direction, framed on the selection or the model.\n"
                       "Isometric: all axes alike. Dimetric: width, height, depth 1 : 1 : 1/2.\n"
                       "Trimetric: 1 : 0.9 : 0.5. Directions follow project north.");
        ImGui::EndDisabled ();
        if (heldToPlan)
            ImGui::TextDisabled ("held to the plan -- free the orbit to change the view");
        else if (const char* note = ViewPresetNote (ViewPresetOutcome (state.viewPresetOutcome)))
            ImGui::TextDisabled ("%s", note);
        ImGui::Checkbox ("smooth navigation", &state.navSmoothing);
        hudshell::Tip ("Spreads each drag step over ~25 ms, so orbits and diagonal pans glide instead of stepping");
        ImGui::TextDisabled ("orbits about: %s", OrbitAnchorName (OrbitAnchorSource (state.orbitAnchor)));
        hudshell::Tip ("The selection's middle when something is selected, else the middle of the scene's box");
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
