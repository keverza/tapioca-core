#ifndef EVP_ARCHVIZ_DILIGENTSCENEIMPL_HPP
#define EVP_ARCHVIZ_DILIGENTSCENEIMPL_HPP

// ArchViz/DiligentSceneImpl — the private state DiligentScene's three
// translation units share.
//
// ⚠️ THIS HEADER IS NOT PART OF THE INTERFACE, and the split is the reason it
// exists. `DiligentScene.hpp` is deliberately Diligent-FREE so the palette side
// can hold a scene without pulling in Diligent's headers; this one is the
// opposite, and only the scene's own .cpp files may include it. If a caller
// outside ArchViz needs something here, it belongs on `DiligentScene` as a
// method instead.
//
// The scene is one class over three files because it crossed the ~1,000-line
// cap, and the seam is what the pieces need from each other rather than where
// the middle happened to be:
//
//   DiligentScene.cpp          shaders, pipeline states, lifecycle
//   DiligentSceneGeometry.cpp  the element map, Consume, bounds, stats
//   DiligentSceneDraw.cpp      the passes: shadow map, main, overlay
//
// Everything they share is here, and nothing else is.

#include "ArchViz/DiligentScene.hpp"

#include "ArchViz/DiligentAmbientOcclusion.hpp"
#include "ArchViz/DiligentEpipolarLightScattering.hpp"
#include "ArchViz/DiligentPointCloudLayer.hpp"
#include "ArchViz/GhPreviewLayer.hpp"
#include "ArchViz/StorySliceLayer.hpp"
#include "ArchViz/DiligentScreenSpaceReflection.hpp"
#include "ArchViz/DiligentTemporalAntiAliasing.hpp"
#include "ArchViz/DiligentShaders.hpp" // DiligentSceneConstants, for UploadConstants below
#include "ArchViz/DiligentDepthRange.hpp"
#include "ArchViz/DiligentShadowMap.hpp"
#include "ArchViz/EnvironmentMap.hpp"
#include "ArchViz/SunShadowMath.hpp"

#include <windows.h>
#include <d3d11.h> // Must precede any Diligent D3D11 interop header (Probe 1a).
#include <Buffer.h>
#include <DeviceContext.h>
#include <GraphicsTypes.h>
// ⚠️ NO <MapHelper.hpp>. It lives in Diligent-GraphicsTools, a module the .apx
// does not link and would not otherwise need; IDeviceContext::MapBuffer is the
// same two calls without the dependency, and it is what
// archive/experiments/TransparencyProbe/src/PatternRenderer.cpp already does.
#include <PipelineState.h>
#include <RefCntAutoPtr.hpp>
#include <RenderDevice.h>
#include <Shader.h>
#include <ShaderResourceBinding.h>
#include <TextureView.h>

#include "Components/interface/GBuffer.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {

using Diligent::RefCntAutoPtr;

// `kOpaqueAlpha` (the transparent-pass threshold) comes from MaterialTable.hpp
// -- one definition, so the two renderers cannot disagree about which surfaces
// blend.

constexpr int kCullModeCount = 3;
constexpr int kShadowModeCount = 4;

inline int ShadowModeIndex (DiligentShadowMode mode)
{
    const int index = int (mode) - int (DiligentShadowMode::Pcf);
    return index >= 0 && index < kShadowModeCount ? index : 0;
}
// Keep the render-target elements contiguous. GBuffer::Bind uses an element's
// index as its MRT slot by default, while depth is selected as the DSV and the
// motion target remains cleared-only until RE51.C2 supplies real vectors.
constexpr Diligent::Uint32 kGBufferNormal = 0;
constexpr Diligent::Uint32 kGBufferAlbedo = 1;
constexpr Diligent::Uint32 kGBufferRoughness = 2;
constexpr Diligent::Uint32 kGBufferMaterialData = 3;
constexpr Diligent::Uint32 kGBufferDepth = 4;
constexpr Diligent::Uint32 kGBufferMotion = 5;
constexpr Diligent::Uint32 kGBufferGeometryMask = (1u << kGBufferNormal) | (1u << kGBufferAlbedo) |
                                                  (1u << kGBufferRoughness) | (1u << kGBufferMaterialData) |
                                                  (1u << kGBufferDepth) | (1u << kGBufferMotion);

// ⚠️ THE MOTION TARGET'S ELEMENT INDEX IS 5 AND ITS MRT SLOT IS 4, AND WITHOUT
// THIS TABLE THEY WOULD DISAGREE. GBuffer::Bind defaults an element's MRT slot
// to its own index, but element 4 is the DEPTH buffer, which becomes the DSV and
// consumes no colour slot -- so motion would be bound at slot 5 while the pixel
// shader writes SV_TARGET4, and the vectors would land in nothing at all. The
// array lists the slot for each RENDER-TARGET element in mask order; depth is
// absent because Bind does not advance the cursor for it.
constexpr Diligent::Uint32 kGBufferRTIndices[5] = { 0, 1, 2, 3, 4 };

