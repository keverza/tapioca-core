#pragma once

#include <array>
#include <algorithm>
#include <memory>
#include <set>
#include <vector>

namespace geomsrv::archviz {
struct SectionMesh;

struct SunCylinder {
    double x, y, radius, floor, top;
    bool operator== (const SunCylinder&) const = default;
};

using CylinderPoint = std::array<float, 3>;
using CylinderSegment = std::array<CylinderPoint, 2>;

// Retaining the meshes prevents an upsert from recycling an address in the key.
// The camera is deliberately absent: orbiting only redraws the uploaded ribbon.
struct CylinderContourKey {
    SunCylinder cylinder {};
    std::array<float, 4> plane {};
    std::vector<std::shared_ptr<const SectionMesh>> meshes;

    template <class Range> bool Update (const SunCylinder& next, const float clip[4], Range&& visible)
    {
        if (cylinder == next && std::equal (plane.begin (), plane.end (), clip) && std::ranges::equal (meshes, visible))
            return false;
        cylinder = next;
        std::copy_n (clip, 4, plane.begin ());
        meshes.assign (visible.begin (), visible.end ());
        return true;
    }
};

// Ordered endpoints weld on the renderer's float grid, including reversed duplicate faces.
struct CylinderContour {
    std::set<CylinderSegment> segments;
    bool refused = false;
    void Append (const SectionMesh& mesh, const SunCylinder& cylinder);
};
} // namespace geomsrv::archviz
