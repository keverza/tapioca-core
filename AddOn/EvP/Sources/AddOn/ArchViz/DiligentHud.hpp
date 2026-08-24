#ifndef EVP_ARCHVIZ_DILIGENTHUD_HPP
#define EVP_ARCHVIZ_DILIGENTHUD_HPP

// ArchViz/DiligentHud — the Dear ImGui panel over the Diligent viewport.
//
// WHY IT EXISTS. Every question the viewport has raised so far -- is the sun
// reaching the shader, did the shadow map get fitted, how many elements have
// arrived, which debug view is on -- has been answered by running a Python
// command, reading a log, changing a parameter and running it again. That is a
// thirty-second loop for a question the renderer can answer in the corner of the
// frame it is already drawing, and it is why the debug views were nearly
// impossible to compare: by the time the second one is on screen the first is a
// memory.
//
// ⚠️ IT OWNS THE IMGUI CONTEXT AND THEREFORE MUST BE A SINGLE INSTANCE PER
// THREAD. ImGui keeps its state in a thread-global current context; the bgfx
// viewer has its own (ImGuiBgfx), and the two must never run at once. They
// cannot today -- one viewer at a time -- and if that ever changes, this is the
// thing that breaks silently rather than loudly.
//
// ⚠️ RENDER THREAD ONLY, like everything else in the Diligent viewport.
//
// ⚠️ NO ImGuiImplWin32. Input comes from the same InputRingBuffer +
// PollHardwareInput pair the camera reads, because the viewport's HWND is a DG
// palette child whose messages the palette already pumps -- installing a second
// WndProc hook over it is how two input paths start disagreeing about whether
// the mouse is down. The cost is that ImGui gets mouse and modifiers but no text
// entry, which a diagnostic HUD does not need.

#include "ArchViz/DiligentScene.hpp"   // ElementInfo, for the hover callout

#include <cstdint>
#include <string>

namespace Diligent {
struct IDeviceContext;
struct IRenderDevice;
struct ISwapChain;
}   // namespace Diligent

namespace geomsrv {
namespace archviz {

struct InputSnapshot;
struct DiligentSceneStats;

// What the HUD shows, and what the user can change from it. The viewport owns
// one of these and reads it back after Draw.
struct HudState {
    // Set by the viewport before Draw.
    double fps = 0.0;
    uint64_t frames = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string adapter;
    // ⚠️ THE FPS READOUT IS A REQUIREMENT, NOT A DECORATION, and it is drawn in
    // its own always-visible window rather than inside the main panel. The
    // add-on renders on top of Archicad and must never slow it down; the only
    // way to notice that it has started to is to be able to see the frame cost
    // at every moment, including while the main panel is collapsed, scrolled
    // away, or -- on the overlay, which is click-through -- impossible to
    // uncollapse at all.
    bool showFpsBadge = true;

    // Set by the HUD, read by the viewport. `debugView` mirrors
    // DiligentDebugView; the HUD is a SECOND way to set it, alongside the
    // command's parameter, and the two must not fight -- see DiligentHud.cpp.
    int debugView = 0;
    // ⚠️ THE SUN OVERRIDE IS A MEASURING INSTRUMENT, not a lighting control. The
    // viewport's sun did not match Archicad's 3D window, and the two readings of
    // Archicad's `sunAngXY` (CCW from +X, or from PROJECT NORTH) differ by
    // exactly the project's north -- invisible when north is zero, and plausible
    // either way. Dragging the azimuth until the shadows agree is the only way to
    // find out which, and it takes one run instead of one rebuild per guess.
    bool sunOverride = false;
    float sunAzimuthDegrees = 135.0f;
    float sunAltitudeDegrees = 45.0f;
    bool wantsMouse = false;   // ⚠️ ASK THIS BEFORE THE CAMERA READS THE MOUSE

    // SceneRenderMode as an int, for the same reason `debugView` is: this struct
    // crosses no seam, but keeping the two knobs the same shape means the
    // HUD-versus-command reconciliation in the frame loop is one pattern rather
    // than two.
    int renderMode = 0;

    // ---- READ-ONLY ON THE OVERLAY (the click-through rule) -----------------
    //
    // ⚠️ THE OVERLAY'S HUD MUST NOT OFFER A CONTROL IT CANNOT HONOUR.
    // `WS_EX_TRANSPARENT` is all-or-nothing per window (PLAT-RE55): every widget
    // on the overlay DRAWS and NONE of them can be clicked. A checkbox nobody can
    // tick is worse than no checkbox -- it invites the user to try, and the
    // failure looks like the viewer having hung rather than like a window style.
    //
    // So on the overlay the HUD is a READOUT: live numbers, hover coordinates,
    // selected-element facts, and nothing that implies input. The panel keeps the
    // full interactive set. Set by the viewport from its SurfaceMode.
    bool readOnly = false;