// ⚠️ IDs START AT 1, AND 0 IS RESERVED FOR "NOTHING". The pick target is cleared
// to zero, so a pixel of 0 means the ray missed every element -- "you clicked the
// sky". An element numbered 0 would be indistinguishable from empty space and
// would be selected every time the user clicked past the building.
constexpr uint32_t kNoPickId = 0;

// The three cull modes, as Diligent's rasterizer states.
//
// ⚠️ THESE MIRROR bgfx's D3D11 MAPPING EXACTLY, AND THAT IS THE POINT. bgfx
// sets `FrontCounterClockwise = false` and then maps BGFX_STATE_CULL_CW to
// D3D11_CULL_FRONT and CULL_CCW to D3D11_CULL_BACK (renderer_d3d11.cpp,
// s_cullMode). So "CW" does not mean "clockwise faces are front" -- it means
// cull whatever is clockwise ON SCREEN. Reproducing the mapping rather than
// reasoning it out afresh means the Diligent viewer shows the same faces as the
// bgfx one on the same setting, which is the only way "the port changed the
// picture" stays separable from "the port broke culling".
inline Diligent::CULL_MODE ToDiligentCull (CullMode cull)
{
    switch (cull) {
        case CullMode::Cw:
            return Diligent::CULL_MODE_FRONT;
        case CullMode::Ccw:
            return Diligent::CULL_MODE_BACK;
        case CullMode::None:
            return Diligent::CULL_MODE_NONE;
    }
    return Diligent::CULL_MODE_FRONT;
}

// The pipeline-state array index for a cull mode. ⚠️ THIS ORDER AND THE LOOP IN
// Init ARE ONE CONTRACT: index 0 is Ccw, 1 is Cw, 2 is None. Disagreeing does
// not fail, it draws the wrong faces.
inline int CullIndex (CullMode cull)
{
    switch (cull) {
        case CullMode::Ccw:
            return 0;
        case CullMode::Cw:
            return 1;
        case CullMode::None:
            return 2;
    }
    return 1;
}

// The cull mode an inverted-hull silhouette needs: the OTHER one.
//
// ⚠️ IT MUST TRACK THE VISIBLE PASS'S SETTING RATHER THAN BEING A CONSTANT.
// The hull works by drawing exactly the faces the visible pass throws away; with
// CullMode::None nothing is thrown away, so there is no hull to draw and the
// outline would paint a solid expanded copy of the model over the model. None
// stays None and the outline pass skips it.
inline CullMode InverseCull (CullMode cull)
{
    switch (cull) {
        case CullMode::Cw:
            return CullMode::Ccw;
        case CullMode::Ccw:
            return CullMode::Cw;
        case CullMode::None:
            return CullMode::None;
    }
    return CullMode::Ccw;
}

// How a selected element reads. ⚠️ THREE THINGS AT ONCE, AND THE LIVE RUN THAT
// ASKED FOR THEM SAID WHY: a tint alone was reported as "the element turned
// cyanish", which is exactly as much as a tint can say. It cannot show WHICH
// element boundary was hit when two abut, and it cannot show anything at all
// about the element behind it. So:
//
//   the SILHOUETTE says where the element ends, unambiguously, even against
//   another element of the same material;
//   the TINT says which element it is, at a glance, from any distance;
//   the TRANSPARENCY lets the user see what the selection is sitting in front
//   of, which is most of why they selected it.
constexpr float kSelectionTintMix = 0.35f; // how much of the material survives
constexpr float kSelectionTintR = 0.10f;
constexpr float kSelectionTintG = 0.65f;
constexpr float kSelectionTintB = 0.85f;
constexpr float kSelectionAlpha = 0.55f; // 1 = the old opaque tint
constexpr float kSelectionOutlinePixels = 3.0f;
// Deliberately NOT the tint: the outline has to be readable ON the tinted
// surface as well as against the background, so it is the same hue taken to full
// saturation rather than the same colour.
constexpr float kSelectionOutlineColor[4] = { 0.05f, 0.85f, 1.0f, 1.0f };

// The HOVER silhouette (PLAT-RE136): what a click would take, shown before it is
// taken.
//
// ⚠️ A DIFFERENT HUE FROM THE SELECTION, NOT A DIMMER ONE. The two are on screen
// at the same time and mean different things -- "this IS selected" against "this
// WOULD BE" -- and distinguishing them by brightness alone fails against a pale
// facade, which is most of a building. Amber reads as distinct from the cyan at
// any exposure and does not collide with the wireframe either.
//
// ⚠️ AND IT IS THINNER. The hover follows the mouse, so at the selection's 3 px
// it flickers a heavy band across the model on every move; thin enough to read as
// a highlight, thick enough to survive a thin railing.
constexpr float kHoverOutlineColor[4] = { 1.0f, 0.62f, 0.10f, 1.0f };
constexpr float kHoverOutlinePixels = 2.0f;

