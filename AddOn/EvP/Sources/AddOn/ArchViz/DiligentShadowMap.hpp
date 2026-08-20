#ifndef EVP_ARCHVIZ_DILIGENTSHADOWMAP_HPP
#define EVP_ARCHVIZ_DILIGENTSHADOWMAP_HPP

// ArchViz/DiligentShadowMap — the sun's depth buffer, and the pipeline that
// fills it.
//
// It owns ONLY the resources: the depth texture, its two views, and the
// position-only pipeline state. It does NOT iterate the scene -- `DiligentScene`
// does that, because the scene is the thing that knows which entries exist and
// which ranges are opaque. This split is why DiligentScene stays under the file
// size cap and why the resource setup can be read in one sitting.
//
// ⚠️ ONE CASCADE, DELIBERATELY. DiligentFX ships a full ShadowMapManager with
// cascade distribution, VSM and EVSM, and it is better than this in every way
// that matters for a landscape. It also arrives with the whole DiligentFX build,
// whose CMakeLists does a FetchContent from GitHub at configure time -- which
// would put a network dependency in the middle of the user's local build. A
// single ortho cascade fitted to the model's own bounds is the right shape for a
// viewer that is always looking at ONE BUILDING, and it is what the Shadows
// sample degenerates to at one cascade. When the viewer grows a site, terrain
// and a far view, this is the file that grows cascades.
//
// ⚠️ THE MAP IS ALSO A SHADER RESOURCE. A D32_FLOAT texture created with
// BIND_DEPTH_STENCIL|BIND_SHADER_RESOURCE gets an R32_FLOAT default SRV from
// Diligent automatically (GraphicsAccessories: TexFormatToViewFormatConverter).
// It must never be bound as both at once -- see Begin().

#include <cstdint>
#include <string>

namespace Diligent {
struct IBuffer;
struct IDeviceContext;
struct IPipelineState;
struct IRenderDevice;
struct IShader;
struct IShaderResourceBinding;
struct ITextureView;
}   // namespace Diligent

namespace geomsrv {
namespace archviz {

class DiligentShadowMap final {
public:
    DiligentShadowMap ();
    ~DiligentShadowMap ();
    DiligentShadowMap (const DiligentShadowMap&) = delete;
    DiligentShadowMap& operator= (const DiligentShadowMap&) = delete;

    // `shadowVs` is the position-only vertex shader; `constants` is the SAME
    // buffer the mesh shaders read, so the light matrix arrives through the
    // field the mesh pixel shader also samples with. `inputLayout` is the mesh
    // vertex layout, passed as opaque pointer + count so the header stays
    // Diligent-light.
    bool Init (Diligent::IRenderDevice* device, Diligent::IShader* shadowVs,
               Diligent::IBuffer* constants, const void* inputLayout,
               uint32_t layoutElementCount, uint32_t resolution, std::string& error);
    void Shutdown ();
    bool IsReady () const;

    uint32_t Resolution () const;
    Diligent::ITextureView* ShaderView () const;

    // Binds the shadow map as the only render target (depth only, no colour),
    // clears it, sets the viewport to the map's size, and selects the depth
    // pipeline. After this the caller issues DrawIndexed calls and nothing else.
    //
    // ⚠️ IT DOES NOT RESTORE ANYTHING. The caller must re-bind its own render
    // targets and viewport afterwards; End() only unbinds, so that the map is
    // free to be read as a texture in the same frame. Drawing the main pass with
    // the shadow map still bound as the depth target puts the whole scene into
    // the shadow map and leaves the screen on the clear colour, which reads
    // exactly like a renderer that stopped drawing.
    void Begin (Diligent::IDeviceContext* context);
    void End (Diligent::IDeviceContext* context);

    // The pipeline's SRB, which the caller commits once per Begin().
    Diligent::IShaderResourceBinding* Srb () const;

private:
    struct Impl;
    Impl* impl_;
};

}   // namespace archviz
}   // namespace geomsrv

#endif
