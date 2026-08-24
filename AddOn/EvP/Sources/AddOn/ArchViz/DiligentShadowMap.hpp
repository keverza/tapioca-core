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
    float fixedDepthBias = 0.0025f;
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

    bool Prepare (Diligent::IDeviceContext* context, const float view[16], const float projection[16],
                  const float towardSun[3]);
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