    // ---- render quality, ORTHOGONAL to renderMode (PLAT-RE126) -------------
    //
    // ⚠️ DELIBERATELY A SEPARATE AXIS, NOT MORE SceneRenderMode CASES. Quality
    // (how a surface is lit) and mode (whether surfaces or lines are drawn) are
    // independent questions, and the combinations are all wanted -- a fast
    // wireframe and a realistic shaded view are different answers to different
    // questions. Collapsing them is what turned CameraSyncMode into "a set of
    // independent switches wearing an enum costume", which cost a session to
    // untangle; this starts on the right side of that lesson.
    //
    // Mirrors RenderQuality in ViewerSettings.hpp.
    int renderQuality = 0;

    // ---- the environment, as live controls (PLAT-RE51) ---------------------
    //
    // ⚠️ THESE EXIST BECAUSE THE FEATURES COULD NOT BE JUDGED WITHOUT THEM.
    // Every one of them was settled by a rebuild-and-look round trip during
    // 2026-08-17 -- "is it too bright", "does the reflection move", "does the
    // sky rotation reach the diffuse as well as the specular" -- and each round
    // trip is a full build plus a restart of Archicad. They are the same four
    // knobs `Commands/ModelViewer`'s three.js panel puts under Environment, and
    // for the same reason: a rendering feature that cannot be A/B'd live cannot
    // be verified at all.
    //
    // ⚠️ NOT COMMANDABLE OVER THE BUS, on purpose, exactly as renderQuality is
    // not. A second source for one value is how the two start disagreeing; the
    // debug view and render mode already need reconciliation code in the frame
    // loop and that is enough of it.
    bool environmentEnabled = true;
    float environmentIntensity = 1.0f;
    float environmentRotationDegrees = 0.0f;
    // Whether the sky is DRAWN behind the model, not merely lighting it. Forced
    // off on the overlay surface, where an opaque background would hide
    // Archicad's own 3D window.
    bool environmentBackground = true;
    // ⚠️ HOW MUCH ANALYTIC SUN SURVIVES WHEN A SKY IS ACTIVE, and it is a real
    // control rather than a fudge factor with a nice name. An HDR containing a
    // sun disc already carries that light, so adding the analytic sun at full
    // strength double-counts it -- reported live as "too bright" and previously
    // hard-coded to 0.55 with no way to check the number. The right value
    // depends on the sky, which is exactly why it is a slider.
    float sunWithSkyWeight = 0.55f;

    // ---- Realistic grading (PLAT-RE51) -------------------------------------
    //
    // ⚠️ THE HONEST NAME FOR THESE IS "COMPENSATION FOR NON-PBR SOURCE DATA".
    // Realistic was reported as looking barely different from Fast, and the
    // measurement says why: 0.72-0.8% `shining` on nearly every painted surface
    // in this project, which is a roughness of 0.99 and a GGX lobe with no
    // visible highlight. The shading is doing the right thing to numbers that
    // describe a wholly matte building.
    //
    // `reflectance` in particular is what makes GLASS read as glass. A
    // dielectric reflects 4-8% head-on, which is correct and invisible against
    // a 0.7-radiance sky; pushing it past 1 is not physical and is exactly what
    // an architectural viewer wants until the material presets land.
    // ⚠️ MUST MATCH DiligentSceneConstants::gradeParams[0]. The HUD's value is
    // what actually reaches the shader (DiligentViewport -> SetGrading), so a
    // stale default here silently overrides the one next to the tone curve. It
    // moved 0.6 -> 1.2 when AcesFitted replaced the per-channel curve.
    float exposure = 1.2f;
    float reflectance = 1.0f;
    float roughnessBias = 0.0f;

    // ---- RE51.B9 -------------------------------------------------------------
    //
    // ⚠️ AUTO EXPOSURE SHIPS OFF, AND THE CHECKBOX IS THE MEASUREMENT. The
    // estimate (ArchViz/AutoExposure) is computed every frame whether or not it
    // is applied, and the HUD prints what it chose beside the fixed value -- so
    // the first live run says whether middle grey is the right target for this
    // project's materials without risking a single image on the answer. Turning
    // it on by default before that number exists would be guessing in public.
    // ⚠️ ON BY DEFAULT SINCE THE 2026-08-21 LIVE RUN, WHICH IS THE MEASUREMENT
    // THE PREVIOUS DEFAULT WAS WAITING FOR. It shipped OFF for one session
    // because the estimate has a calibration constant -- middle grey -- and
    // nobody had yet compared what it chooses against a render they had looked
    // at. The user's verdict from that run: "autoexposure should be default
    // setting as otherwise scene is too bright and saturated." That is exactly
    // the direction a fixed key errs in when the sky is brighter than the key
    // assumed, and it is the case B9 exists to remove.
    bool autoExposure = true;

