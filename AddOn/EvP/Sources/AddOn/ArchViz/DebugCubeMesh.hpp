#ifndef EVP_ARCHVIZ_DEBUGCUBEMESH_HPP
#define EVP_ARCHVIZ_DEBUGCUBEMESH_HPP

// ArchViz/DebugCubeMesh — the Phase 5 box's GEOMETRY, with no renderer attached.
//
// `DebugCube.cpp` built this inline and then handed it to bgfx in the same
// function, so the Diligent port could not reuse a single line of it. The
// geometry is the part worth keeping: it is 24 vertices rather than 8, wound
// counter-clockwise seen from outside, with true per-face normals — three
// properties the real extraction pipeline also has, and three that make a
// culling, winding or normal fault a DISTINGUISHABLE picture instead of one
// grey box.
//
// ⚠️ IT IS ALSO THE ONLY GEOMETRY THE VIEWER CAN DRAW WITHOUT ARCHICAD, which
// is what makes it the first thing to point a new renderer at. Keep it free of
// bgfx, Diligent, DG and the DevKit so both renderers and the offline tests can
// all use the same vertices.

#include "ArchViz/ArchVizVertex.hpp"

#include <cstddef>
#include <cstdint>

namespace geomsrv {
namespace archviz {
namespace debugcubemesh {

constexpr size_t kVertexCount = 24;   // 4 corners x 6 faces, sharing nothing
constexpr size_t kIndexCount  = 36;   // 2 triangles x 6 faces

// Half-extent in metres. The cube is 2 m across because a room is, and a viewer
// whose default scene is 1 unit wide teaches the wrong reflex about units.
constexpr float kHalfExtent = 1.0f;

// Which colours the faces carry.
//
// ⚠️ `Neutral` EXISTS BECAUSE THE COLOURS MADE THE SHADING UNREADABLE. Six
// strongly different hues let a user confirm orientation, but they make "are
// these two faces lit differently?" nearly impossible to answer by eye -- a
// dark red face at 71% brightness and a bright green one at 35% look about
// equally luminous. The first live run of the Diligent viewport hit exactly
// that and reported the shading as flat when it may not have been. With
// AxisGnomonMesh carrying the orientation question, the cube can be one grey
// and answer the lighting question cleanly instead.
enum class Palette {
    Colored,   // six named hues; the orientation instrument
    Neutral,   // one light grey; the shading instrument
};

// Fills caller-owned arrays of exactly the sizes above.
//
// ⚠️ Z-UP: +Z is the top face, as in Archicad. Every face is wound
// counter-clockwise seen from OUTSIDE, so with a right-handed view+projection
// the front faces are the outside ones. If a cull setting hides the outside of
// the box rather than its inside, the matrices are mirrored — look at
// ArchViz/MatrixMath, not at these indices.
void Build (ArchVizVertex vertices[kVertexCount], uint16_t indices[kIndexCount],
            Palette palette = Palette::Colored);

// The colour of face `index` in build order, ABGR (0xAABBGGRR). Exposed so a
// probe can ask the user to name a face by colour without the names living in
// two places -- a wrong name there turns a real answer into a wrong one.
// Order: +Z, -Z, +X, -X, +Y, -Y.
uint32_t FaceColorAbgr (int faceIndex);
const char* FaceName (int faceIndex);

}   // namespace debugcubemesh
}   // namespace archviz
}   // namespace geomsrv

#endif
