#ifndef EVP_ARCHVIZ_DILIGENTSCENE_HPP
#define EVP_ARCHVIZ_DILIGENTSCENE_HPP

// ArchViz/DiligentScene — the Diligent port of SceneCache.
//
// Consumes `SceneCmdQueue` exactly as `SceneCache` does: one vertex/index
// buffer pair per element keyed by GUID, one draw per MATERIAL RANGE, opaque
// before transparent. The reasoning behind every one of those choices lives in
// `SceneCache.hpp`/`.cpp` and is NOT repeated here — it did not change with the
// renderer. What changed is only how each step is spelled.
//
// ⚠️ THE FOUR THINGS THAT MUST NOT BE LOST IN THE TRANSLATION, because each
// produces a plausible-looking picture rather than a failure:
//
//   ONE DRAW PER MATERIAL RANGE. Drawing an element in one call paints every
//   surface in the first material's colour. That is the whole reason MeshGroups
//   exists, and it looks like a styling choice.
//
//   32-BIT INDICES. A curtain wall or a detailed stair passes 65,535 vertices
//   easily; a 16-bit buffer wraps silently and draws triangles between
//   unrelated corners, which reads as a bad extraction.
//
//   TRANSPARENT RANGES DO NOT WRITE DEPTH. A pane of glass that writes depth
//   hides the whole room behind it. Unsorted blending merely looks slightly
//   wrong; this cannot delete the building.
//
//   THE MATERIAL TABLE MUST PRECEDE ITS UPSERTS. The pool is renumbered on
//   every rebuild, so an element drawn against the previous table is the right
//   building in another material's colours.
//
// ⚠️ RENDER THREAD ONLY. Diligent permits resource creation from any thread,
// unlike bgfx, but nothing here is synchronised and the context certainly is
// not.
//
// ⚠️ THE HEADER IS Diligent-FREE on purpose: the palette side and the state
// command hold one of these without pulling in Diligent's headers.

#include "ArchViz/ArchVizVertex.hpp"
#include "ArchViz/SceneCmdQueue.hpp"
#include "ArchViz/Uniforms.hpp"
#include "ArchViz/ViewerSettings.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Diligent {
struct IRenderDevice;
struct IDeviceContext;
struct ITextureView;
} // namespace Diligent

namespace geomsrv {
namespace archviz {

struct DiligentSceneStats {
    size_t elements = 0;
    size_t triangles = 0;
    size_t vertices = 0;
    size_t gpuBytes = 0;
    size_t pending = 0;
    size_t drawCalls = 0;
    // ⚠️ THESE TWO TOGETHER ARE THE DIAGNOSIS FOR A WHITE BUILDING, which is
    // otherwise indistinguishable from a white building: 0 materials = the
    // table never arrived; N materials with misses = the pool was renumbered
    // under us.
    size_t materials = 0;
    size_t materialMisses = 0;
    size_t transparentRanges = 0;
    // ⚠️ `sunApplied` false means no SetEnvironment ever arrived and the shader
    // is running on a hardcoded default -- indistinguishable from a real sun by
    // eye, and the first thing to check when the model reads flat.
    bool sunApplied = false;
    bool sunBelowHorizon = false;
    float sun[3] = { 0.0f, 0.0f, 1.0f };
    float ambient = 0.35f;
    // The sun ACTUALLY IN USE, as angles, and whether it came from the HUD
    // rather than from Archicad. ⚠️ Reported separately from `sun` so a log can
    // never claim Archicad's sun while showing an overridden one.
    bool sunOverridden = false;
    // ⚠️ TWO AZIMUTHS, AND CONFUSING THEM IS THE WHOLE HISTORY OF THIS FEATURE.
    //   sunAzimuthDegrees   MODEL space, counterclockwise from +X. This is
    //                       sunAngXY, it is what the shader's vector encodes,
    //                       and it is reported in [-180, 180].
    //   sunBearingDegrees   COMPASS, clockwise from geographic north, [0, 360).
    //                       `northDegrees - sunAzimuthDegrees`. This is the one
    //                       Archicad's dialogs show, so it is the only one a
    //                       user can compare a number against.
    // A live report of "Archicad says 240, the viewer says -120" was these two
    // quantities, unlabelled, in different ranges -- so both are carried now and
    // both are labelled wherever they are printed.
    float sunAzimuthDegrees = 0.0f;
    float sunBearingDegrees = 0.0f;
    float northDegrees = 90.0f;
    float sunAltitudeDegrees = 0.0f;
    // ⚠️ THE PLACE AND MOMENT THE SUN CAME FROM, so "the viewer's sun is wrong"
    // can be told apart from "the viewer is reading a different project's place
    // settings" without opening a log. The angles above are Archicad's STORED
    // ones; `computed*` is what this date and place would imply, carried purely
    // so a project whose sun was typed by hand names itself.
    float latitudeDegrees = 0.0f;
    float longitudeDegrees = 0.0f;
    float siteAltitudeMetres = 0.0f;
    uint16_t year = 0, month = 0, day = 0, hour = 0, minute = 0;
    bool summerTime = false;
    bool haveComputedSun = false;
    float computedAzimuthDegrees = 0.0f;
    float computedAltitudeDegrees = 0.0f;
    // ⚠️ `shadowReady` false with `shadowFitted` true means the map exists but
    // the sun's frustum could not be fitted this frame (no bounds, or a
    // degenerate sun) -- the scene then renders UNSHADOWED, which is the correct
    // fallback and is otherwise indistinguishable from shadows being switched
    // off. `shadowResolution` 0 means the map was never created at all.
    bool shadowReady = false;
    bool shadowFitted = false;
    uint32_t shadowResolution = 0;
    float shadowTexelMetres = 0.0f;

