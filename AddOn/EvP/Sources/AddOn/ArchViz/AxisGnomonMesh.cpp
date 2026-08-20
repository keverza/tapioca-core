#include "ArchViz/AxisGnomonMesh.hpp"

#include <cmath>

namespace geomsrv {
namespace archviz {
namespace axisgnomon {

namespace {

// A right-handed local frame per axis, so the SAME local geometry can be
// emitted three times without any of the three coming out mirrored.
//
// ⚠️ (u x v) MUST EQUAL d. A permutation of the axes that gets this wrong is a
// reflection (determinant -1): it reverses every triangle's winding while
// leaving the shape looking correct, which is the same class of bug as the
// (x,y,z)->(x,z,y) "axis swap" that has shipped in this repo before. Each row
// below is checked by test_axisgnomonmesh.cpp rather than by eye.
struct Frame {
    float u[3];
    float v[3];
    float d[3];
};

constexpr Frame kFrames[3] = {
    {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},   // +X
    {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},   // +Y
    {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},   // +Z
};

constexpr const char* kAxisNames[3] = {
    "+X east (red)",
    "+Y north (green)",
    "+Z up (blue)",
};

// local (lu, lv, ld) -> world, through the axis's frame.
void ToWorld (const Frame& f, float lu, float lv, float ld, float out[3])
{
    for (int i = 0; i < 3; ++i)
        out[i] = f.u[i] * lu + f.v[i] * lv + f.d[i] * ld;
}

void PushVertex (std::vector<ArchVizVertex>& vertices, const float p[3], const float n[3],
                 uint32_t abgr)
{
    vertices.push_back (ArchVizVertex {p[0], p[1], p[2], n[0], n[1], n[2], abgr});
}

// One flat quad, four vertices sharing the face's normal (never averaged -- the
// same rule DebugCubeMesh follows and for the same reason). Corners must be
// given counter-clockwise seen from OUTSIDE.
void PushQuad (std::vector<ArchVizVertex>& vertices, std::vector<uint16_t>& indices,
               const float a[3], const float b[3], const float c[3], const float d[3],
               const float n[3], uint32_t abgr)
{
    const uint16_t base = uint16_t (vertices.size ());
    PushVertex (vertices, a, n, abgr);
    PushVertex (vertices, b, n, abgr);
    PushVertex (vertices, c, n, abgr);
    PushVertex (vertices, d, n, abgr);
    indices.push_back (uint16_t (base + 0));
    indices.push_back (uint16_t (base + 1));
    indices.push_back (uint16_t (base + 2));
    indices.push_back (uint16_t (base + 0));
    indices.push_back (uint16_t (base + 2));
    indices.push_back (uint16_t (base + 3));
}

void PushTriangle (std::vector<ArchVizVertex>& vertices, std::vector<uint16_t>& indices,
                   const float a[3], const float b[3], const float c[3], uint32_t abgr)
{
    // The face normal comes from the winding rather than being passed in: for a
    // pyramid's slanted sides there is no axis to name, and deriving it here
    // makes the normal and the winding incapable of disagreeing.
    const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    float n[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                  e1[2] * e2[0] - e1[0] * e2[2],
                  e1[0] * e2[1] - e1[1] * e2[0]};
    const float length = std::sqrt (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (length > 0.0f) {
        n[0] /= length;
        n[1] /= length;
        n[2] /= length;
    }
    const uint16_t base = uint16_t (vertices.size ());
    PushVertex (vertices, a, n, abgr);
    PushVertex (vertices, b, n, abgr);
    PushVertex (vertices, c, n, abgr);
    indices.push_back (uint16_t (base + 0));
    indices.push_back (uint16_t (base + 1));
    indices.push_back (uint16_t (base + 2));
}

void BuildArrow (std::vector<ArchVizVertex>& vertices, std::vector<uint16_t>& indices,
                 int axis, float length, float thickness)
{
    const Frame& f = kFrames[axis];
    const uint32_t abgr = kAxisColorAbgr[axis];

    const float headLength = length * 0.25f;
    const float shaftLength = length - headLength;
    const float shaft = thickness;
    const float head = thickness * 3.0f;

    // ---- the shaft: a square prism from the origin to shaftLength -----------
    // Corners of the cross-section, counter-clockwise in the (u, v) plane seen
    // from +d, so a side quad taken in order (near_i, near_i+1, far_i+1, far_i)
    // faces outward.
    const float cornerU[4] = {-shaft, shaft, shaft, -shaft};
    const float cornerV[4] = {-shaft, -shaft, shaft, shaft};

    float nearCorner[4][3];
    float farCorner[4][3];
    for (int i = 0; i < 4; ++i) {
        ToWorld (f, cornerU[i], cornerV[i], 0.0f, nearCorner[i]);
        ToWorld (f, cornerU[i], cornerV[i], shaftLength, farCorner[i]);
    }

    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) % 4;
        // The outward normal of this side is the cross-section edge's outward
        // direction: the midpoint of the edge, in the (u, v) plane.
        float sideNormal[3];
        ToWorld (f, (cornerU[i] + cornerU[j]) * 0.5f, (cornerV[i] + cornerV[j]) * 0.5f,
                 0.0f, sideNormal);
        const float len = std::sqrt (sideNormal[0] * sideNormal[0] +
                                     sideNormal[1] * sideNormal[1] +
                                     sideNormal[2] * sideNormal[2]);
        if (len > 0.0f) {
            sideNormal[0] /= len;
            sideNormal[1] /= len;
            sideNormal[2] /= len;
        }
        PushQuad (vertices, indices, nearCorner[i], nearCorner[j], farCorner[j], farCorner[i],
                  sideNormal, abgr);
    }

    // The cap at the origin end, facing -d. Reversed order so it faces outward.
    float backNormal[3];
    ToWorld (f, 0.0f, 0.0f, -1.0f, backNormal);
    PushQuad (vertices, indices, nearCorner[3], nearCorner[2], nearCorner[1], nearCorner[0],
              backNormal, abgr);

    // ---- the head: a pyramid from shaftLength to the tip --------------------
    const float baseU[4] = {-head, head, head, -head};
    const float baseV[4] = {-head, -head, head, head};
    float baseCorner[4][3];
    for (int i = 0; i < 4; ++i)
        ToWorld (f, baseU[i], baseV[i], shaftLength, baseCorner[i]);
    float tip[3];
    ToWorld (f, 0.0f, 0.0f, length, tip);

    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) % 4;
        PushTriangle (vertices, indices, baseCorner[i], baseCorner[j], tip, abgr);
    }
    // The head's underside, facing back down the shaft.
    PushQuad (vertices, indices, baseCorner[3], baseCorner[2], baseCorner[1], baseCorner[0],
              backNormal, abgr);
}

}   // namespace

void Build (std::vector<ArchVizVertex>& vertices, std::vector<uint16_t>& indices,
            float length, float thickness)
{
    for (int axis = 0; axis < 3; ++axis)
        BuildArrow (vertices, indices, axis, length, thickness);
}

const char* AxisName (int axis)
{
    if (axis < 0 || axis >= 3)
        return "";
    return kAxisNames[axis];
}

}   // namespace axisgnomon
}   // namespace archviz
}   // namespace geomsrv
