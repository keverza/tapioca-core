// ArchViz/Dxgi/InjectionCamera for the offline suite: the three setters the camera
// recognizer's commit calls (§4), recorded so a test can read what the injection was
// pointed at. The injection itself is Direct3D 11 and is not here.

#include "ArchViz/Dxgi/InjectionCamera.hpp"

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace injection {

namespace stub {
CameraSource source = CameraSource::None;
uint32_t interpretation = 0xffffffffu;
uint32_t occurrence = 0;
} // namespace stub

void SetCameraSource (CameraSource source)
{
    stub::source = source;
}

void SetExpectedInterpretation (uint32_t variant)
{
    stub::interpretation = variant;
}

void SetSelectedOccurrence (uint32_t index)
{
    stub::occurrence = index;
}

} // namespace injection
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
