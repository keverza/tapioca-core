#ifndef EVP_ARCHVIZ_SCENETEXTLIVECHECK_HPP
#define EVP_ARCHVIZ_SCENETEXTLIVECHECK_HPP

#include <cstdint>

namespace Diligent {
struct IDeviceContext;
struct IRenderDevice;
struct ITextureView;
} // namespace Diligent

namespace geomsrv::archviz {

struct HudState;
class SceneTextLayer;

void DrawSceneTextLiveCheckControls (HudState& state);
void DrawSceneTextLiveCheck (SceneTextLayer& layer, Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                             HudState& state, uint32_t width, uint32_t height, float dpiScale);
void DrawSceneTextOcclusionLiveCheck (SceneTextLayer& layer, Diligent::IRenderDevice* device,
                                      Diligent::IDeviceContext* context, Diligent::ITextureView* depthView,
                                      HudState& state, const float placementViewProj[16], const float depthViewProj[16],
                                      uint32_t width, uint32_t height, float dpiScale, float nearClip, float farClip,
                                      bool perspective);

} // namespace geomsrv::archviz

#endif
