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
    bool searchBestCascade = true;
    bool filterAcrossCascades = true;
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
