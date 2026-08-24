#include "ArchViz/DiligentHud.hpp"

#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/DiligentShaders.hpp"
#include "ArchViz/InputRingBuffer.hpp"

#include <windows.h>
#include <d3d11.h> // Must precede any Diligent D3D11 interop header (Probe 1a).

#include <imgui.h>

#include <ImGuiDiligentRenderer.hpp>
#include <ImGuiImplDiligent.hpp> // for ImGuiDiligentCreateInfo

#include <DeviceContext.h>
#include <RenderDevice.h>
#include <SwapChain.h>

#include <cmath>
#include <memory>

namespace geomsrv {
namespace archviz {

namespace {

// The debug views, by the names the user already knows from the command's
// `debug_view` parameter. ⚠️ THE ORDER IS THE DiligentDebugView ENUM'S ORDER --
// the combo's index IS the value, so an insertion here silently renumbers them.
const char* const kDebugViewNames[] = {
    "final",
    "normals",
    "lit",
    "base color",
    "sun vector",
    "shadow",
    "roughness",
    "G-buffer normals",
    "G-buffer depth",
    "ambient occlusion",
    "G-buffer albedo",
    "G-buffer roughness",
    "G-buffer material",
    "motion vectors",
};
constexpr int kDebugViewCount = int (sizeof (kDebugViewNames) / sizeof (kDebugViewNames[0]));

static_assert (kDebugViewCount == int (DiligentDebugView::MotionVectors) + 1,
               "the HUD's combo and DiligentDebugView have drifted apart");

// The render modes, in SceneRenderMode's order -- the combo's index IS the
// value, exactly as for the debug views above.
const char* const kRenderModeNames[] = { "shaded", "wireframe", "shaded + wireframe" };
constexpr int kRenderModeCount = int (sizeof (kRenderModeNames) / sizeof (kRenderModeNames[0]));

static_assert (kRenderModeCount == int (SceneRenderMode::ShadedWireframe) + 1,
               "the HUD's combo and SceneRenderMode have drifted apart");

// The render qualities, in RenderQuality's order -- same rule again: the combo's
// index IS the enum value.
const char* const kRenderQualityNames[] = { "fast", "realistic" };
constexpr int kRenderQualityCount = int (sizeof (kRenderQualityNames) / sizeof (kRenderQualityNames[0]));

static_assert (kRenderQualityCount == int (RenderQuality::Realistic) + 1,
               "the HUD's combo and RenderQuality have drifted apart");

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

// ImGui's mouse button numbering, against InputRingBuffer's bitmask.
int ToImGuiButton (uint8_t button)
{
    if (button == kMouseLeft)
        return 0;
    if (button == kMouseRight)
        return 1;
    return -1;
}

} // namespace

struct DiligentHud::Impl {
    ImGuiContext* context = nullptr;
    std::unique_ptr<Diligent::ImGuiDiligentRenderer> renderer;
    bool ready = false;
    double lastTimeSeconds = 0.0;