    // ---- the HDR environment ------------------------------------------------
    // ⚠️ `environmentAverage` IS NOT DECORATION. A sky that loads, resamples and
    // uploads perfectly but happens to be black renders EXACTLY like a texture
    // that was never bound, and no debug view separates them. One number does.
    // `environmentError` is the only place a deferred load's failure survives,
    // since SetEnvironmentMap returns before the load is attempted.
    bool environmentLoaded = false;
    bool environmentActive = false; // loaded AND enabled AND intensity > 0
    uint32_t environmentMipLevels = 0;
    float environmentAverage[3] = { 0.0f, 0.0f, 0.0f };
    std::string environmentPath;
    std::string environmentError;
    // ---- RE51.B6: whether the mip chain is a real GGX prefilter -------------
    // ⚠️ NOTHING ON SCREEN SEPARATES THESE. A prefiltered chain and a
    // box-filtered one are both "a blurrier sky at higher roughness"; the only
    // way to know which one a reflection is reading is to ask.
    // `environmentPrefilterError` names the fallback's reason and is empty when
    // the prefilter ran.
    bool environmentPrefiltered = false;
    uint32_t environmentPrefilteredMips = 0;
    double environmentPrefilterMs = 0.0;
    std::string environmentPrefilterError;

    // ---- RE51.B9: the exposure the light implies ----------------------------
    // ⚠️ REPORTED EVEN WHEN NOT APPLIED. `autoExposureEnabled` says whether it
    // reached the shader; `autoExposure` and `sceneLuminance` are computed every
    // frame regardless, so one live run produces the number that decides whether
    // middle grey is the right target for this project.
    bool autoExposureEnabled = false;
    float autoExposure = 0.0f;
    float sceneLuminance = 0.0f;
    float appliedExposure = 0.0f;
    // ⚠️ THE MANUAL VALUE, REPORTED EVEN WHILE AUTO IS DRIVING. Without it the
    // drift check has nowhere to look: `appliedExposure` is the estimate once
    // auto is on, and the two defaults that drifted apart once already
    // (DiligentHud::exposure and DiligentSceneImpl::exposure) would go
    // unwatched for exactly as long as auto stayed enabled.
    float fixedExposure = 0.0f;
    float whiteBalanceGains[3] = { 1.0f, 1.0f, 1.0f };
    // The mean linear reflectance of the surface pool -- the auto exposure's
    // other input, and the one that comes from the model rather than the sky.
    float meanAlbedo = 0.0f;