// The wireframe's line colour. Alpha below 1 so overlapping edges on a dense
// mesh build up instead of saturating into a solid mass.
constexpr float kWireframeColor[4] = { 0.10f, 0.85f, 1.0f, 0.75f };

struct Entry {
    std::string guid; // empty for a static mesh
    RefCntAutoPtr<Diligent::IBuffer> vertexBuffer;
    RefCntAutoPtr<Diligent::IBuffer> indexBuffer;
    RefCntAutoPtr<Diligent::IBuffer> wireEdgeBuffer;
    Diligent::Uint32 vertexCount = 0;
    Diligent::Uint32 indexCount = 0;
    bool indices32 = true;
    std::vector<MaterialRange> ranges;
    uint32_t transparentRanges = 0;
    uint32_t unmappedRanges = 0;
    size_t gpuBytes = 0;
    // Stable for as long as the entry lives; handed to the pick pass as a colour
    // and mapped back on readback. ⚠️ NOT THE INDEX IN `elements` -- that moves
    // when swap-and-pop removes an element, and a pick already in flight would
    // then resolve to whichever element took its place.
    uint32_t pickId = kNoPickId;
    bool selected = false;
    bool seenThisBatch = false;
    bool hasBounds = false;
    float boundsMin[3] = { 0.0f, 0.0f, 0.0f };
    float boundsMax[3] = { 0.0f, 0.0f, 0.0f };
};

// ---- the three draw helpers, shared by the forward and G-buffer passes ------
//
// ⚠️ THEY LIVE HERE BECAUSE TWO TRANSLATION UNITS DRAW THE SAME GEOMETRY, and
// they must do it identically. DiligentSceneDraw renders the image the user
// sees; DiligentSceneGBuffer renders the same triangles into the G-buffer that
// the occlusion and the debug views read. A second copy of "how a range is
// bound and drawn" would be free to drift, and the symptom of that drift is
// occlusion that does not line up with the shading it darkens -- which reads as
// a projection bug rather than as two copies of one loop.
inline void UploadConstants (Diligent::IDeviceContext* context, Diligent::IBuffer* buffer,
                             const DiligentSceneConstants& constants)
{
    Diligent::PVoid mapped = nullptr;
    context->MapBuffer (buffer, Diligent::MAP_WRITE, Diligent::MAP_FLAG_DISCARD, mapped);
    if (mapped != nullptr) {
        *static_cast<DiligentSceneConstants*> (mapped) = constants;
        context->UnmapBuffer (buffer, Diligent::MAP_WRITE);
    }
}

inline void BindMesh (Diligent::IDeviceContext* context, const Entry& e)
{
    Diligent::IBuffer* vertexBuffers[1] = { e.vertexBuffer };
    const Diligent::Uint64 offsets[1] = { 0 };
    context->SetVertexBuffers (0, 1, vertexBuffers, offsets, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                               Diligent::SET_VERTEX_BUFFERS_FLAG_RESET);
    context->SetIndexBuffer (e.indexBuffer, 0, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

inline void DrawEntryRange (Diligent::IDeviceContext* context, const Entry& e, const MaterialRange& r)
{
    Diligent::DrawIndexedAttribs draw;
    draw.NumIndices = r.indexCount;
    draw.FirstIndexLocation = r.firstIndex;
    draw.IndexType = e.indices32 ? Diligent::VT_UINT32 : Diligent::VT_UINT16;
    draw.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
    context->DrawIndexed (draw);
}

// Vertex and index buffers for one mesh, replacing whatever the entry held.
// Shared by the element upserts and by the static/overlay meshes.
bool CreateMeshBuffers (Diligent::IRenderDevice* device, const char* name, Entry& entry, const void* vertices,
                        size_t vertexBytes, const void* indices, size_t indexBytes, std::string& error);

} // namespace archviz
} // namespace geomsrv

