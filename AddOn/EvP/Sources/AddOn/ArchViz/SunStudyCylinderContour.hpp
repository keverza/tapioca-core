#pragma once

#include <array>
#include <set>
#include <vector>

namespace geomsrv::archviz {
struct SectionMesh;

struct SunCylinder {
    double x, y, radius, floor, top;
};

using CylinderPoint = std::array<float, 3>;
using CylinderSegment = std::array<CylinderPoint, 2>;

// Ordered endpoints weld on the renderer's float grid, including reversed duplicate faces.
struct CylinderContour {
    std::set<CylinderSegment> segments;
    bool refused = false;
    void Append (const SectionMesh& mesh, const SunCylinder& cylinder);
};
} // namespace geomsrv::archviz
