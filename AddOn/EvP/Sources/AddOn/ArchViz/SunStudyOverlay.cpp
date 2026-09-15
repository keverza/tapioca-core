#include "ArchViz/SunStudyOverlay.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv {
namespace archviz {

size_t SunStudyElementMap::Bytes () const
{
    return guid.capacity () + faces.capacity () * sizeof (SunFaceMap);
}

size_t SunStudyAtlasUpload::Bytes () const
{
    size_t bytes = studyId.capacity ();
    // ⚠️ THE IMAGE IS COUNTED ONCE HERE EVEN THOUGH IT IS SHARED. PendingBytes
    // exists so the producer can throttle itself, and under-reporting a 256 MB
    // atlas because a second owner exists is the opposite of what it is for.
    if (texels != nullptr)
        bytes += texels->capacity () * sizeof (float);
    for (const SunStudyElementMap& element : elements)
        bytes += sizeof (SunStudyElementMap) + element.Bytes ();
    return bytes;
}

bool BuildSunStudyElementMap (const std::vector<evp::sunstudy::AtlasTile>& tiles,
                              const std::vector<evp::sunstudy::FaceLayout>& layouts, double spacing,
                              const std::vector<uint32_t>& triangles, const std::vector<int32_t>& triMaterial,
                              uint32_t faceBase, SunStudyElementMap& out)
{
    out.faces.clear ();
    out.topologyHash = 0;

    if (!(spacing > 0.0) || tiles.empty ())
        return false;
    // One entry per face in each, or the two are not describing the same study.
    if (tiles.size () != layouts.size ())
        return false;

    const size_t triCount = triangles.size () / 3;
    if (triCount == 0)
        return false;
    // Off the end of the study's face list means the caller paired this element
    // with a study that did not measure it. Refused, not clamped: a clamp would
    // hand the last few triangles somebody else's tile.
    if (static_cast<size_t> (faceBase) + triCount > tiles.size ())
        return false;

    // ⚠️ THE PERMUTATION COMES FROM THE SAME FUNCTION THAT PRODUCED THE INDEX
    // BUFFER, not from a second sort written here. See MeshGroups.hpp.
    std::vector<uint32_t> indices;
    std::vector<MaterialRange> ranges;
    std::vector<uint32_t> order;
    BuildMaterialGroups (triangles, triMaterial, indices, ranges, nullptr, nullptr, &order);
    if (order.size () != triCount)
        return false;
    // The buffer the viewer would be drawing with if it holds the same
    // extraction -- which is exactly the question the render thread asks.
    out.topologyHash = MeshIndexHash (indices);

    const float invSpacing = static_cast<float> (1.0 / spacing);
    out.faces.resize (triCount);
    for (size_t i = 0; i < triCount; ++i) {
        const uint32_t face = faceBase + order[i];
        const evp::sunstudy::AtlasTile& tile = tiles[face];
        const evp::sunstudy::FaceLayout& layout = layouts[face];
        SunFaceMap& map = out.faces[i];

        // A face with no lattice keeps the default all-zero record: `tile.zw`
        // stays 0, which is the shader's "no sample here, shade normally".
        if (!tile.Placed () || !layout.gridded)
            continue;

        for (int axis = 0; axis < 3; ++axis) {
            map.originAndInvSpacing[axis] = static_cast<float> (layout.origin[axis]);
            map.uAxisAndStart[axis] = static_cast<float> (layout.uAxis[axis]);
            map.vAxisAndStart[axis] = static_cast<float> (layout.vAxis[axis]);
        }
        map.originAndInvSpacing[3] = invSpacing;
        map.uAxisAndStart[3] = static_cast<float> (layout.uStart);
        map.vAxisAndStart[3] = static_cast<float> (layout.vStart);
        map.tile[0] = static_cast<float> (tile.x);
        map.tile[1] = static_cast<float> (tile.y);
        map.tile[2] = static_cast<float> (tile.width);
        map.tile[3] = static_cast<float> (tile.height);
    }
    return true;
}

SunFaceBinding ClassifySunFaceBinding (const SunStudyElementMap& map, const SceneElementFacts& element)
{
    // ⚠️ AN EMPTY SIDE CAR IS "NOT RECEIVED", NOT "REFUSED". A study that
    // built no faces for an element never described it, so there is nothing to
    // disagree with -- reporting a refusal would send a reader hunting for a
    // model mismatch that does not exist.
    if (!element.present || map.faces.empty ())
        return SunFaceBinding::NotYetReceived;
    if (map.faces.size () != static_cast<size_t> (element.triangleCount))
        return SunFaceBinding::RefusedTriangleCount;
    if (map.topologyHash != element.topologyHash)
        return SunFaceBinding::RefusedTopologyHash;
    // ⚠️ THE TWO CHECKS COME FIRST, AND THE ORDER MATTERS. An already-bound
    // buffer whose element has since been re-extracted must be RELEASED, not
    // kept -- so "already bound" can only be answered after the element has been
    // proved to still be the one the study measured.
    return element.alreadyBound ? SunFaceBinding::AlreadyBound : SunFaceBinding::Attach;
}

int64_t SunStudyTexelAt (const SunFaceMap& face, const double point[3], uint32_t atlasWidth, uint32_t atlasHeight)
{
    if (point == nullptr || atlasWidth == 0 || atlasHeight == 0)
        return -1;
    if (!(face.tile[2] > 0.0f) || !(face.tile[3] > 0.0f))
        return -1;

    const double relative[3] = { point[0] - face.originAndInvSpacing[0], point[1] - face.originAndInvSpacing[1],
                                 point[2] - face.originAndInvSpacing[2] };
    const double u =
        relative[0] * face.uAxisAndStart[0] + relative[1] * face.uAxisAndStart[1] + relative[2] * face.uAxisAndStart[2];
    const double v =
        relative[0] * face.vAxisAndStart[0] + relative[1] * face.vAxisAndStart[1] + relative[2] * face.vAxisAndStart[2];

    const double cellU = std::floor (u * face.originAndInvSpacing[3]) - face.uAxisAndStart[3];
    const double cellV = std::floor (v * face.originAndInvSpacing[3]) - face.vAxisAndStart[3];

    // Clamped to the CELLS, not to the tile's edge: the last valid cell is
    // width - 1, and one past it is the gutter the atlas packs between tiles.
    const double column = std::min (std::max (cellU, 0.0), static_cast<double> (face.tile[2]) - 1.0);
    const double row = std::min (std::max (cellV, 0.0), static_cast<double> (face.tile[3]) - 1.0);

    const int64_t x = static_cast<int64_t> (face.tile[0] + column);
    const int64_t y = static_cast<int64_t> (face.tile[1] + row);
    if (x < 0 || y < 0 || x >= static_cast<int64_t> (atlasWidth) || y >= static_cast<int64_t> (atlasHeight))
        return -1;
    return y * static_cast<int64_t> (atlasWidth) + x;
}

} // namespace archviz
} // namespace geomsrv