struct geomsrv::archviz::DiligentScene::Impl {
    // ⚠️ KEPT SO A DEFERRED LOAD CAN CREATE RESOURCES. The environment map is
    // loaded on the RENDER thread, frames after the bus asked for it, and the
    // device is not otherwise in scope there. It is a borrowed raw pointer, not
    // a RefCntAutoPtr, deliberately: the viewport owns the device and outlives
    // the scene, and taking a strong reference here is precisely the mistake
    // PLAT-RE39 spent a session on -- a second strong reference kept the device
    // alive past its own teardown and the viewport could then only be opened once.
    Diligent::IRenderDevice* device = nullptr;
    RefCntAutoPtr<Diligent::IShader> vs;
    RefCntAutoPtr<Diligent::IShader> ps[kShadowModeCount];
    RefCntAutoPtr<Diligent::IShader> shadowVs;
    RefCntAutoPtr<Diligent::IBuffer> constants;
    // One PSO per cull mode, twice: opaque and blended. A PSO is immutable, so
    // a state toggle is a different object rather than a state change -- the
    // whole stateless-design difference from bgfx::setState.
    RefCntAutoPtr<Diligent::IPipelineState> opaquePso[kShadowModeCount][kCullModeCount];
    RefCntAutoPtr<Diligent::IPipelineState> blendPso[kShadowModeCount][kCullModeCount];
    RefCntAutoPtr<Diligent::IShaderResourceBinding> opaqueSrb[kShadowModeCount][kCullModeCount];
    RefCntAutoPtr<Diligent::IShaderResourceBinding> blendSrb[kShadowModeCount][kCullModeCount];
    // The pick pass. A THIRD set rather than a reuse of the opaque one, because
    // a PSO records the formats it renders into and the pick target is an 8x8
    // RGBA8_UNORM, not the swap chain's sRGB back buffer.
    RefCntAutoPtr<Diligent::IShader> flatPs;
    RefCntAutoPtr<Diligent::IPipelineState> pickPso[kCullModeCount];
    RefCntAutoPtr<Diligent::IShaderResourceBinding> pickSrb[kCullModeCount];
    // The selection silhouette (PLAT-RE41) and the wireframe (PLAT-RE42). Both
    // are the flat pixel shader over the swap chain's formats; they differ in the
    // vertex shader and the rasterizer, so they are two PSO sets rather than one
    // with a flag.
    RefCntAutoPtr<Diligent::IShader> outlineVs;
    RefCntAutoPtr<Diligent::IPipelineState> outlinePso[kCullModeCount];
    RefCntAutoPtr<Diligent::IShaderResourceBinding> outlineSrb[kCullModeCount];
    // ⚠️ ONE WIREFRAME PSO, NOT THREE. Lines have no facing to cull -- culling a
    // wireframe removes half of every closed surface's edges, which reads as a
    // broken mesh rather than as a cull setting. CULL_MODE_NONE always.
    RefCntAutoPtr<Diligent::IPipelineState> wirePso;
    RefCntAutoPtr<Diligent::IShaderResourceBinding> wireSrb;
    RefCntAutoPtr<Diligent::IShader> wireVs;
    RefCntAutoPtr<Diligent::IShader> wireHs;
    RefCntAutoPtr<Diligent::IShader> wireDs;
    RefCntAutoPtr<Diligent::IShader> wireGs;
    RefCntAutoPtr<Diligent::IShader> wirePs;
    RefCntAutoPtr<Diligent::IPipelineState> semanticWirePso;
    RefCntAutoPtr<Diligent::IShaderResourceBinding> semanticWireSrb;
    bool semanticWireSupported = false;

    std::unique_ptr<Diligent::GBuffer> gBuffer;
    RefCntAutoPtr<Diligent::IShader> gBufferPs;
    RefCntAutoPtr<Diligent::IShader> fullScreenVs;
    RefCntAutoPtr<Diligent::IShader> gBufferDebugPs;
    RefCntAutoPtr<Diligent::IShader> ambientOcclusionDebugPs;
    RefCntAutoPtr<Diligent::IPipelineState> gBufferPso[kCullModeCount];
    RefCntAutoPtr<Diligent::IShaderResourceBinding> gBufferSrb[kCullModeCount];
    RefCntAutoPtr<Diligent::IPipelineState> gBufferDebugPso;
    RefCntAutoPtr<Diligent::IShaderResourceBinding> gBufferDebugSrb;
    RefCntAutoPtr<Diligent::IPipelineState> ambientOcclusionDebugPso;
    RefCntAutoPtr<Diligent::IShaderResourceBinding> ambientOcclusionDebugSrb;
    RefCntAutoPtr<Diligent::IShader> envBackgroundVs;
    RefCntAutoPtr<Diligent::IShader> envBackgroundPs;
    RefCntAutoPtr<Diligent::IPipelineState> envBackgroundPso;
    RefCntAutoPtr<Diligent::IShaderResourceBinding> envBackgroundSrb;
    // ---- the HDR scene-colour target and its resolve pass --------------------
    //
    // ⚠️ THE HDR PSOs ARE THE SAME SHADERS AS THE LDR ONES, only the RTV format
    // differs (RGBA16_FLOAT instead of the swap chain's sRGB). A PSO records its
    // render-target formats at creation time, so a format change is a new object
    // rather than a state toggle. The mesh pixel shader branches on
    // g_frameControl.x to skip Grade() when writing into HDR; the resolve PS
    // applies Grade() once when copying HDR back to the swap chain.
    RefCntAutoPtr<Diligent::IShader> resolvePs;
    RefCntAutoPtr<Diligent::IShader> coveragePs;
    RefCntAutoPtr<Diligent::IShader> ssrCompositePs;
    RefCntAutoPtr<Diligent::IPipelineState> hdrOpaquePso[kShadowModeCount][kCullModeCount];
    RefCntAutoPtr<Diligent::IPipelineState> hdrBlendPso[kShadowModeCount][kCullModeCount];
    RefCntAutoPtr<Diligent::IShaderResourceBinding> hdrOpaqueSrb[kShadowModeCount][kCullModeCount];
    RefCntAutoPtr<Diligent::IShaderResourceBinding> hdrBlendSrb[kShadowModeCount][kCullModeCount];
    RefCntAutoPtr<Diligent::IPipelineState> hdrEnvBackgroundPso;
    RefCntAutoPtr<Diligent::IShaderResourceBinding> hdrEnvBackgroundSrb;
    RefCntAutoPtr<Diligent::IPipelineState> resolvePso;
    RefCntAutoPtr<Diligent::IShaderResourceBinding> resolveSrb;
    RefCntAutoPtr<Diligent::ITexture> hdrColorTexture;
    Diligent::ITextureView* hdrColorRTV = nullptr;
    Diligent::ITextureView* hdrColorSRV = nullptr;
    uint32_t hdrWidth = 0;
    uint32_t hdrHeight = 0;

