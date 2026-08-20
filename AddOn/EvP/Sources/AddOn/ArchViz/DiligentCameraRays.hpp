#ifndef EVP_ARCHVIZ_DILIGENTCAMERARAYS_HPP
#define EVP_ARCHVIZ_DILIGENTCAMERARAYS_HPP

#include <cstdint>

namespace geomsrv::archviz {

class Camera;
class DiligentScene;

void SetDiligentCameraRays (DiligentScene& scene, const Camera& camera, uint32_t width, uint32_t height);

} // namespace geomsrv::archviz

#endif