    // ---- RE51.B2: how far the substance join actually got --------------------
    // ⚠️ COVERAGE IS THE WHOLE MEASUREMENT AND IT IS NOT A SUCCESS RATE.
    // BuildingMaterialSignal refuses whenever its two signals disagree, and
    // SubstanceJoin refuses again whenever a surface is shared across
    // substances -- so a low number is a property of the project's attributes
    // and of how its surfaces are reused, not a failure. What WOULD be a failure
    // is zero with a non-empty pool, which says the element-index join is wrong.
    // The per-substance breakdown is what separates those two.
    size_t substanceNamed = 0;
    // Indexed by Substance's own enum values, so index 0 is the refusals.
    size_t substanceCounts[7] = {};
    // How many elements draw highlighted. ⚠️ THIS IS THE ONE NUMBER THAT
    // SEPARATES "the bridge never delivered the selection" from "the tint is not
    // visible": a selection of three in Archicad and 0 here is the former, 3
    // here and nothing on screen is the latter, and by eye they are one symptom.
    size_t selected = 0;
};

class DiligentScene final {
  public:
    DiligentScene ();
    ~DiligentScene ();
    DiligentScene (const DiligentScene&) = delete;
    DiligentScene& operator= (const DiligentScene&) = delete;

    // Compiles the shaders and builds the pipeline states. The two formats come
    // from the swap chain: a PSO records the formats it renders into, so a
    // mismatch fails at creation rather than at draw time. `error` carries the
    // reason on false -- an alert cannot.
    bool Init (Diligent::IRenderDevice* device, uint32_t colorBufferFormat, uint32_t depthBufferFormat,
               std::string& error);
    void Shutdown ();
    bool IsReady () const;

    // A mesh that is not an Archicad element: the debug cube, the axis gnomon.
    // Kept separate from the element map so a real extraction
    // does not remove them and they need no GUID. Drawn with the material
    // colour white, so their own vertex colours come through unchanged.
    bool AddStaticMesh (Diligent::IRenderDevice* device, const char* name, const ArchVizVertex* vertices,
                        size_t vertexCount, const uint16_t* indices, size_t indexCount, std::string& error);
    void ClearStaticMeshes ();

    // An OVERLAY mesh is drawn by DrawOverlay and by nothing else: the axis
    // gnomon, which belongs in a corner of the screen at a fixed size rather
    // than in the world.
    //
    // ⚠️ IT IS A SEPARATE LIST BECAUSE A WORLD-SPACE GNOMON DOES NOT WORK, and
    // that was found the expensive way: a two-metre gnomon at the project origin
    // is, in an ordinary project, INSIDE THE GROUND FLOOR SLAB. The first live
    // run of the real model reported it simply absent, and "buried in the
    // geometry" and "never uploaded" look identical from outside.
    bool AddOverlayMesh (Diligent::IRenderDevice* device, const char* name, const ArchVizVertex* vertices,
                         size_t vertexCount, const uint16_t* indices, size_t indexCount, std::string& error);

    // Drain up to `maxCommands` from SceneCmdQueue and apply them.
    //
    // ⚠️ BOUNDED, AND THAT IS THE POINT. A full extraction queues thousands of
    // elements; uploading them all in one frame stops the viewer presenting for
    // a second and takes that second out of Archicad's UI thread too. Returns
    // how many it applied.
    size_t Consume (Diligent::IRenderDevice* device, size_t maxCommands);

    // Everything visible, opaque pass then transparent pass. `viewProj` is
    // MatrixMath's row-major product, uploaded unchanged (DiligentShaders.hpp
    // explains why that is the transpose the shader wants).
    //
    // ⚠️ `eye` IS NOT DERIVED HERE. The pixel shader needs the camera position
    // to decide which side of a face it is looking at; Camera already has it
    // exactly, and re-deriving a value someone holds is how the two quietly
    // stop agreeing.

