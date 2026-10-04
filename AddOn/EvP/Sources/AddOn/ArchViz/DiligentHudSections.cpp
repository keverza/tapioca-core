// ArchViz/DiligentHudSections -- the viewer HUD's sections (DiligentHudSections.hpp): moved
// out of DiligentHud.cpp whole, each the block it was, when the HUD's tabs arrived.

#include "ArchViz/DiligentHudSections.hpp"

#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/HudShell.hpp" // ColourChoice
#include "ArchViz/InputRingBuffer.hpp"

#include <imgui.h>

#include <cmath>
#include <cstdint>

namespace geomsrv {
namespace archviz {

namespace {

// The callout's offset from the cursor, in pixels. ⚠️ IT IS DOWN AND TO THE
// RIGHT SO IT DOES NOT COVER WHAT IS BEING POINTED AT, and it is flipped near
// the edges below -- a tooltip that runs off the surface is one the user has to
// move the model to read, which is exactly the wrong trade for an overlay.
//
// ⚠️ 18 PX WAS NOT ENOUGH AND THE CALLOUT SAT UNDER THE POINTER. A Windows arrow
// cursor is 32x32 with its hot-spot at the top-left, so an 18 px offset puts the
// callout's corner INSIDE the cursor's own bitmap -- the pointer then overlaps
// the first line of text and, worse, covers the very geometry the callout is
// describing. The offset has to clear the cursor bitmap, not merely the hot-spot.
constexpr float kCalloutOffsetX = 34.0f;
constexpr float kCalloutOffsetY = 34.0f;

} // namespace

void DrawEnvironmentControls (HudState& state, const DiligentSceneStats& scene)
{
    // ---- Environment ------------------------------------------------
    // The same grouping `Commands/ModelViewer` uses in its three.js
    // panel: the sky's strength, its orientation, whether it is drawn,
    // and how much sun survives beside it. Collapsed by default -- the
    // controls above are the ones reached every session, and these
    // matter only once an HDR is loaded.
    if (ImGui::CollapsingHeader ("environment")) {
        ImGui::Checkbox ("sky lights the model", &state.environmentEnabled);
        ImGui::Checkbox ("draw the sky behind the model", &state.environmentBackground);

        ImGui::SetNextItemWidth (-1.0f);
        ImGui::SliderFloat ("##envintensity", &state.environmentIntensity, 0.0f, 3.0f, "%.2f");
        ImGui::TextDisabled ("sky intensity");

        // ⚠️ THE ROTATION IS THE CONVENTION TEST, not a styling knob.
        // Turn it 90 degrees: the LIT SIDE of the building must turn
        // with it. If the reflection moves and the diffuse does not,
        // the equirect lookup and the SH disagree about direction --
        // which is invisible on a still and unexplainable without this.
        ImGui::SetNextItemWidth (-1.0f);
        ImGui::SliderFloat ("##envrotation", &state.environmentRotationDegrees, -180.0f, 180.0f, "%.0f deg");
        ImGui::TextDisabled ("sky rotation -- turn it 90 deg, the lit side must follow");

        ImGui::SetNextItemWidth (-1.0f);
        ImGui::SliderFloat ("##sunwithsky", &state.sunWithSkyWeight, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled ("sun beside the sky -- lower it if the result is too bright");

        // ---- RE51.B6 -------------------------------------------------
        //
        // ⚠️ NOTHING ON SCREEN SEPARATES A GGX-PREFILTERED MIP CHAIN
        // FROM A BOX-FILTERED ONE. Both are "blurrier at higher
        // roughness"; the difference is whether a polished surface
        // reflects a recognisable environment or a smear, and that is a
        // judgement, not an observation. So the state is printed. If it
        // says box-filtered, the reason is the prefilter pipeline
        // failing to build and the message names it.
        if (scene.environmentLoaded) {
            if (scene.environmentPrefiltered)
                ImGui::TextDisabled ("  GGX-prefiltered: %u mips in %.1f ms", scene.environmentPrefilteredMips,
                                     scene.environmentPrefilterMs);
            else
                ImGui::TextColored (ImVec4 (1.0f, 0.8f, 0.2f, 1.0f), "  box-filtered (mirrors will smear): %s",
                                    scene.environmentPrefilterError.c_str ());
        }
    }
}

void DrawPostProcessingControls (HudState& state, const DiligentSceneStats& scene)
{
    // ---- Materials & grading ----------------------------------------
    //
    // ⚠️ ONLY `realistic` READS THESE. Shown regardless rather than
    // hidden on `fast`, because a control that vanishes reads as a bug;
    // the note below says which switch turns them on.
    if (ImGui::CollapsingHeader ("post processing")) {
        if (state.renderQuality != int (RenderQuality::Realistic))
            ImGui::TextDisabled ("(quality is `fast` -- these apply to `realistic`)");

        ImGui::SetNextItemWidth (-1.0f);
        ImGui::SliderFloat ("##reflectance", &state.reflectance, 0.0f, 8.0f, "%.2f");
        ImGui::TextDisabled ("reflectance -- 1 is physical; RAISE IT to make glass read as glass");

        ImGui::SetNextItemWidth (-1.0f);
        ImGui::SliderFloat ("##roughnessbias", &state.roughnessBias, -1.0f, 1.0f, "%+.2f");
        ImGui::TextDisabled ("roughness bias -- negative is glossier; this pool is ~0.99 matte");

        ImGui::SetNextItemWidth (-1.0f);
        ImGui::SliderFloat ("##exposure", &state.exposure, 0.05f, 3.0f, "%.2f");
        ImGui::TextDisabled ("exposure into the tone curve");

        // ---- RE51.B9 ------------------------------------------------
        //
        // ⚠️ THE ESTIMATE IS PRINTED WHETHER OR NOT IT IS APPLIED, and
        // that is the whole point of shipping the checkbox off. The auto
        // exposure has one calibration constant and no live measurement
        // behind it; showing what it WOULD choose beside the fixed value
        // turns the first run into the measurement instead of into a
        // surprise. If the two are close, turn it on and delete this
        // note; if they are not, the ratio is the correction.
        // ---- RE51.C3 ------------------------------------------------
        ImGui::Checkbox ("ambient occlusion", &state.ambientOcclusion);
        ImGui::TextDisabled ("  contact darkening -- costs a SECOND geometry pass");
        ImGui::SetNextItemWidth (-1.0f);
        ImGui::SliderFloat ("##aointensity", &state.ambientOcclusionIntensity, 0.0f, 2.0f, "%.2f");
        ImGui::TextDisabled ("AO amount -- separate from the effect's own radius");

        // ⚠️ THE RADIUS IS THE INSTRUMENT FOR THE ONE OPEN AO QUESTION.
        // The first live run reported "AO darkens whole scene, but soft
        // contact shadow is not visible", and a radius too small for
        // the model is the leading explanation. Sweeping this settles
        // it: if a larger radius produces recognisable contact
        // darkening, that was the whole story; if the image only dims
        // further at every setting, the fault is in the AO's INPUTS
        // (depth or normals) and debug view 9 is the next look.
        ImGui::SetNextItemWidth (-1.0f);
        ImGui::SliderFloat ("##aoradius", &state.ambientOcclusionRadius, 0.0f, 20.0f, "%.2f m");
        ImGui::TextDisabled ("AO radius -- 0 derives it from the model; now %.2f m", scene.aoRadiusMetres);

        ImGui::Checkbox ("epipolar atmosphere", &state.epipolarAtmosphere);
        ImGui::TextDisabled ("  physical aerial perspective -- needs Realistic quality");
        ImGui::SetNextItemWidth (-1.0f);
        ImGui::SliderFloat ("##atmosphereintensity", &state.atmosphereIntensity, 0.0f, 20.0f, "%.1f");
        ImGui::TextDisabled ("extraterrestrial sun intensity");
        ImGui::Checkbox ("atmospheric light shafts", &state.atmosphereLightShafts);
        ImGui::Checkbox ("atmospheric lighting only", &state.atmosphereLightingOnly);

        // ---- RE51.C7: screen-space reflections ----------------------
        ImGui::Checkbox ("screen-space reflections", &state.screenSpaceReflection);
        ImGui::TextDisabled ("  neighbouring-object reflections -- needs Realistic quality");
        ImGui::SetNextItemWidth (-1.0f);
        ImGui::SliderFloat ("##ssrintensity", &state.ssrIntensity, 0.0f, 2.0f, "%.2f");
        ImGui::TextDisabled ("SSR amount -- how much of the reflection to show");
        ImGui::SetNextItemWidth (-1.0f);
        ImGui::SliderFloat ("##ssrroughness", &state.ssrRoughnessThreshold, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled ("SSR roughness threshold -- surfaces rougher than this get no rays");
        // ⚠️ "colour: NO" WHILE SSR IS ON IS THE CONVERGENCE FAULT.
        // It means every reflection is sampling the CURRENT frame, so
        // the effect restarts from scratch each frame and the jitter
        // never settles -- and the picture cannot show it, because
        // reflections appear either way.
        if (state.screenSpaceReflection) {
            ImGui::TextDisabled ("SSR history -- depth: %s, colour: %s", scene.ssrDepthHistory ? "yes" : "NO",
                                 scene.ssrColorHistory ? "yes" : "NO");
        }

        // ---- RE51.C8: temporal anti-aliasing ------------------------
        ImGui::Checkbox ("temporal anti-aliasing", &state.temporalAntiAliasing);
        ImGui::TextDisabled ("  HDR history accumulation -- needs Realistic quality");
        ImGui::SetNextItemWidth (-1.0f);
        ImGui::SliderFloat ("##taastability", &state.taaStability, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled ("TAA stability -- higher is steadier; lower rejects history faster");
        // ⚠️ THE ONE READOUT THAT SEPARATES "TAA IS WEAK" FROM "TAA
        // NEVER RAN". Draw falls back to the raw JITTERED target when
        // the TAA pass produces nothing, so that failure does not look
        // like a missing effect -- it looks like the image shakes. If
        // this says the jitter is non-zero and TAA is NOT resolving,
        // stop adjusting the slider above: nothing it does can reach
        // the screen.
        if (state.temporalAntiAliasing) {
            ImGui::TextDisabled ("TAA jitter %+.2f, %+.2f px -- resolved: %s", scene.taaJitterPixels[0],
                                 scene.taaJitterPixels[1], scene.taaResolved ? "yes" : "NO");
        }

        ImGui::Checkbox ("auto exposure", &state.autoExposure);
        ImGui::TextDisabled ("  auto would pick %.2f (scene luminance %.4f, albedo %.3f)", scene.autoExposure,
                             scene.sceneLuminance, scene.meanAlbedo);

        ImGui::SetNextItemWidth (-1.0f);
        ImGui::SliderFloat ("##whitebalance", &state.whiteBalanceKelvin, 2000.0f, 12000.0f, "%.0f K");
        ImGui::TextDisabled ("white balance -- the light being CORRECTED FOR; 6500 K is neutral");

        ImGui::SetNextItemWidth (-1.0f);
        ImGui::SliderFloat ("##tint", &state.whiteBalanceTint, -1.0f, 1.0f, "%+.2f");
        ImGui::TextDisabled ("tint -- negative green, positive magenta; gains %.2f %.2f %.2f",
                             scene.whiteBalanceGains[0], scene.whiteBalanceGains[1], scene.whiteBalanceGains[2]);
    }
}

void DrawStorySliceControls (HudState& state, const DiligentSceneStats& scene)
{
    // ---- the storey section overlay ----------------------------
    // Live now: the storeys are read in the extraction pass's acquire
    // slice, every element is cut against each level, and the union is
    // drawn by StorySliceLayer.
    //
    // ⚠️ TURNING IT ON ASKS FOR A REFRESH, and the widget has to say so.
    // The cut runs during a FULL pass and only when it was requested
    // before that pass began -- a union over the elements a pass happened
    // to reach is a clean outline of part of a building. So the first tick
    // shows nothing until the refresh lands, and a checkbox that appears
    // to do nothing for several seconds is indistinguishable from a broken
    // one unless it explains itself.
    const bool sliceWasOn = state.showStorySlices;
    ImGui::Checkbox ("story slices", &state.showStorySlices);
    if (state.showStorySlices && !sliceWasOn && scene.storeySlices == 0)
        state.storySlicesNeedRefresh = true;
    if (state.showStorySlices) {
        ImGui::Indent ();
        if (!scene.storeySliceLayerReady) {
            ImGui::TextColored (ImVec4 (1.0f, 0.4f, 0.3f, 1.0f), "the slice layer failed to create -- see archviz.log");
        }
        else if (scene.storeySlices == 0) {
            ImGui::TextColored (ImVec4 (1.0f, 0.8f, 0.2f, 1.0f), "no storey set yet -- refresh to cut the model");
        }
        else {
            ImGui::TextDisabled ("%llu storey(s), %.0f m2 enclosed", (unsigned long long) scene.storeySlices,
                                 scene.storeySliceAreaM2);
        }

        // Their colours, from a few (the user, 2026-10-03: style controls for the displays).
        ImGui::SetNextItemWidth (-1.0f);
        hudshell::ColourChoice ("##slicecolour", state.storySliceRgba);
        ImGui::TextDisabled ("line colour");
        ImGui::Checkbox ("fill the contour", &state.storySliceFill);
        if (state.storySliceFill) {
            ImGui::SetNextItemWidth (-1.0f);
            hudshell::ColourChoice ("##slicefill", state.storySliceFillRgba, true);
            ImGui::TextDisabled ("fill colour");
        }

        // ⚠️ THREE STATES, NOT A "HIDE BEHIND GEOMETRY" BOOL. They answer
        // different questions: hidden reads the storey as a plan, dashed
        // is the drafting convention for buried linework, and solid is the
        // register check against something else.
        int occluded = int (state.storySliceOccluded);
        ImGui::SetNextItemWidth (-1.0f);
        static const char* const kOccludedNames[] = { "hidden", "dashed", "solid" };
        ImGui::Combo ("##sliceoccluded", &occluded, kOccludedNames, IM_ARRAYSIZE (kOccludedNames));
        state.storySliceOccluded = SliceOccludedStyle (occluded);
        ImGui::TextDisabled ("behind geometry");

        ImGui::SetNextItemWidth (-1.0f);
        ImGui::SliderFloat ("##slicewidth", &state.storySliceWidthPixels, 1.0f, 8.0f, "%.1f px");
        ImGui::TextDisabled ("line width -- PIXELS, so it holds at every zoom");

        if (state.storySliceOccluded == SliceOccludedStyle::Dashed) {
            ImGui::SetNextItemWidth (-1.0f);
            ImGui::SliderFloat ("##slicedash", &state.storySliceDashPixels, 2.0f, 40.0f, "%.0f px");
            ImGui::TextDisabled ("dash period");
        }
        ImGui::Unindent ();
    }
}

void DrawGhPreviewControls (HudState& state)
{
    // ---- what a Grasshopper definition asked Archicad to show ---
    //
    // ⚠️ THE COUNTS ARE THE PANEL'S REASON TO EXIST, NOT THE CHECKBOX.
    // An empty viewport means one of four different things -- preview
    // switched off, nothing sent, everything of a kind Tapioca does not
    // draw yet, or text with nowhere to go -- and they are one picture.
    // Without these numbers the only way to tell them apart is to read
    // grasshopper.log and guess.
    ImGui::Checkbox ("grasshopper preview", &state.showGhPreview);
    if (state.showGhPreview) {
        ImGui::Indent ();
        const bool anyGeometry = state.ghPreviewMeshIndices > 0 || state.ghPreviewLineVertices > 0;
        if (!anyGeometry && state.ghPreviewDeferredKinds == 0 && state.ghPreviewLabels == 0) {
            ImGui::TextColored (ImVec4 (1.0f, 0.8f, 0.2f, 1.0f),
                                "nothing received -- run a definition with a Tapioca Preview component");
        }
        else {
            ImGui::TextDisabled ("%llu triangle(s), %llu curve segment(s)",
                                 (unsigned long long) (state.ghPreviewMeshIndices / 3),
                                 (unsigned long long) (state.ghPreviewLineVertices / 6));
        }
        // Not drawn, and saying so beats showing nothing and explaining
        // nothing: this renderer has no text pass yet.
        if (state.ghPreviewLabels > 0)
            ImGui::TextColored (ImVec4 (1.0f, 0.8f, 0.2f, 1.0f),
                                "%llu label(s) received -- Tapioca cannot draw text yet",
                                (unsigned long long) state.ghPreviewLabels);
        if (state.ghPreviewDeferredKinds > 0)
            ImGui::TextColored (ImVec4 (1.0f, 0.8f, 0.2f, 1.0f), "%llu point/plane/arrow/bounds not drawn yet",
                                (unsigned long long) state.ghPreviewDeferredKinds);
        if (state.ghPreviewTruncated)
            ImGui::TextColored (ImVec4 (1.0f, 0.4f, 0.3f, 1.0f),
                                "TRUNCATED at the drawable ceiling -- this is not the whole result");

        ImGui::SetNextItemWidth (-1.0f);
        ImGui::SliderFloat ("##ghpreviewwidth", &state.ghPreviewWidthPixels, 1.0f, 8.0f, "%.1f px");
        ImGui::TextDisabled ("curve width -- PIXELS, so it holds at every zoom");

        ImGui::SetNextItemWidth (-1.0f);
        ImGui::SliderFloat ("##ghpreviewambient", &state.ghPreviewAmbient, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled ("ambient -- the shading is legibility, not the scene's sun");

        // ---- PLAT-RE151 -------------------------------------------
        // ⚠️ THE CAVEAT IS PART OF THE CONTROL, not decoration under it.
        // The occlusion is reconstructed from the EXTRACTED model and
        // the SYNCED camera, never from Archicad's own depth buffer --
        // which is not reachable. So the one failure a user will
        // actually see is the occluding edge lagging during a drag, and
        // a user who has been told where the depth comes from reads that
        // as the camera sync it is, rather than as the occlusion being
        // broken.
        ImGui::Checkbox ("occlude behind the building", &state.ghPreviewOcclusion);
        ImGui::TextDisabled ("depth from the EXTRACTED model, not from Archicad's own buffer:");
        ImGui::TextDisabled ("the edge lags a drag by exactly what the camera sync does");
        if (state.ghPreviewOcclusion && state.renderMode != int (SceneRenderMode::Wireframe))
            ImGui::TextDisabled ("(shaded modes occlude for free -- no prepass runs)");
        ImGui::Unindent ();
    }
}

void DrawLightInspector (HudState& state, const DiligentSceneStats& scene)
{
    // The requested C9 panels expose only values that reach a real renderer
    // subsystem. The overlay retains these diagnostics but no dead widgets.
    const bool showLightInspector = state.readOnly || ImGui::CollapsingHeader ("light inspector");
    if (showLightInspector) {
        ImGui::Text ("sun %s%s", scene.sunApplied ? "applied" : "DEFAULT (never arrived)",
                     scene.sunBelowHorizon ? ", below horizon" : "");
        ImGui::Text ("  dir %.2f %.2f %.2f   ambient %.2f", scene.sun[0], scene.sun[1], scene.sun[2], scene.ambient);
        // ⚠️ BOTH AZIMUTHS, BOTH LABELLED. Showing one unlabelled number in
        // [-180, 180] produced a live report of "Archicad says 240, the viewer
        // says -120" -- which is not necessarily a disagreement at all: -120 and
        // 240 are the same direction, and the model-space angle and the compass
        // bearing are different quantities that only coincide at north = 0.
        // Whichever of the two Archicad's dialog is showing, one of these lines
        // now matches it exactly, and the mismatch (if any) is a number rather
        // than an impression.
        ImGui::Text ("  model  %.1f deg (CCW from +X)   altitude %.1f deg", scene.sunAzimuthDegrees,
                     scene.sunAltitudeDegrees);
        ImGui::Text ("  compass %.1f deg (CW from north)   north %.1f deg", scene.sunBearingDegrees,
                     scene.northDegrees);
        // ⚠️ THE PLACE AND MOMENT, because a wrong sun has two very different
        // causes and they look identical on a building: the CONVERSION is wrong,
        // or the viewer is reading a sun the user never set. The angles above are
        // Archicad's STORED ones -- what its own 3D window shades with -- and
        // this line says what date/place they belong to. If the two lines below
        // disagree, the project's sun was TYPED into the Sun dialog rather than
        // computed from its date, which is ordinary and is not a bug in either
        // number.
        ImGui::TextDisabled ("  place %.4f, %.4f  alt %.0f m   %04u-%02u-%02u %02u:%02u%s", scene.latitudeDegrees,
                             scene.longitudeDegrees, scene.siteAltitudeMetres, scene.year, scene.month, scene.day,
                             scene.hour, scene.minute, scene.summerTime ? " DST" : "");
        if (scene.haveComputedSun) {
            const float azGap = std::abs (scene.computedAzimuthDegrees - scene.sunAzimuthDegrees);
            const float altGap = std::abs (scene.computedAltitudeDegrees - scene.sunAltitudeDegrees);
            if (!scene.sunOverridden && (azGap > 0.5f || altGap > 0.5f))
                ImGui::TextColored (ImVec4 (1.0f, 0.8f, 0.2f, 1.0f),
                                    "  that date implies %.1f / %.1f deg -- STORED sun used",
                                    scene.computedAzimuthDegrees, scene.computedAltitudeDegrees);
        }

        if (!state.readOnly) {
            ImGui::Checkbox ("override the sun", &state.sunOverride);
            ImGui::SameLine ();
            if (ImGui::Button ("match Archicad"))
                state.sunOverride = false;
            if (state.sunOverride) {
                ImGui::SliderFloat ("azimuth", &state.sunAzimuthDegrees, -180.0f, 180.0f, "%.1f deg");
                ImGui::SliderFloat ("altitude", &state.sunAltitudeDegrees, 0.0f, 90.0f, "%.1f deg");
                float overrideBearing = scene.northDegrees - state.sunAzimuthDegrees;
                overrideBearing -= 360.0f * std::floor (overrideBearing / 360.0f);
                ImGui::TextDisabled ("azimuth is CCW from +X (east) = compass %.1f deg", overrideBearing);
            }
        }
    }
}

void DrawShadowSettings (HudState& state, const DiligentSceneStats& scene)
{
    const bool showShadowSettings = state.readOnly || ImGui::CollapsingHeader ("shadow settings");
    if (showShadowSettings) {
        if (!state.readOnly) {
            ImGui::Checkbox ("cast shadows", &state.shadowsEnabled);
            const char* resolutions[] = { "512", "1024", "2048", "4096" };
            int resolutionIndex = state.shadowResolution <= 512
                                      ? 0
                                      : (state.shadowResolution <= 1024 ? 1 : (state.shadowResolution <= 2048 ? 2 : 3));
            if (ImGui::Combo ("resolution", &resolutionIndex, resolutions, 4))
                state.shadowResolution = 512 << resolutionIndex;
            ImGui::SliderInt ("cascades", &state.shadowCascades, 1, 8);
            const char* modes[] = { "PCF", "VSM", "EVSM2", "EVSM4" };
            int modeIndex = state.shadowMode - 1;
            if (ImGui::Combo ("mode", &modeIndex, modes, 4))
                state.shadowMode = modeIndex + 1;
            ImGui::SliderFloat ("partitioning", &state.shadowPartitioning, 0.0f, 1.0f, "%.3f");
            ImGui::SliderFloat ("filter size (m)", &state.shadowFilterWorldSize, 0.001f, 0.5f, "%.3f");
            if (state.shadowMode != int (DiligentShadowMode::Pcf)) {
                const int filterSizes[] = { 2, 3, 5, 7 };
                const char* filterLabels[] = { "2 x 2", "3 x 3", "5 x 5", "7 x 7" };
                int filterIndex = state.shadowFilterSize <= 2
                                      ? 0
                                      : (state.shadowFilterSize <= 3 ? 1 : (state.shadowFilterSize <= 5 ? 2 : 3));
                if (ImGui::Combo ("conversion filter", &filterIndex, filterLabels, 4))
                    state.shadowFilterSize = filterSizes[filterIndex];
            }
            if (state.shadowMode == int (DiligentShadowMode::Pcf)) {
                ImGui::Checkbox ("contact hardening (PCSS)", &state.shadowPcss);
                if (state.shadowPcss) {
                    ImGui::SliderFloat ("sun diameter", &state.shadowPcssLightAngularDiameter, 0.1f, 5.0f, "%.2f deg");
                    ImGui::SliderFloat ("blocker search (m)", &state.shadowPcssBlockerSearch, 0.1f, 10.0f, "%.2f",
                                        ImGuiSliderFlags_Logarithmic);
                    ImGui::SliderFloat ("max penumbra (m)", &state.shadowPcssMaxPenumbra, 0.05f, 5.0f, "%.2f",
                                        ImGuiSliderFlags_Logarithmic);
                }
                ImGui::SliderFloat ("depth bias", &state.shadowDepthBias, 0.00001f, 0.02f, "%.5f",
                                    ImGuiSliderFlags_Logarithmic);
                ImGui::SliderFloat ("receiver bias clamp", &state.shadowReceiverBiasClamp, 0.0f, 20.0f);
            }
            else {
                ImGui::SliderFloat ("bleed reduction", &state.shadowLightBleeding, 0.0f, 0.99f);
                ImGui::SliderFloat ("variance bias", &state.shadowVsmBias, 0.00001f, 0.1f, "%.5f",
                                    ImGuiSliderFlags_Logarithmic);
            }
            if (state.shadowMode >= int (DiligentShadowMode::Evsm2))
                ImGui::SliderFloat ("positive exponent", &state.shadowEvsmPositiveExponent, 0.1f, 40.0f);
            if (state.shadowMode == int (DiligentShadowMode::Evsm4))
                ImGui::SliderFloat ("negative exponent", &state.shadowEvsmNegativeExponent, 0.1f, 40.0f);
            ImGui::SliderFloat ("cascade transition", &state.shadowCascadeTransition, 0.0f, 0.5f);
            ImGui::Checkbox ("visualize cascades", &state.shadowVisualizeCascades);
            ImGui::Checkbox ("shadows only", &state.shadowOnly);
        }
        if (!state.shadowsEnabled)
            ImGui::TextDisabled ("shadow rendering disabled");
        else if (scene.shadowResolution == 0)
            ImGui::TextColored (ImVec4 (1.0f, 0.5f, 0.4f, 1.0f), "no shadow map (see archviz.log)");
        else if (!scene.shadowFitted)
            ImGui::TextColored (ImVec4 (1.0f, 0.8f, 0.2f, 1.0f), "shadow map %u, not fitted yet",
                                scene.shadowResolution);
        else
            ImGui::Text ("shadow %u x %u   texel %.3f m", scene.shadowResolution, scene.shadowCascades,
                         scene.shadowTexelMetres);
        const char* activeMode =
            scene.shadowMode >= 1 && scene.shadowMode <= 4
                ? (scene.shadowMode == 1
                       ? "PCF"
                       : (scene.shadowMode == 2 ? "VSM" : (scene.shadowMode == 3 ? "EVSM2" : "EVSM4")))
                : "unknown";
        ImGui::TextDisabled ("DiligentFX %s", activeMode);
    }
}

void DrawHoverCallout (const HudState& state, const InputSnapshot& input, uint32_t width, uint32_t height)
{
    // ---- the callout under the cursor (PLAT-RE43) --------------------------
    //
    // ⚠️ A PLAIN WINDOW, NOT ImGui::SetTooltip, AND THE DIFFERENCE MATTERS HERE.
    // A tooltip is anchored to ImGui's own idea of the mouse and is suppressed
    // while any item is hovered -- so it would vanish exactly when the cursor
    // crossed the panel, which over a transparent overlay is most of the screen.
    // A positioned, non-interactive window is placed where WE say and never
    // takes the mouse, which is the whole requirement: the callout must not make
    // the overlay stop being click-through.
    //
    // ⚠️ NoInputs IS LOAD-BEARING, NOT TIDINESS. Without it the callout counts as
    // a hovered ImGui item, `WantCaptureMouse` goes true wherever it sits, and
    // the camera and the pick both stop responding under the very thing that
    // follows the cursor around.
    // ⚠️ `hover.valid` IS NO LONGER REQUIRED. The callout now carries the CURSOR
    // COORDINATE as well as the hovered element, and a coordinate is a real
    // answer over empty space -- which is exactly where a massing study wants to
    // read one. Requiring an element would blank the readout over the ground
    // plane, the most useful place to have it.
    if (state.showCallout && input.inside) {
        const DiligentScene::ElementInfo& info = state.hover;

        // Flip to the other side near an edge rather than letting the callout run
        // off the surface. 260/150 is a generous estimate of its size; being
        // approximate costs a few pixels of margin and nothing else.
        float x = float (input.x) + kCalloutOffsetX;
        float y = float (input.y) + kCalloutOffsetY;
        if (x + 260.0f > float (width))
            x = float (input.x) - 260.0f - kCalloutOffsetX;
        if (y + 150.0f > float (height))
            y = float (input.y) - 150.0f - kCalloutOffsetY;
        ImGui::SetNextWindowPos (ImVec2 (x < 0.0f ? 0.0f : x, y < 0.0f ? 0.0f : y));
        ImGui::SetNextWindowBgAlpha (0.82f);
        const ImGuiWindowFlags calloutFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                              ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                              ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin ("##callout", nullptr, calloutFlags)) {
            // ---- the coordinate, first --------------------------------------
            ImGui::Text ("cursor %d, %d px", state.cursorX, state.cursorY);
            if (state.cursorGroundValid) {
                // ⚠️ LABELLED "on z=0", BECAUSE THAT IS WHAT IT IS. This is where
                // the view ray meets the GROUND PLANE, not the surface under the
                // cursor -- the surface point would need a depth readback per
                // hover on the path that already throttles itself to keep one
                // readback from delaying a click. Naming it precisely is the
                // difference between a useful number and a wrong one.
                ImGui::Text ("world %.3f, %.3f  (on z=0)", state.cursorGround[0], state.cursorGround[1]);
            }
            else {
                ImGui::TextDisabled ("world -- (ray misses z=0)");
            }

            ImGui::Separator ();
            if (!info.valid) {
                ImGui::TextDisabled ("no element under the cursor");
            }
            else {
                const float sizeX = info.boundsMax[0] - info.boundsMin[0];
                const float sizeY = info.boundsMax[1] - info.boundsMin[1];
                const float sizeZ = info.boundsMax[2] - info.boundsMin[2];
                // ⚠️ HEIGHT AND ELEVATION ARE BOTH SHOWN, and they are different
                // questions: "how tall is this" and "where does it sit". A user
                // comparing the viewer against Archicad's own element settings
                // needs the second at least as often as the first, and deriving
                // it from a height alone is impossible.
                ImGui::Text ("height %.3f m", sizeZ);
                ImGui::Text ("elevation %.3f .. %.3f m", info.boundsMin[2], info.boundsMax[2]);
                ImGui::Text ("footprint %.3f x %.3f m", sizeX, sizeY);
                ImGui::Separator ();
                ImGui::Text ("%llu triangles, %llu vertices", (unsigned long long) info.triangles,
                             (unsigned long long) info.vertices);
                ImGui::Text ("%llu material range%s%s", (unsigned long long) info.materialRanges,
                             info.materialRanges == 1 ? "" : "s", info.hasTransparency ? ", some transparent" : "");
                if (info.selected)
                    ImGui::TextColored (ImVec4 (0.2f, 0.9f, 1.0f, 1.0f), "SELECTED");
                // ⚠️ THE GUID LAST AND DIMMED. It is the only thing here a user
                // can paste into a script, so it must be present -- and it is
                // also the least readable line, so it must not be the first
                // thing the eye lands on when the point is the height.
                ImGui::TextDisabled ("%s", info.guid.c_str ());
            }
        }
        ImGui::End ();
    }
}

void DrawSelectedElementWindow (const HudState& state, uint32_t height)
{
    // ---- the selected element's properties ---------------------------------
    //
    // ⚠️ THIS PANEL IS WHY PICKING EXISTS HERE. The viewer never writes a
    // selection back to Archicad (the panel arms selectionbridge::ToViewer only),
    // so a click's entire product is this readout -- inspection, not editing.
    //
    // ⚠️ EVERY FIGURE BELOW IS DERIVED FROM THE EXTRACTED MESH, and the panel
    // says so. Archicad's own quantities (its computed surface area, volume,
    // element type, layer, ID) are NOT here yet: they need an ACAPI read on the
    // main thread keyed by the picked guid, which is a different mechanism from
    // anything the render thread can reach. Labelling these as bounding-box
    // figures is the difference between a useful approximation and a number a
    // user would put in a schedule believing Archicad had said it.
    if (state.showProperties && state.selected.valid) {
        const DiligentScene::ElementInfo& sel = state.selected;
        ImGui::SetNextWindowPos (ImVec2 (12.0f, float (height) - 12.0f), ImGuiCond_FirstUseEver, ImVec2 (0.0f, 1.0f));
        ImGui::SetNextWindowSize (ImVec2 (300.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGuiWindowFlags propFlags = ImGuiWindowFlags_AlwaysAutoResize;
        if (state.readOnly)
            propFlags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
        if (ImGui::Begin ("Selected element", nullptr, propFlags)) {
            const float sizeX = sel.boundsMax[0] - sel.boundsMin[0];
            const float sizeY = sel.boundsMax[1] - sel.boundsMin[1];
            const float sizeZ = sel.boundsMax[2] - sel.boundsMin[2];

            ImGui::Text ("footprint %.3f x %.3f m", sizeX, sizeY);
            // The bounding-box footprint area -- the "area" a massing study wants
            // at this stage. ⚠️ NOT Archicad's computed area: for anything that
            // is not a rectangular block in plan these differ, and for a rotated
            // wall they differ a lot, because the box is axis-aligned.
            ImGui::Text ("footprint area %.3f m2  (bbox)", sizeX * sizeY);
            ImGui::Text ("height %.3f m", sizeZ);
            ImGui::Text ("bbox volume %.3f m3", sizeX * sizeY * sizeZ);
            ImGui::Text ("elevation %.3f .. %.3f m", sel.boundsMin[2], sel.boundsMax[2]);
            ImGui::TextDisabled ("axis-aligned bounding box, from the mesh --");
            ImGui::TextDisabled ("not Archicad's own computed quantities");

            ImGui::Separator ();
            ImGui::Text ("%llu triangles, %llu vertices", (unsigned long long) sel.triangles,
                         (unsigned long long) sel.vertices);
            ImGui::Text ("%llu material range%s%s", (unsigned long long) sel.materialRanges,
                         sel.materialRanges == 1 ? "" : "s", sel.hasTransparency ? ", some transparent" : "");
            ImGui::TextDisabled ("%s", sel.guid.c_str ());
        }
        ImGui::End ();
    }
}

} // namespace archviz
} // namespace geomsrv