    // ---- RE51.C8: coverage, so TAA can resolve it -------------------------
    // ⚠️ A WHOLE TEXTURE FOR ONE CHANNEL, AND IT IS NOT WASTE. DiligentFX's TAA
    // reads Texture2D<float3> and writes its history weight into alpha, so the
    // coverage that lives in the HDR target's alpha cannot pass through it
    // where it is. kArchVizCoveragePS broadcasts it into RGB here and TAA's
    // SECOND accumulation buffer resolves it beside the radiance. Only ever
    // allocated on the HDR path with TAA on; see EnsureCoverageTarget.
    Diligent::RefCntAutoPtr<Diligent::ITexture> coverageTexture;
    Diligent::ITextureView* coverageRTV = nullptr;
    Diligent::ITextureView* coverageSRV = nullptr;
    uint32_t coverageWidth = 0;
    uint32_t coverageHeight = 0;
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> coveragePso;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> coverageSrb;
    // The accumulated coverage the resolve should read, or null when TAA did
    // not produce one this frame -- in which case the resolve falls back to the
    // raw HDR alpha, which is correct precisely because no jitter was applied.
    Diligent::ITextureView* taaCoverageView = nullptr;

    // ---- RE51.C7: the SSR composite, which must land BEFORE TAA -----------
    // ⚠️ A SECOND HDR TARGET, BECAUSE A PASS CANNOT READ AND WRITE ONE TEXTURE.
    // The composite reads the scene colour and writes the same image with the
    // reflections folded in; TAA then accumulates THAT. Only allocated when SSR
    // is actually running -- see EnsureSsrCompositeTarget.
    Diligent::RefCntAutoPtr<Diligent::ITexture> ssrCompositeTexture;
    Diligent::ITextureView* ssrCompositeRTV = nullptr;
    Diligent::ITextureView* ssrCompositeSRV = nullptr;
    uint32_t ssrCompositeWidth = 0;
    uint32_t ssrCompositeHeight = 0;
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> ssrCompositePso;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> ssrCompositeSrb;
    // ---- RE51.C7: the SSR composition PSO and state --------------------------
    //
    // ⚠️ THE COMPOSITION HAPPENS IN THE RESOLVE PASS, NOT AS A SEPARATE PASS.
    // The resolve PS samples the HDR colour, the SSR radiance, and the G-buffer
    // roughness, blends SSR over HDR based on confidence and roughness, then
    // applies Grade() and writes to the swap chain. One pass, no extra targets.
    //
    // ⚠️ `ssrView` IS NON-NULL ONLY BETWEEN PrepareScreenSpaceReflection AND
    // THE Draw THAT CONSUMES IT. It points into the SSR pass's own texture,
    // which that pass owns and may reallocate on resize, so it is never held
    // across a frame boundary -- same contract as `aoView`.
    Diligent::ITextureView* ssrView = nullptr;
    bool ssrEnabled = false;
    float ssrIntensity = 1.0f;
    float ssrRoughnessThreshold = 0.2f;
    // A 1x1 black RGBA16_FLOAT texture bound to the resolve's g_ssrColor when
    // SSR is off, so the shader variable is always assigned and the SRB is
    // valid. ⚠️ SAME LESSON AS THE AO FALLBACK: a DYNAMIC variable left unset
    // is a validation failure inside Archicad's process.
    RefCntAutoPtr<Diligent::ITexture> ssrFallback;
    // A 1x1 R16_FLOAT at 1.0 (fully rough) bound to g_gbufferRoughness when SSR
    // is off, so the shader's `roughness < 1.0` check skips the SSR branch.
    RefCntAutoPtr<Diligent::ITexture> ssrRoughnessFallback;
    DiligentAmbientOcclusion ambientOcclusion;
    DiligentEpipolarLightScattering epipolarLightScattering;
    bool atmosphereEnabled = false;
    bool atmosphereLightShafts = true;
    bool atmosphereLightingOnly = false;
    float atmosphereIntensity = 10.0f;
    DiligentScreenSpaceReflection screenSpaceReflection;
    DiligentTemporalAntiAliasing temporalAntiAliasing;
    DiligentPointCloudLayer pointCloud;
    // ---- the storey section overlay ----------------------------------------
    // ⚠️ THE PENDING SET IS HELD HERE RATHER THAN UPLOADED ON ARRIVAL, because
    // Consume has a device but no context (see DiligentScene::DrawStorySlices).
    // `storySlicesDirty` rather than "is the pointer null": an EMPTY set is a
    // real answer -- the storeys were read and the planes missed the model --
    // and it must replace the previous one rather than leave it on screen.
    StorySliceLayer storySlices;
    std::unique_ptr<StorySliceUpload> pendingStorySlices;
    bool storySlicesDirty = false;
    bool storySliceInitFailed = false;
    uint32_t storeySliceCount = 0;
    double storeySliceAreaM2 = 0.0;

