#ifndef EVP_ARCHVIZ_STORYSLICELAYER_HPP
#define EVP_ARCHVIZ_STORYSLICELAYER_HPP

// ArchViz/StorySliceLayer — the GPU half of the storey slice overlay.
//
// It owns ONLY resources: two vertex buffers (the outline ribbon and the fill),
// one constant buffer, four pipeline states and the draws. The geometry comes
// from StorySliceGeometry, which is pure and tested offline. Same split as
// PlanAnchorLayer and DiligentShadowMap, for the same reason: resource setup
// stays readable in one sitting and geometry stays testable without a device.
//
// ⚠️ IT DOES NOT USE DiligentScene's PIPELINE, AND MUST NOT — the same rule
// PlanAnchorLayer states. The scene's mesh PSO is lit, culled and shadowed, and
// every one of those is wrong for a section line. A storey slice is an
// INSTRUMENT, not a surface.
//
// ⚠️ BUT UNLIKE PlanAnchorLayer, IT DOES DEPTH-TEST, AND THAT IS THE POINT. An
// anchor is compared against Archicad's own 2D linework and must be visible
// wherever it falls, so it never depth-tests. A storey slice is read against the
// BUILDING, in 3D, where "is this contour in front of that wall or behind it" is
// information the user asked for. So the layer draws in TWO passes over one
// vertex buffer:
//
//     pass 1   depth LESS_EQUAL   the visible portions, always solid
//     pass 2   depth GREATER      the occluded portions, per OccludedStyle
//
// Neither pass WRITES depth. A section contour is an annotation; letting it
// occlude the geometry it annotates would hide the very wall being sectioned,
// and would make the two passes fight each other besides.
//
// ⚠️ THE DASH IS MEASURED IN SCREEN PIXELS, not metres, for the same reason the
// width is (see StorySliceGeometry.hpp). A world-space dash period is a solid
// line when zoomed out and one long dash when zoomed in — at which point it no
// longer reads as "this part is hidden", which is the entire message it carries.
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
} // namespace Diligent

namespace geomsrv {
namespace archviz {

struct StorySliceVertex;
struct StorySliceFillVertex;

// What happens to the parts of a contour that lie BEHIND geometry.
//
// ⚠️ THREE STATES, NOT A BOOL, because the three answer different questions and
// the user asked for all of them. `Hidden` reads the slice as a plan — only what
// this storey actually exposes. `Dashed` is the drafting convention: the whole
// contour is legible, and which parts are buried is still visible at a glance.
// `Solid` is the register check — the full contour, unmodified, for comparing
// against something else.
enum class OccludedStyle : uint8_t {
    Hidden = 0,
    Dashed = 1,
    Solid = 2,
};

class StorySliceLayer final {
  public:
    StorySliceLayer ();
    ~StorySliceLayer ();
    StorySliceLayer (const StorySliceLayer&) = delete;
    StorySliceLayer& operator= (const StorySliceLayer&) = delete;

    // Compiles both shader pairs and builds the four PSOs. The formats come from
    // the swap chain for the usual reason: a PSO records what it renders into, so
    // a mismatch fails here instead of at draw time.
    bool Init (Diligent::IRenderDevice* device, uint32_t colorBufferFormat, uint32_t depthBufferFormat,
               std::string& error);
    void Shutdown ();
    bool IsReady () const;

    size_t OutlineVertexCount () const;
    size_t FillVertexCount () const;

    // Replaces whatever the layer held. Either may be empty — an empty set is
    // ordinary (no storey selected, or a storey the model does not reach).
    bool Upload (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                 const std::vector<StorySliceVertex>& outline, const std::vector<StorySliceFillVertex>& fill,
                 std::string& error);

    struct DrawParams {
        float widthPixels = 2.0f;
        uint32_t rgba = 0xFFB300FFu;     // the outline colour
        uint32_t fillRgba = 0xFFB3002Eu; // the fill, alpha already low
        bool drawFill = false;
        OccludedStyle occluded = OccludedStyle::Dashed;
        float dashPixels = 8.0f; // one full on+off cycle
        float dashDuty = 0.55f;  // fraction of it that is drawn
    };

    void Draw (Diligent::IDeviceContext* context, const float viewProj[16], uint32_t surfaceWidth,
               uint32_t surfaceHeight, const DrawParams& params);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace archviz
} // namespace geomsrv

#endif // EVP_ARCHVIZ_STORYSLICELAYER_HPP