    // Fill the sun's depth buffer from the current geometry and sun.
    //
    // ⚠️ CALL IT BEFORE BINDING THE MAIN RENDER TARGETS, and expect it to leave
    // NOTHING bound: it swaps the render targets to the shadow map and unbinds
    // them again. Returns false when there was nothing to fit a frustum to, in
    // which case Draw renders unshadowed rather than wrong.
    bool RenderShadowMap (Diligent::IDeviceContext* context);

    // ⚠️ `colorTarget` AND `depthTarget` ARE PARAMETERS BECAUSE THIS FUNCTION
    // NOW BINDS THEM ITSELF, AND IT DOES THAT BECAUSE NOT DOING SO PRODUCED A
    // GREY VIEWPORT ON 2026-08-21. Draw used to inherit whatever the frame loop
    // had bound. That held for as long as nothing in between rebound anything
    // -- and then TWO passes inside this very call started doing exactly that:
    // the ambient-occlusion prepass (RE51.C3), which renders the G-buffer and
    // ends with SetRenderTargets(0, ...), and the environment prefilter
    // (RE51.B6), which renders into the environment map's own mip chain when an
    // HDR arrives. After either one, every subsequent draw in the frame went
    // nowhere. Nothing errored: the geometry simply stopped appearing while the
    // G-buffer debug views -- which bind their own target at the end -- kept
    // working perfectly, which is what made it look like a shading fault rather
    // than a binding one.
    //
    // ⚠️ THE DEPTH VIEW IS NOT CLEARED HERE. The frame loop clears it once,
    // deliberately, before navigation; clearing it again would discard the
    // shadow pass's and the prepass's contribution to this frame's ordering.
    void Draw (Diligent::IDeviceContext* context, Diligent::ITextureView* colorTarget,
               Diligent::ITextureView* depthTarget, const float viewProj[16], const float eye[3], CullMode cull,
               int debugView);

    // Render opaque geometry into the shader-readable normal/depth G-buffer,
    // then resolve one channel to the viewport target.
    void DrawGBufferDebug (Diligent::IDeviceContext* context, Diligent::ITextureView* colorTarget, const float view[16],
                           const float proj[16], const float viewProj[16], const float eye[3], float nearClip,
                           float farClip, float focusDistance, bool perspective, uint32_t frameIndex, CullMode cull,
                           int debugView);

    // Shaded, wireframe, or both. ⚠️ WIREFRAME IS AN OVERLAY REQUIREMENT, NOT A
    // STYLE -- see SceneRenderMode in ViewerSettings.hpp for why.
    void SetRenderMode (SceneRenderMode mode);
    SceneRenderMode RenderMode () const;

    // How well surfaces are lit. ⚠️ A SEPARATE AXIS FROM RenderMode, not more
    // cases of it -- ViewerSettings.hpp says why, and it is the CameraSyncMode
    // lesson applied before rather than after the fact. It needs no new PSO: the
    // quality rides in the constant buffer and the pixel shader branches on it,
    // so switching costs one uniform rather than a pipeline rebuild.
    void SetRenderQuality (RenderQuality quality);
    RenderQuality GetRenderQuality () const;

    // ---- the HDR environment (PLAT-RE51) -----------------------------------
    //
    // ⚠️ THE LOAD IS DEFERRED TO THE RENDER THREAD, so this returns immediately
    // and CANNOT report whether the file was readable. Ask
    // `DiligentSceneStats::environmentError` a frame or two later -- the bus
    // thread has no device context, and doing the file read here is how a
    // renderer acquires a crash that reproduces only on slow disks.
    //
    // An empty path unloads.
    void SetEnvironmentMap (const char* path);
    // Intensity multiplies the sky's radiance; rotation turns it about Z, in
    // DEGREES at this boundary (radians in the constant buffer).
    void SetEnvironmentSettings (bool enabled, float intensity, float rotationDegrees);

    // Whether the loaded sky is also DRAWN behind the model. ⚠️ MUST BE FALSE
    // ON THE COMPOSITION OVERLAY: there the viewport is a transparent layer over
    // Archicad's own 3D window, and an opaque sky hides it completely.
    void SetEnvironmentBackground (bool enabled);