    // ---- RE51.C3 -------------------------------------------------------------
    //
    // ⚠️ ON BY DEFAULT AND STILL A TOGGLE, because it is the first thing in the
    // forward path to cost a SECOND GEOMETRY PASS. Contact darkening is the
    // single biggest visible win left in the raster path -- nothing else in the
    // shader knows that a block is SITTING on the ground rather than floating
    // above it -- but doubling the geometry cost of every frame is a real
    // trade on a large project, and it should be one somebody can undo without
    // a rebuild.
    bool ambientOcclusion = true;

    // Scales the darkening only. ⚠️ NOT THE EFFECT'S RADIUS -- C3's acceptance
    // asks for the two to be separable, because "more AO" and "AO that reaches
    // further" are different requests and one slider cannot serve both.
    float ambientOcclusionIntensity = 1.0f;

    // The occlusion radius in world METRES. ⚠️ 0 MEANS "DERIVE IT FROM THE
    // MODEL", which is the default and is not the same as "no radius" -- see
    // DiligentScene::SetAmbientOcclusion. The derived value is printed beside
    // the slider, so moving it off zero starts from what the renderer chose
    // rather than from an arbitrary number.
    float ambientOcclusionRadius = 0.0f;

    // ---- RE51.C9: shadow settings -----------------------------------------
    // Keep the map allocated and gate fitting/sampling. Re-enabling therefore
    // restores shadows without rebuilding GPU resources.
    bool shadowsEnabled = true;

    // ---- RE51.C7: screen-space reflections --------------------------------
    // ⚠️ OFF BY DEFAULT. SSR is a port of DiligentFX's ScreenSpaceReflection,
    // and the first live run is the measurement -- the user's "tree on glass"
    // report is the acceptance test. Defaulting to on would hide whether the
    // effect is helping or hurting on a project that was never seen with it.
    bool screenSpaceReflection = false;
    // Scales how much of the SSR result is blended over the HDR colour.
    float ssrIntensity = 1.0f;
    // Surfaces rougher than this get no SSR. 0.2 = only near-mirrors; 1.0 = all.
    float ssrRoughnessThreshold = 0.2f;

    // ---- RE51.C8: temporal anti-aliasing -----------------------------------
    // Off until its first live A/B. The control makes the port reversible while
    // the unfiltered HDR fallback remains exactly the previously verified path.
    bool temporalAntiAliasing = false;
    float taaStability = 0.9f;

    // 6500 K and tint 0 are the EXACT identity (AutoExposure.hpp), so these
    // defaults change no image rendered before this existed.
    float whiteBalanceKelvin = 6500.0f;
    float whiteBalanceTint = 0.0f;

    // ---- projection: perspective or axonometric ----------------------------
    //
    // Archicad's own two 3D projections, and the viewer should offer both: a
    // massing study is judged in parallel projection, where equal lengths stay
    // equal and two schemes can be compared, while perspective is for seeing what
    // the building will look like.
    //
    // ⚠️ SWITCHING MUST NOT RE-FRAME THE MODEL. The camera keeps its eye, target
    // and orientation across the change; only the projection differs, with the
    // parallel half-height derived from the CURRENT eye-to-target distance and
    // field of view so the model covers the same part of the screen before and
    // after. A toggle that jumps the view makes the two hard to compare, which is
    // the one thing it exists for.
    //
    // ⚠️ THIS IS ALSO WHAT MAKES PLAT-RE62 REACHABLE from the panel: parallel
    // projection needs a parallel PICK, and before this toggle the only
    // orthographic camera was the plan overlay's.
    bool orthographic = false;

    // ---- story slices ------------------------------------------------------
    // Every storey's horizontal cut through the model, boolean-unioned into one
    // outline per level. The heights come from Archicad's own storey settings,
    // read in the extraction pass's acquire slice.
    //
    // ⚠️ THE TOGGLE DOES NOT BUILD THE SET. The cut runs during a FULL extraction
    // pass, and only when it was already wanted when that pass began -- a union
    // over the elements a pass happened to reach is not a rougher outline, it is
    // a clean and confident WRONG one. So switching this on raises
    // `storySlicesNeedRefresh` and the overlay stays as it was until the refresh
    // lands. The HUD says which of those two states it is in, because a checkbox
    // that does nothing for several seconds is otherwise indistinguishable from a
    // broken one.
    bool showStorySlices = false;
    // Raised by the HUD, cleared by the frame loop -- the same one-shot shape as
    // ViewerSettings::frameScene, and for the same reason.
    bool storySlicesNeedRefresh = false;
    // ⚠️ THE FILL IS BUILT WHETHER OR NOT IT IS DRAWN, so this costs nothing to
    // flip. Making it a build-time choice would put a multi-second re-extraction
    // behind a checkbox whose entire purpose is quick comparison.
    bool storySliceFill = false;
    // Defined in ViewerSettings.hpp so exactly one Diligent-free enum exists;
    // StorySliceLayer::OccludedStyle mirrors it and the frame loop converts.
    SliceOccludedStyle storySliceOccluded = SliceOccludedStyle::Dashed;
    float storySliceWidthPixels = 2.0f;
    float storySliceDashPixels  = 8.0f;    // one full on+off cycle, in pixels
    float storySliceDashDuty    = 0.55f;   // the fraction of it that is drawn
    // RGBA, packed like every other colour here. ⚠️ The FILL's alpha is low on
    // purpose: it tints the storey rather than hiding the building beneath it.
    uint32_t storySliceRgba     = 0xFFB300FFu;
    uint32_t storySliceFillRgba = 0xFFB3002Eu;