    // ⚠️ ONE METHOD RATHER THAN FIVE LINES AT THE CALL SITE, because the call
    // site is DiligentScene.cpp -- a file the architecture gate has FROZEN at its
    // current size. Teardown that belongs together should live together anyway;
    // this just makes the rule and the taste agree.
    void ShutdownStorySlices ()
    {
        storySlices.Shutdown ();
        pendingStorySlices.reset ();
        storySlicesDirty = false;
        storySliceInitFailed = false;
        storeySliceCount = 0;
        storeySliceAreaM2 = 0.0;
    }

    // ---- the Grasshopper preview overlay -----------------------------------
    // ⚠️ THE LAYER IS THE ONLY THING TORN DOWN WITH THE VIEWPORT. What the
    // definition previewed lives in Preview/GhPreviewCache, on the CPU, and is
    // deliberately NOT touched here: closing the 3D window frees the GPU
    // resources and the next window rebuilds the same preview from the same
    // snapshot, without asking Grasshopper for anything. A viewport that owned
    // the preview would make "close the viewer" silently mean "lose your preview
    // until you re-solve".
    //
    // `ghPreviewGeneration` is what decides when to rebuild: GhPreviewCache
    // stamps a monotonic generation on every snapshot it publishes, so an
    // unchanged preview costs a pointer compare per frame rather than a rebuild.
    GhPreviewLayer ghPreview;
    uint64_t ghPreviewGeneration = 0;
    bool ghPreviewInitFailed = false;
    size_t ghPreviewDeferredKinds = 0;
    bool ghPreviewTruncated = false;

    void ShutdownGhPreview ()
    {
        ghPreview.Shutdown ();
        ghPreviewGeneration = 0;
        ghPreviewInitFailed = false;
        ghPreviewDeferredKinds = 0;
        ghPreviewTruncated = false;
    }
    Diligent::ITextureView* taaView = nullptr;
    bool taaEnabled = false;
    float taaStability = 0.9f;
    // ⚠️ REPORTED BECAUSE THE FALLBACK IS SILENT. Draw falls back to the raw
    // HDR target whenever the TAA pass returns nothing, and the raw target is
    // the JITTERED one -- so a TAA that never runs does not look like a missing
    // effect, it looks like the renderer developed a shake. These two say which
    // of those is happening without needing a debug view.
    bool taaResolvedThisFrame = false;
    float taaJitterInUse[2] = { 0.0f, 0.0f };
    DiligentDepthRange depthRange;
    uint32_t gBufferWidth = 0;
    uint32_t gBufferHeight = 0;
    uint32_t gBufferFrameIndex = 0;
    bool gBufferFrameValid = false;

    std::vector<Entry> elements;      // Archicad geometry, keyed by GUID
    std::vector<Entry> staticMeshes;  // the debug cube, until geometry lands
    std::vector<Entry> overlayMeshes; // the gnomon: screen corner, not world

    MaterialTable materials;
    bool inFullBatch = false;