    // 0..1: how much of the analytic sun survives when an HDR sky is active.
    // An HDR with a visible sun disc already carries that light, so the two
    // double-count at full strength — see the HUD's `sun beside the sky`.
    void SetSunWithSkyWeight (float weight);

    // Realistic-quality grading. `exposure` is the pre-scale into the ACES
    // curve (0.6 reproduces the previously hard-coded look), `reflectance`
    // multiplies both specular terms (1 = physical), `roughnessBias` is added
    // to every material's roughness (negative = glossier).
    //
    // ⚠️ THESE COMPENSATE FOR SOURCE DATA, NOT FOR THE SHADER. See
    // DiligentSceneConstants::gradeParams: Archicad's surfaces are Blinn-Phong
    // percentages, and this project's read as matte almost everywhere.
    void SetGrading (float exposure, float reflectance, float roughnessBias);

    // ---- RE51.B9: the finishing controls that follow the LIGHT ---------------

    // Auto exposure. When on, `SetGrading`'s exposure is IGNORED and the value
    // is derived from the scene's own light each frame (ArchViz/AutoExposure).
    //
    // ⚠️ ON BY DEFAULT SINCE THE 2026-08-21 LIVE RUN. It shipped off for one
    // session because the estimate has a calibration constant and nobody had
    // compared what it chooses against a render they had looked at. That run
    // settled it: the fixed key was reported as "too bright and saturated",
    // which is what a fixed key does when the sky is brighter than the key
    // assumed. Both values are still reported every frame (Stats::autoExposure
    // beside Stats::fixedExposure) so the comparison stays available.
    void SetAutoExposure (bool enabled);

    // White balance, as the temperature of the light being CORRECTED FOR.
    // 6500 K and tint 0 is the exact identity -- see AutoExposure.hpp.
    void SetWhiteBalance (float kelvin, float tint);

    // The camera's view-ray basis, for the sky background. Supplied rather than
    // derived because the scene has no camera — only the frame loop does.
    // `right` and `up` must already carry the projection plane's half-extent,
    // and are ZERO in a parallel projection.
    void SetCameraRays (const float right[3], const float up[3], const float forward[3]);

    // The surface's size in pixels, for the ONE thing that needs it: the
    // selection silhouette's offset is expressed in pixels and has to become NDC
    // somewhere. ⚠️ CALL IT BEFORE Draw ON ANY FRAME THE SIZE MAY HAVE CHANGED;
    // a stale size makes the outline the wrong thickness rather than absent,
    // which is much harder to notice.
    void SetViewportSize (uint32_t width, uint32_t height);

    // Override the sun with an explicit azimuth and altitude, in DEGREES.
    //
    // ⚠️ THIS EXISTS TO SETTLE A CONVENTION, not to be a lighting control. The
    // viewport's sun did not match Archicad's 3D window on the first live run
    // against a real project, and the two candidate readings of `sunAngXY`
    // (measured CCW from +X in model space, or measured from PROJECT NORTH)
    // differ by exactly the project's `north` -- which is invisible in any
    // project whose north is zero, and produces an entirely plausible shadow
    // either way. A guess cannot be falsified by looking, so the HUD offers the
    // angle directly and the user turns it until the shadows agree.
    //
    // `azimuth` is CCW from +X (east) looking down; `altitude` is above the
    // horizon. Passing enabled=false returns to Archicad's own computed sun.
    void SetSunOverride (bool enabled, float azimuthDegrees, float altitudeDegrees);

    // The overlay meshes only, with their own view-projection — the caller has
    // already set a small corner viewport and cleared its depth. Unshadowed and
    // unaffected by the sun: an orientation reference that changes brightness
    // with the time of day is a worse reference.
    void DrawOverlay (Diligent::IDeviceContext* context, const float viewProj[16], const float eye[3]);

