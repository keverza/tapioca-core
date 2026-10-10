#ifndef EVP_ARCHVIZ_VIEWPOINTPROJECTION_HPP
#define EVP_ARCHVIZ_VIEWPOINTPROJECTION_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace geomsrv::archviz {
struct ViewpointScreenPolygon {
    std::array<std::array<float, 2>, 12> points {};
    size_t count = 0;
};

// Clip before perspective divide: a close-zoomed area remains visible even if
// its centre or one radial endpoint lies behind the near plane/off screen.
ViewpointScreenPolygon ProjectViewpointTriangle (const double a[3], const double b[3], const double c[3],
                                                 const float matrix[16], uint32_t width, uint32_t height);
} // namespace geomsrv::archviz
#endif
