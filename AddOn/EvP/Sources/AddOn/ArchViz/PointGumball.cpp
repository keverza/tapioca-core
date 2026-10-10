#include "ArchViz/PointGumball.hpp"
#include <cmath>

namespace geomsrv::archviz {

bool PointGumballParameter (const double point[3], int axis, const double rayOrigin[3], const double rayDirection[3],
                            double parameter[3])
{
    double direction[3];
    double length = 0.0;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite (point[i]) || !std::isfinite (rayOrigin[i]) || !std::isfinite (rayDirection[i]))
            return false;
        length += rayDirection[i] * rayDirection[i];
        parameter[i] = 0.0;
    }
    if (!(length > 1.0e-12) || axis < 0 || axis > 3)
        return false;
    length = std::sqrt (length);
    for (int i = 0; i < 3; ++i)
        direction[i] = rayDirection[i] / length;
    if (axis == 3) {
        if (std::abs (direction[2]) < 1.0e-6)
            return false;
        const double t = (point[2] - rayOrigin[2]) / direction[2];
        if (t < 0.0)
            return false;
        parameter[0] = rayOrigin[0] + t * direction[0];
        parameter[1] = rayOrigin[1] + t * direction[1];
        return true;
    }
    const double denominator = 1.0 - direction[axis] * direction[axis];
    if (denominator < 1.0e-6)
        return false; // axis points into the camera: no stable screen-space move
    double alongRay = 0.0;
    for (int i = 0; i < 3; ++i)
        alongRay += (rayOrigin[i] - point[i]) * direction[i];
    parameter[axis] = (rayOrigin[axis] - point[axis] - direction[axis] * alongRay) / denominator;
    return direction[axis] * parameter[axis] - alongRay >= 0.0;
}

} // namespace geomsrv::archviz