    // ---- the frame-cost badge ----------------------------------------------
    // ⚠️ THE WORST FRAME MATTERS MORE THAN THE AVERAGE, and an average is what an
    // fps counter shows. A viewer that runs at 60 fps and stalls for 200 ms when
    // an extraction batch lands has taken 200 ms out of ARCHICAD's UI thread's
    // neighbourhood, and the mean over the same second still reads ~55 fps. So
    // the badge carries both, and the peak is held long enough to be read.
    float worstMsInWindow = 0.0f;
    float heldWorstMs = 0.0f;
    double heldUntilSeconds = 0.0;
};

namespace {

// How long a spike stays on the badge after it happens. Long enough to notice
// and read, short enough that the number still describes the recent past.
constexpr double kWorstHoldSeconds = 3.0;

// Above this, a frame is slow enough that the user would feel it. 16.7 ms is one
// vsynced frame; a viewer that misses it occasionally is fine, one that misses it
// by a lot is the thing this badge exists to catch.
constexpr float kSlowFrameMs = 33.0f;

} // namespace

DiligentHud::DiligentHud () : impl_ (new Impl ())
{
}
DiligentHud::~DiligentHud ()
{
    Shutdown ();
    delete impl_;
}

bool DiligentHud::IsReady () const
{
    return impl_ != nullptr && impl_->ready;
}

bool DiligentHud::Init (Diligent::IRenderDevice* device, uint32_t colorBufferFormat, uint32_t depthBufferFormat,
                        std::string& error)
{
    if (device == nullptr) {
        error = "DiligentHud::Init got no device";
        return false;
    }
    if (impl_->ready)
        return true;

    // ⚠️ AN EXPLICIT CONTEXT, NOT THE IMPLICIT GLOBAL. ImGui's default context is
    // a process-wide singleton, and the bgfx viewer creates one of its own. Two
    // renderers sharing one context would share one set of windows and one set
    // of GPU buffers owned by whichever initialised last.
    impl_->context = ImGui::CreateContext ();
    if (impl_->context == nullptr) {
        error = "ImGui::CreateContext returned nothing";
        return false;
    }
    ImGui::SetCurrentContext (impl_->context);

    ImGuiIO& io = ImGui::GetIO ();
    // No .ini and no .log next to the .apx. `evp.paths` owns where files go, and
    // ImGui would otherwise drop imgui.ini into Archicad's working directory --
    // which is inside Program Files.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImGui::StyleColorsDark ();

    // ⚠️ THE FORMAT PAIR IS SET BY HAND rather than through the
    // SwapChainDesc constructor, because the overlay has no swap chain to ask.
    // The PSO records the formats it renders into, so a mismatch here is a
    // creation-time failure rather than a draw-time surprise.
    Diligent::ImGuiDiligentCreateInfo ci;
    ci.pDevice = device;
    ci.BackBufferFmt = static_cast<Diligent::TEXTURE_FORMAT> (colorBufferFormat);
    ci.DepthBufferFmt = static_cast<Diligent::TEXTURE_FORMAT> (depthBufferFormat);
    try {
        impl_->renderer = std::make_unique<Diligent::ImGuiDiligentRenderer> (ci);
    }
    catch (const std::exception& ex) {
        error = std::string ("ImGuiDiligentRenderer construction failed: ") + ex.what ();
        ImGui::DestroyContext (impl_->context);
        impl_->context = nullptr;
        return false;
    }

    impl_->ready = true;
    return true;
}

void DiligentHud::Shutdown ()
{
    if (impl_ == nullptr)
        return;
    impl_->renderer.reset ();
    if (impl_->context != nullptr) {
        ImGui::SetCurrentContext (impl_->context);
        ImGui::DestroyContext (impl_->context);
        impl_->context = nullptr;
    }
    impl_->ready = false;
}

void DiligentHud::Draw (Diligent::IDeviceContext* context, uint32_t width, uint32_t height, const InputSnapshot& input,
                        const DiligentSceneStats& scene, HudState& state)
{
    if (context == nullptr || !impl_->ready || width == 0 || height == 0)
        return;

    ImGui::SetCurrentContext (impl_->context);
    ImGuiIO& io = ImGui::GetIO ();
    io.DisplaySize = ImVec2 (float (width), float (height));

    // ⚠️ A NON-ZERO DELTA, ALWAYS. ImGui asserts on DeltaTime <= 0, and an assert
    // inside a render thread in Archicad's process is a hard crash of the host
    // application rather than a message. 1/60 is the honest default for a
    // vsynced present.
    const double now = double (GetTickCount64 ()) / 1000.0;
    const double delta = impl_->lastTimeSeconds > 0.0 ? now - impl_->lastTimeSeconds : 0.0;
    impl_->lastTimeSeconds = now;
    io.DeltaTime = delta > 0.0 && delta < 1.0 ? float (delta) : 1.0f / 60.0f;

    // ---- input ------------------------------------------------------------
    // The same snapshot the camera is about to read. Position first, then the
    // TRANSITIONS -- a press and release inside one frame collapses to nothing
    // if only the final held state is carried, and a fast click on a checkbox is
    // exactly that.
    if (input.inside)
        io.AddMousePosEvent (float (input.x), float (input.y));
    else
        io.AddMousePosEvent (-FLT_MAX, -FLT_MAX);
    for (int i = 0; i < input.transitionCount && i < InputSnapshot::kMaxTransitions; ++i) {
        const int button = ToImGuiButton (input.transitions[i].button);
        if (button >= 0)
            io.AddMouseButtonEvent (button, input.transitions[i].down);
    }
    if (input.wheelDelta != 0)
        io.AddMouseWheelEvent (0.0f, float (input.wheelDelta) / 120.0f);
    io.AddKeyEvent (ImGuiMod_Shift, input.shift);

    impl_->renderer->NewFrame (width, height, Diligent::SURFACE_TRANSFORM_IDENTITY);
    ImGui::NewFrame ();

    // ---- the instruction banner (PLAT-RE111) -------------------------------
    //
    // ⚠️ FIRST, AND ACROSS THE TOP, BECAUSE IT IS THE ONLY TEXT THE USER CAN
    // READ MID-NAVIGATION. Archicad's DG palette does not repaint during a
    // navigation drag, so the status line `evp.ui.progress` writes to is frozen
    // exactly when a measurement run needs to say what to do next. The overlay
    // renders every frame regardless.
    //
    // ⚠️ NoInputs, LIKE THE CALLOUT AND FOR THE SAME REASON. Without it the
    // banner counts as a hovered ImGui item wherever it sits, `WantCaptureMouse`
    // goes true, and navigation stops working under the very thing telling the
    // user to navigate.
    if (!state.instruction.empty ()) {
        ImGui::SetNextWindowPos (ImVec2 (float (width) * 0.5f, 16.0f), ImGuiCond_Always, ImVec2 (0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha (0.85f);
        const ImGuiWindowFlags bannerFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                                             ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin ("##instruction", nullptr, bannerFlags)) {
            // Twice the default size: this is read at arm's length, in
            // peripheral vision, while the hand is busy dragging the view.
            ImGui::SetWindowFontScale (2.0f);
            ImGui::TextColored (ImVec4 (1.0f, 0.85f, 0.25f, 1.0f), "%s", state.instruction.c_str ());
            if (state.instructionSecondsRemaining >= 0.0) {
                ImGui::SetWindowFontScale (3.0f);
                ImGui::Text ("%.0f", std::ceil (state.instructionSecondsRemaining));
            }
            ImGui::SetWindowFontScale (1.0f);
        }
        ImGui::End ();
    }

    ImGui::SetNextWindowPos (ImVec2 (12.0f, 12.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize (ImVec2 (300.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin ("Tapioca viewport")) {
        ImGui::Text ("%.1f fps   %u x %u", state.fps, width, height);
        if (!state.adapter.empty ())
            ImGui::TextWrapped ("%s", state.adapter.c_str ());

        ImGui::Separator ();

        // ---- CONTROLS: PANEL ONLY -----------------------------------------
        //
        // ⚠️ THE OVERLAY GETS READOUTS, NEVER WIDGETS. `WS_EX_TRANSPARENT` is
        // all-or-nothing per window (PLAT-RE55), so on the overlay every one of
        // these would DRAW and none of them could be clicked. A control that
        // cannot be operated is worse than an absent one: the user tries it,
        // nothing happens, and a window style reads as the viewer having hung.
        // So the overlay shows the same STATE as text, and the panel owns input.
        if (state.readOnly) {
            ImGui::TextDisabled ("overlay: click-through, display only");
            ImGui::Text (
                "view %s   surfaces %s",
                kDebugViewNames[state.debugView >= 0 && state.debugView < kDebugViewCount ? state.debugView : 0],
                kRenderModeNames[state.renderMode >= 0 && state.renderMode < kRenderModeCount ? state.renderMode : 0]);
            ImGui::Text ("quality %s   projection %s",
                         kRenderQualityNames[state.renderQuality >= 0 && state.renderQuality < kRenderQualityCount
                                                 ? state.renderQuality
                                                 : 0],
                         state.orthographic ? "axonometric" : "perspective");
        }
        else {
            // ⚠️ THE COMBO'S INDEX IS THE ENUM VALUE. See kDebugViewNames.
            ImGui::SetNextItemWidth (-1.0f);
            ImGui::Combo ("##debugview", &state.debugView, kDebugViewNames, kDebugViewCount);
            ImGui::TextDisabled ("debug view");

            ImGui::SetNextItemWidth (-1.0f);
            ImGui::Combo ("##rendermode", &state.renderMode, kRenderModeNames, kRenderModeCount);
            ImGui::TextDisabled ("surfaces -- wireframe is what makes the OVERLAY readable");
            if (state.renderMode != int (SceneRenderMode::Shaded)) {
                ImGui::SliderInt ("wire subdivisions", &state.wireTessellation, 1, 16);
                ImGui::SliderFloat ("wire width", &state.wireLineWidth, 0.5f, 3.0f, "%.2f px");
            }

            // ⚠️ A SEPARATE COMBO FROM THE ONE ABOVE, not more entries in it.
            // Quality and surfaces are independent axes and every pairing is
            // wanted -- see RenderQuality in ViewerSettings.hpp.
            ImGui::SetNextItemWidth (-1.0f);
            ImGui::Combo ("##renderquality", &state.renderQuality, kRenderQualityNames, kRenderQualityCount);
            ImGui::TextDisabled ("quality -- realistic adds specular + tone mapping");

            ImGui::Checkbox ("axonometric (parallel projection)", &state.orthographic);

            // ---- Environment ------------------------------------------------
            //
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
                                     scene.whiteBalanceGains[0], scene.whiteBalanceGains[1],
                                     scene.whiteBalanceGains[2]);
            }

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
                    ImGui::TextColored (ImVec4 (1.0f, 0.4f, 0.3f, 1.0f),
                                        "the slice layer failed to create -- see archviz.log");
                }
                else if (scene.storeySlices == 0) {
                    ImGui::TextColored (ImVec4 (1.0f, 0.8f, 0.2f, 1.0f),
                                        "no storey set yet -- refresh to cut the model");
                }
                else {
                    ImGui::TextDisabled ("%llu storey(s), %.0f m2 enclosed", (unsigned long long) scene.storeySlices,
                                         scene.storeySliceAreaM2);
                }

                ImGui::Checkbox ("fill the contour", &state.storySliceFill);

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
            ImGui::Checkbox ("callout under the cursor", &state.showCallout);
            ImGui::Checkbox ("selected element properties", &state.showProperties);
        }

        ImGui::Separator ();
        ImGui::Text ("elements %llu", (unsigned long long) scene.elements);
        ImGui::Text ("triangles %llu", (unsigned long long) scene.triangles);
        if (scene.pointLayers > 0)
            ImGui::Text ("point clouds %llu   visible %llu / %llu", (unsigned long long) scene.pointLayers,
                         (unsigned long long) scene.visiblePoints, (unsigned long long) scene.points);
        ImGui::Text ("materials %llu   misses %llu", (unsigned long long) scene.materials,
                     (unsigned long long) scene.materialMisses);
        if (scene.pending > 0)
            ImGui::TextColored (ImVec4 (1.0f, 0.8f, 0.2f, 1.0f), "extracting: %llu queued",
                                (unsigned long long) scene.pending);

        ImGui::Separator ();
        // The requested C9 panels expose only values that reach a real renderer
        // subsystem. The overlay retains these diagnostics but no dead widgets.
        const bool showLightInspector = state.readOnly || ImGui::CollapsingHeader ("light inspector");
        if (showLightInspector) {
            ImGui::Text ("sun %s%s", scene.sunApplied ? "applied" : "DEFAULT (never arrived)",
                         scene.sunBelowHorizon ? ", below horizon" : "");
            ImGui::Text ("  dir %.2f %.2f %.2f   ambient %.2f", scene.sun[0], scene.sun[1], scene.sun[2],
                         scene.ambient);
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

        const bool showShadowSettings = state.readOnly || ImGui::CollapsingHeader ("shadow settings");
        if (showShadowSettings) {
            if (!state.readOnly) {
                ImGui::Checkbox ("cast shadows", &state.shadowsEnabled);
                const char* resolutions[] = { "512", "1024", "2048", "4096" };
                int resolutionIndex =
                    state.shadowResolution <= 512
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
                        ImGui::SliderFloat ("sun diameter", &state.shadowPcssLightAngularDiameter, 0.1f, 5.0f,
                                            "%.2f deg");
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
                    ImGui::SliderFloat ("light bleeding reduction", &state.shadowLightBleeding, 0.0f, 0.99f);
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
        ImGui::Text ("draws %llu   materials in pool %llu", (unsigned long long) scene.drawCalls,
                     (unsigned long long) scene.materials);
    }
    ImGui::End ();

    // ---- the frame-cost badge, ALWAYS ON -----------------------------------
    //
    // ⚠️ A SEPARATE WINDOW FROM THE PANEL ABOVE, DELIBERATELY. The panel is
    // collapsible, movable and scrollable, and on the overlay it is also
    // CLICK-THROUGH -- so once it is collapsed or pushed off the surface there is
    // no way to get the number back without restarting the viewer. The
    // requirement is that the add-on's cost is visible at every moment, and a
    // readout that can be lost does not meet it.
    //
    // NoInputs is load-bearing for the same reason it is on the callout: this
    // must never make ImGui want the mouse, or the camera and the pick stop
    // responding under a corner of the screen.
    if (state.showFpsBadge) {
        const float frameMs = io.DeltaTime * 1000.0f;
        if (frameMs > impl_->worstMsInWindow)
            impl_->worstMsInWindow = frameMs;
        if (impl_->worstMsInWindow > impl_->heldWorstMs || now >= impl_->heldUntilSeconds) {
            impl_->heldWorstMs = impl_->worstMsInWindow;
            impl_->heldUntilSeconds = now + kWorstHoldSeconds;
            impl_->worstMsInWindow = 0.0f;
        }

        // Right-hand edge, top. Pivot (1,0) is what keeps it anchored there as
        // the text width changes rather than growing off the surface.
        ImGui::SetNextWindowPos (ImVec2 (float (width) - 8.0f, 8.0f), ImGuiCond_Always, ImVec2 (1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha (0.55f);
        const ImGuiWindowFlags badgeFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin ("##fps", nullptr, badgeFlags)) {
            // The render thread's own measured rate (a half-second window), not
            // ImGui's -- they agree while the viewer is healthy and diverge
            // exactly when something is wrong, and the renderer's is the true one.
            ImGui::Text ("%.0f fps   %.1f ms", state.fps, frameMs);
            const bool slow = impl_->heldWorstMs > kSlowFrameMs;
            if (slow) {
                ImGui::TextColored (ImVec4 (1.0f, 0.55f, 0.35f, 1.0f), "worst %.0f ms", impl_->heldWorstMs);
            }
            else {
                ImGui::TextDisabled ("worst %.0f ms", impl_->heldWorstMs);
            }
        }
        ImGui::End ();
    }

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

    ImGui::Render ();
    impl_->renderer->RenderDrawData (context, ImGui::GetDrawData ());
    impl_->renderer->EndFrame ();

    // ⚠️ READ BACK AFTER Render, NOT BEFORE. WantCaptureMouse is only meaningful
    // once the frame's widgets have been submitted; asking at the top of the
    // frame reports the PREVIOUS frame's answer, and the camera then orbits the
    // model on the click that was meant for the combo box.
    state.wantsMouse = io.WantCaptureMouse;
}

} // namespace archviz
} // namespace geomsrv