    // ---- picking (PLAT-RE34) -----------------------------------------------
    // The same geometry, drawn as FLAT ID COLOURS into DiligentPickBuffer's id
    // target.
    //
    // ⚠️ `viewProj` IS THE VISIBLE PASS'S OWN, UNMODIFIED, and that is the fix
    // PLAT-RE136 is. It used to be a second matrix aimed at the cursor by
    // DiligentPickBuffer::Aim, which put two descriptions of one camera in the
    // code; every reported picking fault was the two disagreeing. Hand this the
    // matrix Draw is given on the same frame and pixel (x, y) of the id buffer is
    // pixel (x, y) of the picture, in every projection and at every DPI.
    //
    // ⚠️ THE CULL MODE MUST BE THE ONE Draw USED. A pick pass that culls
    // differently from the visible pass can resolve a click to the BACK face of
    // a surface the user cannot see, which reads as picking selecting a random
    // element behind the building.
    //
    // ⚠️ ONE DRAW PER ELEMENT, not per material range: the id is a property of
    // the element, so splitting by range would multiply the pass's cost by the
    // material count for an identical picture.
    void DrawIds (Diligent::IDeviceContext* context, const float viewProj[16], CullMode cull);

    // What the viewer knows about one element, for the hover callout.
    //
    // ⚠️ EVERYTHING HERE IS DERIVED FROM THE GEOMETRY THE RENDERER ALREADY HOLDS,
    // AND THAT IS A DELIBERATE LIMIT. An element's TYPE, layer, ID and storey all
    // live behind ACAPI, which the render thread may never call (CLAUDE.md) --
    // reaching them would mean a MainThreadGate hop per hover, i.e. ~3 ms of
    // Archicad's UI thread every time the mouse moves. Bounds, height and
    // triangle counts answer "is the viewer holding the same object Archicad
    // is", which is what an overlay comparison actually asks; a type name is a
    // separate feature with a separate cost (PLAT-RE45).
    struct ElementInfo {
        bool valid = false;
        std::string guid;
        bool selected = false;
        float boundsMin[3] = { 0.0f, 0.0f, 0.0f };
        float boundsMax[3] = { 0.0f, 0.0f, 0.0f };
        size_t triangles = 0;
        size_t vertices = 0;
        size_t materialRanges = 0;
        bool hasTransparency = false;
    };

    // By pick id (what the GPU readback gives) and by GUID (what the selection
    // bridge speaks). Both return `valid == false` for something no longer in the
    // scene, which is ORDINARY -- a live sync can remove an element between the
    // hover and the readback.
    ElementInfo InfoForId (uint32_t id) const;
    ElementInfo InfoForGuid (const std::string& guid) const;

    // The GUID an id colour maps back to. Empty for 0 (the cleared background --
    // "you clicked the sky") and for an id no longer in the scene, which is
    // ORDINARY rather than an error: the readback lands a few frames after the
    // click and a live sync may have removed the element in between.
    std::string GuidForId (uint32_t id) const;

    // Which elements draw highlighted. WHOLE SET, replacing -- Archicad's
    // selection is a set, and a delta protocol would have to survive a dropped
    // message to stay in step with it.
    void SetSelection (const std::vector<std::string>& guids);

    // Which element the cursor is over, as a PICK ID rather than a GUID, and 0
    // for none. It gets its own silhouette so the user can see what a click
    // would take before making it (PLAT-RE136).
    //
    // ⚠️ AN ID, NOT A GUID, AND DELIBERATELY. The hover changes several times a
    // second; the id is what the readback already produced, and turning it into
    // a GUID here would mean a string lookup and a string copy per mouse move
    // for a value only ever compared back against the same id. It also makes
    // "the element vanished from the scene" a non-event: an id no element
    // carries simply outlines nothing.
    //
    // ⚠️ THE HOVER IS NOT A SELECTION and must never become one. It draws an
    // outline and nothing else -- no tint, no `selected` flag, no entry in
    // `selectionGuids`, so moving the mouse across the viewport cannot alter
    // what the properties panel describes or what the bridge would write back.
    void SetHoverId (uint32_t id);
    uint32_t HoverId () const;

    // The element the properties panel describes. Empty when nothing is selected.
    std::string PrimarySelectedGuid () const;
    size_t SelectionCount () const;