    // The selected set, KEPT rather than only applied.
    //
    // ⚠️ THE ELEMENTS ARRIVE AFTER THE SELECTION AS OFTEN AS BEFORE IT: the user
    // selects a stair on the floor plan and THEN opens the viewport, and the
    // bridge only pushes when Archicad's selection CHANGES. Without this the
    // stair extracts a second later, unselected, and the tint simply never
    // appears -- indistinguishable from the bridge not working at all.
    std::vector<std::string> selectionGuids;

    bool IsSelectedGuid (const std::string& guid) const
    {
        for (const std::string& g : selectionGuids) {
            if (g == guid)
                return true;
        }
        return false;
    }

    // The element under the cursor, as a pick id. 0 = none. See
    // DiligentScene::SetHoverId: it is not part of `selectionGuids` and must
    // never become part of it.
    uint32_t hoverId = kNoPickId;

    float sun[3] = { 0.0f, 0.0f, 1.0f };
    float ambient = 0.35f;
    bool sunApplied = false;
    bool sunBelowHorizon = false;
    // The HUD's override. See DiligentScene.hpp -- it exists to settle whether
    // Archicad's sunAngXY is measured from +X or from project north.
    // Project north, CCW from +X, degrees -- carried only so the compass bearing
    // can be reported. It never enters the sun VECTOR; see ExtractionThread.
    float northDegrees = 90.0f;
    // The place and moment the stored sun belongs to, carried through for the
    // HUD. Nothing here enters a lighting calculation -- see
    // EnvironmentUpload's ⚠️ on why the STORED angles are the ones used.
    float latitudeDegrees = 0.0f;
    float longitudeDegrees = 0.0f;
    float siteAltitudeMetres = 0.0f;
    uint16_t year = 0, month = 0, day = 0, hour = 0, minute = 0;
    bool summerTime = false;
    bool haveComputedSun = false;
    float computedAzimuthDegrees = 0.0f;
    float computedAltitudeDegrees = 0.0f;
    bool sunOverride = false;
    float sunOverrideAzimuth = 135.0f;
    float sunOverrideAltitude = 45.0f;

    // The sun the shader should use this frame: Archicad's, or the override.
    void EffectiveSun (float out[3]) const
    {
        if (!sunOverride) {
            out[0] = sun[0];
            out[1] = sun[1];
            out[2] = sun[2];
            return;
        }
        constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
        const float azimuth = sunOverrideAzimuth * kDegToRad;
        const float altitude = sunOverrideAltitude * kDegToRad;
        const float horizontal = std::cos (altitude);
        out[0] = horizontal * std::cos (azimuth);
        out[1] = horizontal * std::sin (azimuth);
        out[2] = std::sin (altitude);
    }

    DiligentShadowMap shadowMap;
    bool shadowsEnabled = true;

    // The HDR sky. ⚠️ ALLOCATED AT INIT AND NEVER REPLACED, so the static
    // binding made at pipeline-creation time stays valid across loads --
    // EnvironmentMap.hpp explains why that constrains its size.
    EnvironmentMap environment;
    // Applied to the sky's radiance; 0 disables the environment path
    // without unloading it, which is what makes an A/B possible in one run.
    float environmentIntensity = 1.0f;
    bool environmentEnabled = true;
    float environmentRotationRadians = 0.0f;
    // Whether the sky is DRAWN behind the model, as opposed to only lighting
    // it. ⚠️ SEPARATE FROM environmentEnabled, and it has to be: the viewport
    // also runs as a DirectComposition overlay ON TOP of Archicad's own 3D
    // window, where an opaque sky would hide the very thing being annotated.
    // The frame loop turns this off for that surface mode.
    bool environmentBackground = true;
    // How much analytic sun survives beside an active sky. See the HUD slider.
    float sunWithSkyWeight = 0.55f;
    // Realistic-quality grading: exposure into the ACES curve, a multiplier on
    // both specular terms, and a bias added to every material's roughness.
    //
    // ⚠️ 1.2, AND IT HAD DRIFTED FROM DiligentHud::exposure UNTIL 2026-08-21.
    // The tone curve moved from Narkowicz's per-channel ACES to Hill's fit, and
    // 1.2 is the pre-scale at which the two agree on neutral grey to within
    // 0.002; the HUD's copy moved with it and this one did not. It was invisible
    // because the HUD calls SetGrading on its first frame and overwrites this,
    // so the number here only decides what a frame drawn BEFORE the HUD exists
    // looks like -- and a headless render (PLAT-RE52) has no HUD at all, so it
    // would have rendered every image at half the exposure of the interactive
    // viewport. These two are ONE value in two places; move them together.
    float exposure = 1.2f;
    float reflectance = 1.0f;
    float roughnessBias = 0.0f;
    // RE51.B9. `autoExposure` gates whether `exposure` above is used at all;
    // `lastAutoExposure` is what the estimate CHOSE this frame and is reported
    // whether or not it was applied -- see DiligentScene::SetAutoExposure.
    // ⚠️ true, AND IT MUST MATCH DiligentHud::autoExposure. Same trap as
    // `exposure` two lines up: the HUD overwrites this on its first frame, so
    // the value here only decides what a HUD-less render does -- and a headless
    // still (PLAT-RE52) that metered differently from the viewport would be the
    // exposure drift all over again, in a place nobody looks.
    bool autoExposureEnabled = true;

