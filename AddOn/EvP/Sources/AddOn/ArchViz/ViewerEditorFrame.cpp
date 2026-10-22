#include "ArchViz/ViewerEditor.hpp"
#include <cmath>

namespace geomsrv::archviz::vieweredit {
bool Frame::Project (double x, double y, double z, float& px, float& py) const
{
    const double point[] = { x, y, z, 1 };
    double camera[4] {}, clip[4] {};
    for (size_t column = 0; column < 4; ++column)
        for (size_t row = 0; row < 4; ++row)
            camera[column] += point[row] * view[row * 4 + column];
    for (size_t column = 0; column < 4; ++column)
        for (size_t row = 0; row < 4; ++row)
            clip[column] += camera[row] * projection[row * 4 + column];
    if (!width || !height || !std::isfinite (clip[3]) || clip[3] <= 1e-8 || clip[2] < 0 || clip[2] > clip[3])
        return false;
    px = float ((clip[0] / clip[3] + 1) * .5 * width);
    py = float ((1 - clip[1] / clip[3]) * .5 * height);
    return std::isfinite (px) && std::isfinite (py);
}
} // namespace geomsrv::archviz::vieweredit