    // ---- the selected element's properties ---------------------------------
    // ⚠️ SHOWN FOR THE PICK, WHICH IS INSPECTION ONLY. Selecting in the viewer
    // never writes back to Archicad (the panel arms selectionbridge::ToViewer
    // only), so this panel is the entire point of picking here: it is where a
    // click turns into an answer.
    bool showProperties = true;
    // Filled by the viewport before Draw, from the last CLICK pick.
    DiligentScene::ElementInfo selected;

    // ---- the hover callout (PLAT-RE43) -------------------------------------
    // ⚠️ `showCallout` GATES THE HOVER PICK ITSELF, not just the drawing. The
    // callout is fed by a real GPU readback sharing one 8x8 target with clicking,
    // so a hidden callout that kept picking would still cost a readback per few
    // frames and still delay clicks behind it.
    bool showCallout = false;
    // Filled by the viewport BEFORE Draw, from the last hover pick. `valid`
    // false means the cursor is over nothing, which is a real answer.
    DiligentScene::ElementInfo hover;

    // Where the cursor is, in pixels, and where its view ray meets the GROUND
    // PLANE (world z = 0) in metres.
    //
    // ⚠️ IT IS THE GROUND PLANE, NOT THE SURFACE UNDER THE CURSOR, and the label
    // in the callout says so. The surface point would need the depth buffer read
    // back per hover -- a second readback alongside the id pick, on the path that
    // already throttles itself to keep one readback from delaying a click. The
    // ground intersection is exact, free, and is the coordinate a massing study
    // actually asks for; promising the surface point and quietly delivering this
    // one is the failure worth avoiding.
    int32_t cursorX = 0;
    int32_t cursorY = 0;
    bool cursorGroundValid = false;   // false when the ray runs parallel to z=0 or away from it
    float cursorGround[3] = {0.0f, 0.0f, 0.0f};

    // ---- the instruction banner (PLAT-RE111) -------------------------------
    // ⚠️ THIS IS THE ONLY TEXT THE USER CAN READ WHILE NAVIGATING. Archicad's DG
    // palette does not repaint during a navigation drag, so `evp.ui.progress`'s
    // status line is frozen exactly when a measurement run needs to say what to
    // do -- and the log is read afterwards, far too late. The overlay renders
    // every frame regardless, so the HUD is the one surface that can carry a
    // live instruction and a live countdown. Set over the bus; empty hides it.
    std::string instruction;
    // Seconds left, or negative for "no countdown". Recomputed per frame from a
    // deadline held by the viewport, so it ticks in real time rather than in
    // whatever steps a Python caller manages to send.
    double instructionSecondsRemaining = -1.0;
};

class DiligentHud final {
public:
    DiligentHud ();
    ~DiligentHud ();
    DiligentHud (const DiligentHud&) = delete;
    DiligentHud& operator= (const DiligentHud&) = delete;

    // ⚠️ THE FORMATS, NOT A SWAP CHAIN. In OVERLAY mode there is no `ISwapChain`
    // at all -- the composition path wraps back buffers it did not create -- so
    // asking one for its SwapChainDesc would work in the palette and be null on
    // the overlay. Both are raw `Diligent::TEXTURE_FORMAT` values, from
    // DiligentViewportTarget.
    bool Init (Diligent::IRenderDevice* device, uint32_t colorBufferFormat,
               uint32_t depthBufferFormat, std::string& error);
    void Shutdown ();
    bool IsReady () const;

    // One frame. `input` is the SAME snapshot the camera is about to read;
    // `state.wantsMouse` comes back true when the pointer is over a panel, and
    // the caller must then not let the camera consume the same click.
    void Draw (Diligent::IDeviceContext* context, uint32_t width, uint32_t height,
               const InputSnapshot& input, const DiligentSceneStats& scene,
               HudState& state);

private:
    struct Impl;
    Impl* impl_;
};

}   // namespace archviz
}   // namespace geomsrv

#endif
