#ifndef EVP_ARCHVIZ_PLANANCHORLAYER_HPP
#define EVP_ARCHVIZ_PLANANCHORLAYER_HPP

// ArchViz/PlanAnchorLayer — the GPU half of PLAT-RE65's plan anchors.
//
// It owns ONLY resources: one vertex buffer, one constant buffer, one pipeline
// state, and a draw. The triangles come from PlanAnchorRibbon (pure, tested
// offline) and the outlines come from Archicad's 2D data through
// Tapioca.SetPlanAnchors. Same split as DiligentShadowMap, for the same reason:
// the resource setup stays readable in one sitting and the geometry stays
// testable without a device.
//
// ⚠️ IT DOES NOT USE DiligentScene's PIPELINE, AND MUST NOT. The scene's mesh
// PSO is lit, depth-tested, culled and shadowed — every one of which is wrong
// for an anchor. An anchor is an INSTRUMENT: a flat, unlit, constant-width line
// that must be visible wherever it falls, including on top of the geometry it
// is being compared against. Reusing the mesh PSO would hide anchors inside
// walls, which is precisely where they need to be seen.
//
// ⚠️ NO DEPTH TEST AND NO CULLING, both deliberate. Depth-testing an anchor
// against the model buries it in the very wall it outlines. Culling a flat
// ribbon erases the whole layer as soon as the camera crosses its plane, which
// on a top-down plan camera is one scroll away.
//
// ⚠️ RENDER THREAD ONLY, like everything else here.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Diligent {
struct IBuffer;
struct IDeviceContext;
struct IPipelineState;
struct IRenderDevice;
struct IShaderResourceBinding;
}   // namespace Diligent

namespace geomsrv {
namespace archviz {

struct PlanAnchorVertex;

class PlanAnchorLayer final {
public:
    PlanAnchorLayer ();
    ~PlanAnchorLayer ();
    PlanAnchorLayer (const PlanAnchorLayer&) = delete;
    PlanAnchorLayer& operator= (const PlanAnchorLayer&) = delete;

    // Compiles the pair and builds the PSO. The formats come from the swap
    // chain for the usual reason: a PSO records what it renders into, so a
    // mismatch fails here instead of at draw time.
    bool Init (Diligent::IRenderDevice* device, uint32_t colorBufferFormat,
               uint32_t depthBufferFormat, std::string& error);
    void Shutdown ();
    bool IsReady () const;

    // Replace the whole ribbon. WHOLE SET, replacing — the anchors are a
    // snapshot of what the plan currently shows, and a delta protocol would
    // have to survive a dropped message to stay in step with it.
    //
    // The buffer is recreated only when the vertex count GROWS, so a storey
    // being re-read at the same size costs one map instead of an allocation.
    bool Upload (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                 const std::vector<PlanAnchorVertex>& vertices, std::string& error);

    // `viewProj` is MatrixMath's row-major product, uploaded unchanged (see
    // DiligentShaders.hpp for why that is the transpose the shader wants).
    // `widthPixels` is the FULL line width; the shader pushes half of it each
    // way. `rgba` is 0xRRGGBBAA.
    void Draw (Diligent::IDeviceContext* context, const float viewProj[16],
               uint32_t surfaceWidth, uint32_t surfaceHeight,
               float widthPixels, uint32_t rgba);

    size_t VertexCount () const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}   // namespace archviz
}   // namespace geomsrv

#endif
