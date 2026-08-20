#ifndef EVP_ARCHVIZ_AXISGNOMONMESH_HPP
#define EVP_ARCHVIZ_AXISGNOMONMESH_HPP

// ArchViz/AxisGnomonMesh — three arrows, so "which way is east" stops being a
// question anyone has to answer from memory.
//
// WHY IT EXISTS. The first live run of the Diligent viewport asked the user to
// read the model's orientation off the debug cube's face colours while it
// rotated, and the honest answer was that a rotating cube is a bad instrument
// for it. A mirrored image is the one rendering fault that otherwise looks
// perfectly fine, so the viewer needs a direction reference that is unambiguous
// at a glance and does not depend on remembering which face was painted which
// colour.
//
// ⚠️ THE COLOUR CONVENTION IS THE UNIVERSAL ONE, and it is load-bearing: X is
// RED, Y is GREEN, Z is BLUE. Every DCC and CAD package agrees, so it needs no
// legend. In Archicad's world that reads: RED points EAST, GREEN points NORTH,
// BLUE points UP. If red and green ever swap sides on screen, the image is
// mirrored -- and the mesh itself is guarded by tests, so the indictment falls
// on ArchViz/MatrixMath, not here.
//
// Free of bgfx, Diligent, DG and the DevKit, like DebugCubeMesh, so both
// renderers and the offline tests share one definition.

#include "ArchViz/ArchVizVertex.hpp"

#include <cstdint>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace axisgnomon {

// X red, Y green, Z blue -- ABGR (0xAABBGGRR), like every other colour in
// ArchViz. ⚠️ Red is the LAST byte pair; reading these as RGBA swaps X and Z,
// which would make the gnomon lie about exactly the thing it exists to settle.
constexpr uint32_t kAxisColorAbgr[3] = {
    0xff2020e0,   // +X east  -- red
    0xff20e020,   // +Y north -- green
    0xffe02020,   // +Z up    -- blue
};

// One solid arrow per axis: a square shaft and a pyramid head, both closed, so
// the gnomon shades and depth-tests like the rest of the scene rather than
// needing a line pipeline of its own.
//
// `length` is the tip-to-origin distance in metres. `thickness` is the shaft's
// half-width; the head is three times that and a quarter of the length.
//
// Appends to the vectors rather than clearing them, so a caller can build one
// buffer out of several meshes. Every triangle is wound counter-clockwise seen
// from outside, as DebugCubeMesh's are.
void Build (std::vector<ArchVizVertex>& vertices, std::vector<uint16_t>& indices,
            float length = 2.0f, float thickness = 0.03f);

const char* AxisName (int axis);   // 0,1,2 -> "+X east (red)" ...

}   // namespace axisgnomon
}   // namespace archviz
}   // namespace geomsrv

#endif
