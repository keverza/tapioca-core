#ifndef EVP_ARCHVIZ_SCENETEXTLIVECHECK_HPP
#define EVP_ARCHVIZ_SCENETEXTLIVECHECK_HPP

#include <cstdint>

namespace Diligent {
struct IDeviceContext;
struct IRenderDevice;
} // namespace Diligent

namespace geomsrv::archviz {

struct HudState;
class SceneTextLayer;

void DrawSceneTextLiveCheckControls (HudState& state);
void DrawSceneTextLiveCheck (SceneTextLayer& layer, Diligent::IRenderDevice* device,
                             Diligent::IDeviceContext* context, HudState& state, uint32_t width,
                             uint32_t height, float dpiScale);

} // namespace geomsrv::archviz

#endif
