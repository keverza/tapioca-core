#ifndef EVP_ARCHVIZ_GHPREVIEWLAYER_HPP
#define EVP_ARCHVIZ_GHPREVIEWLAYER_HPP

// ArchViz/GhPreviewLayer — the GPU half of the Grasshopper preview overlay.
//
// It owns ONLY resources: per-bucket geometry buffers, one constant buffer,
// the mesh/line pipeline variants and the draws. The geometry comes from
// GhPreviewGeometry, which is pure and tested offline. Same split as
// PlanAnchorLayer and StorySliceLayer, for the same reason.
//
// ⚠️ IT DOES NOT USE DiligentScene's MESH PIPELINE, AND MUST NOT. The scene's
// PSO is lit, culled, shadowed and depth-written, and every one of those is
// wrong for a preview: a Grasshopper result is an INSTRUMENT held up against the
// building, not a surface of it. Preview meshes are shaded flat with a fixed
// hemispheric term so their form reads, and nothing here participates in
// shadows, ambient occlusion or the reflection pass.
//
// ⚠️ IT KNOWS NOTHING ABOUT THE CAMERA, AND THAT IS THE DIVISION OF LABOUR.
// Orbit, zoom, pan and the view matrix belong to the viewport; this layer is
// handed the same `viewProj`, clip planes and surface size, and does its
// screen-space work in clip space from those. It has no camera member, no
// navigation state and nothing to invalidate when the view moves — a moved
// camera is simply the next frame's `viewProj`, which is why panning a preview
// costs nothing but the draw.
//
// ⚠️ AND IT DOES NOT OWN THE PREVIEW EITHER. `GhPreviewCache` holds what the
// worker sent, on the CPU, independently of whether a viewport exists. Closing
// the 3D window calls `Shutdown` here and frees every GPU resource; the cache is
// untouched, so the next viewport rebuilds the same preview from the same
// snapshot without asking Grasshopper for anything. A layer that owned the
// preview would make "close the viewer" silently mean "lose your preview until
// you re-solve".
//
// ⚠️ DEPTH BEHAVIOUR IS A PIPELINE, NOT A COLOUR. A primitive flagged x-ray
// must be visible INSIDE the wall it is being compared against; an analysis
// decal instead tests against that wall with a bounded depth-only offset.
// Meshes write depth to occlude themselves; lines, translucent meshes and
// x-ray geometry do not.
//
// ⚠️ TEXT IS NOT DRAWN HERE. GhPreviewGeometry collects labels; this renderer
// has no text capability yet, and inventing a private one inside the preview
// layer is the one that gets thrown away when a real one lands. `LabelCount`
// exists so the viewport can say how many are waiting.
//
// ⚠️ RENDER THREAD ONLY, like every other layer here. No ACAPI, ever.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace Diligent {
struct IBuffer;
struct IDeviceContext;
struct IPipelineState;
struct IRenderDevice;
struct IShaderResourceBinding;
} // namespace Diligent

namespace geomsrv {
namespace archviz {

// Four binary32 ULPs at depth 1.0. Applied per fragment only by the dedicated
// surface-decal pixel shader; the projected silhouette does not move. Unlike
// the old pulled-mesh PSO there is no slope-scaled term.
constexpr float kGhPreviewSurfaceDecalDepthBiasNdc = 1.0f / 4194304.0f;

// A constant NDC pull grows quadratically in world metres with view distance.
// Bound it by maxMetres as well as by depth precision. This is the shader's
// policy in testable form; perspective clip.w is positive view depth.
constexpr float GhPreviewSurfaceDecalBiasNdc (float clipW, float nearClip, float farClip, bool perspective,
                                              float maxMetres)
{
    const float range = farClip - nearClip;
    if (maxMetres <= 0.0f || nearClip <= 0.0f || range <= 0.0f)
        return 0.0f;
    float cap = maxMetres / range;
    if (perspective) {
        const float viewDepth = clipW < 0.0f ? -clipW : clipW;
        const float safeDepth = viewDepth > 1.0e-6f ? viewDepth : 1.0e-6f;
        cap = maxMetres * nearClip * (farClip / range) / (safeDepth * safeDepth);
    }
    return kGhPreviewSurfaceDecalDepthBiasNdc < cap ? kGhPreviewSurfaceDecalDepthBiasNdc : cap;
}

struct GhPreviewDrawables;
// Forward-declared rather than included: DiligentScene.hpp takes one by const
// reference and has no business pulling the preview cache into every translation
// unit that draws a scene.
struct GhPreviewStyle;

class GhPreviewLayer final {
  public:
    GhPreviewLayer ();
    ~GhPreviewLayer ();
    GhPreviewLayer (const GhPreviewLayer&) = delete;
    GhPreviewLayer& operator= (const GhPreviewLayer&) = delete;

    // Compiles the mesh/line shaders and their pipeline variants. The formats
    // come from the target: a PSO records what it renders into, so a
    // mismatch fails here rather than at draw time.
    bool Init (Diligent::IRenderDevice* device, uint32_t colorBufferFormat, uint32_t depthBufferFormat,
               std::string& error);

    // Frees every GPU resource. Called when the viewport goes away; the CPU-side
    // GhPreviewCache is deliberately NOT touched, so reopening rebuilds.
    void Shutdown ();
    bool IsReady () const;

    // Replaces whatever the layer held. An empty set is ordinary — a definition
    // that previews nothing, or a preview toggled off.
    bool Upload (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                 const GhPreviewDrawables& drawables, std::string& error);

    struct DrawParams {
        float lineWidthPixels = 2.0f;
        // 0..1. How much of the flat shading is ambient rather than directional;
        // a preview must stay readable on the side facing away from the light,
        // because the light here is a legibility device and not a sun.
        float ambient = 0.45f;
        // > 0: the depth-tested lines are drawn a second time where the model
        // hides them, dashed with this period in pixels and `hiddenAlpha` of
        // their alpha. 0 (the preview's default) leaves hidden lines undrawn.
        float hiddenDashPixels = 0.0f;
        float hiddenAlpha = 0.55f;
        // true: depth-tested meshes take the lines' constant and slope-scaled
        // pull toward the camera. Reserved for exact solar-obstruction hits.
        // Spherical isovist patches use surfaceDecals instead, whose bounded
        // depth-only offset cannot acquire the slope-scaled term.
        bool pullMeshes = false;
        // false: segments end exactly at their endpoints. For dense polylines
        // drawn translucent, where overlapping square caps would blend every
        // joint twice and read as a jagged line (#179). Generic previews keep caps.
        bool squareCaps = true;
        // Per-frame projection data, used only to cap surface-decal depth in
        // world metres. Defaults cover generic previews, which have no decals.
        float nearClipMetres = 0.05f;
        float farClipMetres = 20000.0f;
        bool perspective = true;
        float surfaceDecalMaxMetres = 0.05f;
    };

    void Draw (Diligent::IDeviceContext* context, const float viewProj[16], uint32_t surfaceWidth,
               uint32_t surfaceHeight, const DrawParams& params);

    // What the layer is holding, for the HUD. `LabelCount` non-zero with nothing
    // drawn is the answer to "my text panel shows nothing".
    size_t MeshIndexCount () const;
    size_t LineVertexCount () const;
    size_t LabelCount () const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace archviz
} // namespace geomsrv

#endif // EVP_ARCHVIZ_GHPREVIEWLAYER_HPP
