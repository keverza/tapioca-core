#include "SunStudy/SunStudyBounds.hpp"
#include "SunStudy/SunStudyRoles.hpp" // CanonicalGuid
#include "Geometry/Mesh.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace evp::sunstudy {

bool SnapshotBounds (const geomsrv::Snapshot& snapshot, double min[3], double max[3])
{
    bool any = false;
    for (const geomsrv::Mesh& mesh : snapshot.meshes)
        for (size_t v = 0; v + 2 < mesh.vertices.size (); v += 3) {
            for (int axis = 0; axis < 3; ++axis) {
                const double value = mesh.vertices[v + axis];
                min[axis] = any ? (std::min) (min[axis], value) : value;
                max[axis] = any ? (std::max) (max[axis], value) : value;
            }
            any = true;
        }
    return any;
}

BoundedRoles SplitByCylinder (const geomsrv::Snapshot& snapshot, const AnalysisCylinder& cylinder,
                              const std::vector<std::string>& context, const std::vector<std::string>& ignored)
{
    std::set<std::string> picked, skipped;
    for (const auto& guid : context)
        picked.insert (CanonicalGuid (guid));
    for (const auto& guid : ignored)
        skipped.insert (CanonicalGuid (guid));

    // One plan box per ELEMENT: an element may arrive as several meshes.
    struct Box {
        std::string guid;
        double lo[2], hi[2];
        bool any = false;
    };
    std::map<std::string, Box> boxes;
    for (const geomsrv::Mesh& mesh : snapshot.meshes) {
        Box& box = boxes[CanonicalGuid (mesh.guid)];
        if (box.guid.empty ())
            box.guid = mesh.guid;
        for (size_t v = 0; v + 2 < mesh.vertices.size (); v += 3) {
            for (int axis = 0; axis < 2; ++axis) {
                const double value = mesh.vertices[v + axis];
                box.lo[axis] = box.any ? (std::min) (box.lo[axis], value) : value;
                box.hi[axis] = box.any ? (std::max) (box.hi[axis], value) : value;
            }
            box.any = true;
        }
    }

    BoundedRoles roles;
    for (const auto& [canonical, box] : boxes) {
        if (!box.any || skipped.count (canonical) != 0)
            continue;
        // The box point nearest the centre decides whether the footprint reaches in.
        const double dx = std::clamp (cylinder.x, box.lo[0], box.hi[0]) - cylinder.x;
        const double dy = std::clamp (cylinder.y, box.lo[1], box.hi[1]) - cylinder.y;
        const bool inside = dx * dx + dy * dy <= cylinder.radius * cylinder.radius;
        (inside && picked.count (canonical) == 0 ? roles.analysis : roles.context).push_back (box.guid);
    }
    return roles;
}

} // namespace evp::sunstudy