    // ---- RE51.C3 ------------------------------------------------------------
    // ⚠️ `aoView` IS NON-NULL ONLY BETWEEN PrepareAmbientOcclusion AND THE Draw
    // THAT CONSUMES IT. It points into the AO pass's own texture, which that
    // pass owns and may reallocate on resize, so it is never held across a
    // frame boundary -- Draw clears it after binding.
    bool aoEnabled = true;
    float aoIntensity = 1.0f;
    // <= 0 means "derive from the model's bounds"; see SetAmbientOcclusion.
    float aoRadius = 0.0f;
    // What that derivation (or the override) actually produced last frame.
    float aoRadiusInUse = 0.0f;
    Diligent::ITextureView* aoView = nullptr;

    // ⚠️ A 1x1 WHITE TEXTURE MEANING "NOTHING IS OCCLUDED", AND IT IS NOT A
    // CONVENIENCE. `g_ambientOcclusion` is a DYNAMIC shader variable, so it has
    // to be assigned on every SRB before CommitShaderResources -- and there are
    // frames with no occlusion to assign: the first ones, before geometry
    // arrives; any frame with the effect switched off; and any frame where the
    // AO pass declined. Committing an unbound dynamic variable is a validation
    // failure inside Archicad's process, which is exactly the class of fault
    // that took it down once already.
    //
    // ⚠️ AND IT IS *WHITE*, WHICH IS THE SECOND HALF. An unbound texture reads
    // as ZERO, and zero occlusion means FULLY occluded -- the whole model black.
    // Binding white makes the failure mode "no effect" instead of "no image",
    // so the constant gate in g_gradeParams.w becomes a second line of defence
    // rather than the only one.
    RefCntAutoPtr<Diligent::ITexture> aoFallback;

    // ---- RE51.C2 ------------------------------------------------------------
    // Last frame's view-projection. ⚠️ SEEDED FROM THE FIRST FRAME'S OWN MATRIX
    // rather than left as the identity -- see DiligentSceneConstants::
    // prevViewProj for what an identity previous camera does to every temporal
    // effect on frame one.
    float prevViewProj[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    bool havePrevViewProj = false;
    float lastAutoExposure = 0.0f;
    float lastSceneLuminance = 0.0f;
    float whiteBalanceKelvin = 6500.0f;
    float whiteBalanceTint = 0.0f;
    // The view ray basis for the background pass, world space. See
    // DiligentSceneConstants::envRayRight for the convention.
    float cameraRayRight[3] = { 0.0f, 0.0f, 0.0f };
    float cameraRayUp[3] = { 0.0f, 0.0f, 0.0f };
    float cameraRayForward[3] = { 1.0f, 0.0f, 0.0f };
    // A load requested over the bus, consumed by the frame loop.
    // ⚠️ THE BUS THREAD MUST NOT TOUCH THE DEVICE CONTEXT, so the path is
    // parked here and the RENDER thread does the work -- the same shape
    // every other cross-thread request in this viewer uses.
    std::string pendingEnvironmentPath;
    bool environmentLoadPending = false;
    std::string environmentError;

    SceneRenderMode renderMode = SceneRenderMode::Shaded;
    // 1 = no subdivision; see DiligentHud.hpp's wireTessellation for why that is
    // the default and why the floor is 1 rather than 0.
    float wireTessellation = 1.0f;
    float wireLineWidth = 1.25f;
    // ⚠️ Fast BY DEFAULT, and the overlay never leaves it: the overlay's frame
    // budget belongs to Archicad, and it exists to be compared AGAINST
    // Archicad's shading rather than to out-render it.
    RenderQuality renderQuality = RenderQuality::Fast;
    // The viewport, in pixels, as of the last Draw. Only the silhouette needs it
    // -- its offset is expressed in pixels and has to become NDC somewhere.
    uint32_t viewportWidth = 0;
    uint32_t viewportHeight = 0;

    bool ready = false;
    size_t drawCalls = 0;
    // ⚠️ MONOTONIC FOR THE LIFE OF THE SCENE, never reused. Recycling the id of a
    // removed element would let a pick issued before the removal resolve to the
    // element that inherited the number.
    uint32_t nextPickId = kNoPickId + 1;

    Entry* Find (const std::string& guid)
    {
        for (Entry& e : elements) {
            if (e.guid == guid)
                return &e;
        }
        return nullptr;
    }
};

#endif
