#include "ArchViz/DebugCubeMesh.hpp"

namespace geomsrv {
namespace archviz {
namespace debugcubemesh {

namespace {

struct Face {
    float nx, ny, nz;
    uint32_t abgr;
    const char* name;
};

// A different colour per face, so a wrong winding, a wrong normal and a wrong
// camera are three DISTINGUISHABLE pictures rather than one grey box.
//
// ⚠️ ABGR, as bgfx packs Color0 and as the HLSL vertex layout reads it —
// 0xAABBGGRR, so the RED byte is the LAST pair, not the first. Reading these as
// RGBA names three of the six faces wrongly, which is worse than not naming
// them: a probe asks the user to identify a face BY COLOUR to detect a mirrored
// image, and a wrong name turns a real answer into a wrong one. The names below
// are the decoded values, checked byte by byte.
//
// The axis convention is Archicad's: +X east, +Y north, +Z up.
constexpr Face kFaces[6] = {
    { 0.0f,  0.0f,  1.0f, 0xff4f9fdf, "+Z top (orange)"      },
    { 0.0f,  0.0f, -1.0f, 0xff2f4f7f, "-Z bottom (brown)"    },
    { 1.0f,  0.0f,  0.0f, 0xff4fdf7f, "+X east (green)"      },
    {-1.0f,  0.0f,  0.0f, 0xff2f7f4f, "-X west (dark green)" },
    { 0.0f,  1.0f,  0.0f, 0xff4f4fdf, "+Y north (red)"       },
    { 0.0f, -1.0f,  0.0f, 0xff2f2f7f, "-Y south (dark red)"  },
};

// Two in-plane axes per face, chosen so (u x v) == n. That keeps every face
// counter-clockwise seen from outside, which is what makes a winding error show
// up as a MISSING face rather than as nothing at all.
constexpr float kAxes[6][6] = {
    {  1, 0, 0,   0, 1, 0 },   // +Z
    { -1, 0, 0,   0, 1, 0 },   // -Z
    {  0, 1, 0,   0, 0, 1 },   // +X
    {  0,-1, 0,   0, 0, 1 },   // -X
    { -1, 0, 0,   0, 0, 1 },   // +Y
    {  1, 0, 0,   0, 0, 1 },   // -Y
};

// One light grey, chosen so the ambient floor (0.35) is clearly darker than a
// fully lit face without either end clipping.
constexpr uint32_t kNeutralAbgr = 0xffd0d0d0;

}   // namespace

void Build (ArchVizVertex vertices[kVertexCount], uint16_t indices[kIndexCount],
            Palette palette)
{
    uint16_t vi = 0;
    uint16_t ii = 0;
    for (int f = 0; f < 6; ++f) {
        const Face& face = kFaces[f];
        const uint32_t abgr = palette == Palette::Neutral ? kNeutralAbgr : face.abgr;
        const float* u = &kAxes[f][0];
        const float* v = &kAxes[f][3];
        const float c[3] = {face.nx * kHalfExtent, face.ny * kHalfExtent, face.nz * kHalfExtent};

        const uint16_t base = vi;
        for (int corner = 0; corner < 4; ++corner) {
            const float su = (corner == 0 || corner == 3) ? -1.0f : 1.0f;
            const float sv = (corner < 2) ? -1.0f : 1.0f;
            vertices[vi++] = {
                c[0] + u[0] * su * kHalfExtent + v[0] * sv * kHalfExtent,
                c[1] + u[1] * su * kHalfExtent + v[1] * sv * kHalfExtent,
                c[2] + u[2] * su * kHalfExtent + v[2] * sv * kHalfExtent,
                face.nx, face.ny, face.nz,
                abgr,
            };
        }
        indices[ii++] = uint16_t (base + 0);
        indices[ii++] = uint16_t (base + 1);
        indices[ii++] = uint16_t (base + 2);
        indices[ii++] = uint16_t (base + 0);
        indices[ii++] = uint16_t (base + 2);
        indices[ii++] = uint16_t (base + 3);
    }
}

uint32_t FaceColorAbgr (int faceIndex)
{
    if (faceIndex < 0 || faceIndex >= 6)
        return 0u;
    return kFaces[faceIndex].abgr;
}

const char* FaceName (int faceIndex)
{
    if (faceIndex < 0 || faceIndex >= 6)
        return "";
    return kFaces[faceIndex].name;
}

}   // namespace debugcubemesh
}   // namespace archviz
}   // namespace geomsrv
