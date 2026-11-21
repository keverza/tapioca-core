#ifndef EVP_ARCHVIZ_DILIGENTSHADOWMAP_HPP
#define EVP_ARCHVIZ_DILIGENTSHADOWMAP_HPP

// DiligentFX's cascaded sun-shadow resources and the depth-only pipeline that
// fills them. DiligentScene still owns scene iteration and opaque-range policy.

#include <cstdint>
#include <string>

namespace Diligent {
struct IBuffer;
struct IDeviceContext;
struct IRenderDevice;
struct IShader;
struct IShaderResourceBinding;
struct ITextureView;
struct ShadowMapAttribs;
} // namespace Diligent

namespace geomsrv {
namespace archviz {

enum class DiligentShadowMode : uint32_t {
    Pcf = 1,
    Vsm = 2,
    Evsm2 = 3,
    Evsm4 = 4,
};

struct DiligentShadowSettings {
    uint32_t resolution = 2048;
    uint32_t cascadeCount = 4;
    DiligentShadowMode mode = DiligentShadowMode::Pcf;
    int fixedFilterSize = 3;
    float partitioningFactor = 0.95f;
    bool snapCascades = true;
    bool stabilizeExtents = true;
    bool equalizeExtents = true;
    // ⚠️ THERE ARE DELIBERATELY NO `searchBestCascade` / `filterAcrossCascades`
    // FIELDS HERE, AND THAT IS A DECISION RATHER THAN AN OVERSIGHT. Both were
    // carried as bools for a while and neither was ever read: DiligentFX selects
    // them with BEST_CASCADE_SEARCH and FILTER_ACROSS_CASCADES, which are
    // PREPROCESSOR defines resolved when the mesh pixel shader is compiled (see
    // kArchVizShadowMode* in DiligentShaders.hpp), so no value written after
    // shader creation can reach them.
    //
    // Making them real would mean a pixel-shader permutation per combination --
    // four shadow modes x four flag states instead of four -- and a matching
    // widening of every PSO and SRB array in DiligentSceneImpl. That is a
    // material build-time and startup cost for two flags that should never be
    // off: cascade search costs a short loop and picks a tighter cascade, and
    // cross-cascade filtering is what hides the seam between them. Turning
    // either off makes the image worse for no gain, which is why they are
    // compiled in rather than exposed.
    //
    // For LOOKING at cascade placement, which is the one honest reason to want
    // the seam visible, use `visualizeCascades` below -- it exists for exactly
    // that and it is a real runtime uniform.
    bool visualizeCascades = false;
    bool shadowsOnly = false;
    // #234: split the scene's depth range rather than the projection's (FitShadowDepthRange).
    // Off only to compare against the old fit (RenderBench --shadow-fit 0).
    bool fitCascadesToScene = true;
    // #233: 0.0005, down from 0.0025. A flat depth offset lets light through under every
    // zero-thickness occluder edge: RenderBench measured a band ~10 px wide at each Cornell and
    // bathroom wall join at 0.0025. The per-cascade normal offset already removes the acne, so
    // the smaller value shows none; zero would too, but keeps no margin for far cascades.
    float fixedDepthBias = 0.0005f;
    float receiverPlaneBiasClamp = 10.0f;
    float cascadeTransition = 0.1f;
    float filterWorldSize = 0.05f;
    float evsmPositiveExponent = 40.0f;
    float evsmNegativeExponent = 5.0f;
    float lightBleedingReduction = 0.1f;
    float vsmBias = 0.0001f;
    bool pcssEnabled = true;
    float pcssLightAngularDiameter = 2.0f;
    float pcssBlockerSearch = 2.0f;
    float pcssMaxPenumbra = 1.0f;
    // #234: what the PCSS path returns instead of its light amount, for the shadowsOnly view (it
    // forces PCSS). 0 = the light amount. 1 = one depth comparison at the receiver's texel with
    // every bias; 2 = the same without the receiver-plane and fixed bias; 6 = as 1 without the
    // normal offset. 3 = 0 where the blocker search found a blocker. 4 = the filter radius ranked
    // (floor, <= 4, <= 8, wider texels). 5 = the cascade, (index + 1) / 4. 7 = the light amount with
    // the blocker search before #234 (2 m disk, no cone test, no centre tap). RenderBench --shadow-diag.
    int diagnostic = 0;
};

bool CompileDiligentShadowPixelShader (Diligent::IRenderDevice* device, DiligentShadowMode mode,
                                       Diligent::IShader** shader, std::string& error);

class DiligentShadowMap final {
  public:
    DiligentShadowMap ();
    ~DiligentShadowMap ();
    DiligentShadowMap (const DiligentShadowMap&) = delete;
    DiligentShadowMap& operator= (const DiligentShadowMap&) = delete;

    bool Init (Diligent::IRenderDevice* device, Diligent::IShader* shadowVs, Diligent::IBuffer* sceneConstants,
               const void* inputLayout, uint32_t layoutElementCount, const DiligentShadowSettings& settings,
               std::string& error);
    void Shutdown ();
    bool IsReady () const;
    bool IsFitted () const;

    bool SetSettings (const DiligentShadowSettings& settings, std::string& error);
    const DiligentShadowSettings& Settings () const;

    // `sceneMin`/`sceneMax` (optional, world metres): the casters' and receivers' bounds. With
    // them the cascades split the depth range the scene occupies, not the projection's whole
    // near..far (#234, see ShadowDepthFit.hpp).
    // `visibleDepth` (#14), when given: the view-depth range the frame shows ({near, far} metres,
    // VisibleShadowDepthRange). The cascades split the intersection of it and the scene box's range.
    bool Prepare (Diligent::IDeviceContext* context, const float view[16], const float projection[16],
                  const float towardSun[3], const float* sceneMin = nullptr, const float* sceneMax = nullptr,
                  const float* visibleDepth = nullptr);
    void BeginCascade (Diligent::IDeviceContext* context, uint32_t cascade);
    void End (Diligent::IDeviceContext* context);

    uint32_t Resolution () const;
    uint32_t CascadeCount () const;
    float FirstCascadeTexelMetres () const;
    void CopyCascadeViewProjection (uint32_t cascade, float out[16]) const;
    void CopyAttribs (Diligent::ShadowMapAttribs& out) const;

    Diligent::IBuffer* AttribsBuffer () const;
    Diligent::ITextureView* ShaderView () const;
    Diligent::ITextureView* FilterableShaderView () const;
    Diligent::IShaderResourceBinding* Srb () const;

  private:
    struct Impl;
    Impl* impl_;
};

} // namespace archviz
} // namespace geomsrv

#endif
