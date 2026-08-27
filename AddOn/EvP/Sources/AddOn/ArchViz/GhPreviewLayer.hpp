#ifndef EVP_ARCHVIZ_GHPREVIEWLAYER_HPP
#define EVP_ARCHVIZ_GHPREVIEWLAYER_HPP

// ArchViz/GhPreviewLayer — the GPU half of the Grasshopper preview overlay.
//
// It owns ONLY resources: three vertex buffers, one index buffer, one constant
// buffer, four pipeline states and the draws. The geometry comes from
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
// handed the same `viewProj` and surface size every other layer gets, and does
// its screen-space work in clip space from those. It has no camera member, no
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
// ⚠️ FOUR PSOs, BECAUSE X-RAY IS A PIPELINE. A primitive flagged x-ray must be
// visible INSIDE the wall it is being compared against, which is a
// depth-test-off variant rather than a colour. Mesh and line each get a
// depth-tested and an x-ray PSO. NONE of the four writes depth: preview is an
// annotation, and letting it occlude the building it annotates would hide the
// very wall being measured.
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

    // Compiles both shader pairs and builds the four PSOs. The formats come from
    // the target for the usual reason: a PSO records what it renders into, so a
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