    // The whole scene's AABB, elements only. False when no element has arrived:
    // framing an empty scene points the camera at the origin, which reads as
    // "the model failed to load" even when it simply has not arrived yet.
    bool SceneBounds (float outMin[3], float outMax[3]) const;

    DiligentSceneStats Stats () const;

    // ---- RE51.C3: ambient occlusion in the ORDINARY path --------------------

    // Run the G-buffer geometry prepass and GTAO, and keep the result for the
    // next `Draw` to multiply into its ambient term.
    //
    // ⚠️ THIS IS A SECOND GEOMETRY PASS, AND IT IS THE FIRST TIME THE FORWARD
    // PATH HAS PAID FOR ONE. Every earlier G-buffer use was debug-only,
    // deliberately, so ordinary rendering stayed single-pass. Contact darkening
    // cannot be had without depth and normals for the whole frame, so the trade
    // is now made -- and it is made VISIBLY, behind an HUD toggle, rather than
    // by quietly doubling the cost of every frame.
    //
    // ⚠️ CALL IT BEFORE Draw, NOT AFTER. Calling it after would darken the NEXT
    // frame with THIS frame's occlusion, which under orbit reads as the shading
    // lagging the camera and is easy to mistake for a temporal effect.
    //
    // ⚠️ IT LEAVES NO RENDER TARGET BOUND, AND Draw REBINDS ITS OWN *BECAUSE OF
    // THIS*. Draw used to inherit the frame loop's binding; this prepass ends
    // with SetRenderTargets(0, ...) so the G-buffer can be sampled, and the
    // first live run after it landed rendered a GREY VIEWPORT with no geometry
    // and no error at all -- while the G-buffer debug views, which bind their
    // own target at the end, kept working perfectly. Do not "simplify" Draw
    // back to inheriting its targets.
    void PrepareAmbientOcclusion (Diligent::IDeviceContext* context, const float view[16], const float proj[16],
                                  const float viewProj[16], const float eye[3], float nearClip, float farClip,
                                  float focusDistance, uint32_t frameIndex, CullMode cull);

    // Forget any prepared occlusion, so the next Draw shades without it. ⚠️ THE
    // FRAME LOOP MUST CALL THIS WHENEVER IT SKIPS THE PREPASS, or Draw keeps
    // multiplying in a texture that describes an older camera.
    void ClearAmbientOcclusion ();

    // ---- RE51.C2: real motion vectors ---------------------------------------
    //
    // Hand this frame's view-projection over to be the NEXT frame's previous
    // one. ⚠️ CALL IT ONCE, AFTER EVERY PASS THAT USED THIS FRAME'S CAMERA.
    // Calling it early makes a frame's motion vectors zero against itself, which
    // is silent -- temporal effects simply stop reprojecting and nothing on
    // screen says so until something moves fast.
    //
    // ⚠️ AND CALL IT ON EVERY PATH, INCLUDING THE DEBUG VIEWS. A frame that
    // skips it leaves the previous matrix two frames stale, so the next motion
    // vector is twice as long as the truth -- which reads as a temporal effect
    // over-shooting rather than as a missed call.
    void AdvanceFrame (const float viewProj[16]);

    // `intensity` scales the darkening only -- 0 is off, 1 is the effect at the
    // strength GTAO computed. ⚠️ SEPARATED FROM THE EFFECT'S OWN STRENGTH on
    // purpose: the breakdown's acceptance for C3 asks for exactly this, because
    // "more AO" and "a wider AO radius" are different requests and one slider
    // cannot serve both.
    void SetAmbientOcclusion (bool enabled, float intensity);

  private:
    struct Impl;

    // Resize the G-buffer to the viewport and rebind the debug SRB. False when
    // any target failed to allocate, in which case nothing may sample them.
    bool EnsureGBufferTargets ();

    // The opaque geometry, into the G-buffer's MRTs. Shared by the debug views
    // and by the occlusion prepass so the two cannot describe different scenes.
    void RenderGBufferGeometry (Diligent::IDeviceContext* context, const float viewProj[16], CullMode cull);
    std::unique_ptr<Impl> impl_;
};

} // namespace archviz
} // namespace geomsrv

#endif
