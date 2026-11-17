#ifndef EVP_ARCHVIZ_SECTIONCAPS_HPP
#define EVP_ARCHVIZ_SECTIONCAPS_HPP

// ArchViz/SectionCaps -- the section plane's cap fill and section curves (issue #14 P2).
//
// The clip (SectionPlane.hpp) removes everything beyond the plane; this closes the cut. Every
// element the plane crosses is cut with the SAME kernel the storey slices and the plan cut use
// (Geometry/SliceEngine `SliceMesh`), its closed loops filled even-odd -- holes and islands
// included -- by the same Clipper2 path (`AppendIndexedSliceFill`), and every loop outlined by
// the same pixel-width ribbon (`BuildSliceRibbon`). The result draws through a StorySliceLayer.
//
// ⚠️ CLOSED, OPEN AND FAILED ARE COUNTED, NEVER GUESSED. A closed loop is filled. An open chain --
// a mesh that is not watertight at the plane, which Archicad's tessellation produces -- is OUTLINED
// BUT NOT FILLED and counted, because closing it would invent a solid. A fill the triangulator
// refuses is counted too. The HUD prints all three, so an unsuitable mesh fails visibly.
//
// ⚠️ BUILT IN THE PLANE'S OWN FRAME. The kernels cut at z = const and push ribbons in xy, so a
// vertical plane is handled by rotating the mesh so the plane's axis becomes z (a CYCLIC
// permutation, which keeps handedness), cutting there, and drawing with `viewProj` premultiplied
// by SectionLocalToWorld. Nothing downstream needs to know the plane was vertical.
//
// ⚠️ THE MESHES ARE THE RENDERER'S FLOATS, NOT THE EXTRACTION'S DOUBLES, ON PURPOSE: the cap must
// meet the geometry the GPU draws, and that is the float copy. The weld is widened to the float
// grid at the element's coordinates for the same reason.
//
// PURE: no Diligent -- runs on a worker (DiligentSceneSection.cpp) and offline (test_sectioncaps.cpp).

#include "ArchViz/SectionPlane.hpp"
#include "ArchViz/StorySliceGeometry.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace geomsrv {
namespace archviz {

// One element's triangles as the renderer holds them: xyz per vertex, 3 indices per triangle.
struct SectionMesh {
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    float boundsMin[3] = { 0.0f, 0.0f, 0.0f };
    float boundsMax[3] = { 0.0f, 0.0f, 0.0f };
};

// The HUD's look for the caps; colours are sRGB 0xRRGGBBAA like the storey slices'.
struct SectionCapStyle {
    bool caps = true;
    uint32_t fillRgba = 0x4A4A4AFFu; // poche: solid, dark
    uint32_t lineRgba = 0x141414FFu; // the section line
    float linePixels = 1.5f;
};

struct SectionCapRequest {
    SectionAxis axis = SectionAxis::Z;
    float coordinate = 0.0f; // world, along the axis
    bool flipped = false;    // which side is kept: the tangency nudge goes into it
    std::vector<std::shared_ptr<const SectionMesh>> meshes;
};

// What a build found, for the HUD (DiligentSceneStats::sectionCaps).
struct SectionCapCounts {
    uint32_t elementsCut = 0;  // elements with at least one chain
    uint32_t closedLoops = 0;  // filled
    uint32_t openChains = 0;   // outlined, not filled
    uint32_t fillFailures = 0; // elements whose closed loops the triangulator refused
    double areaM2 = 0.0;       // the filled area
    bool building = false;     // a newer plane is being cut; the drawn caps are the previous one
};

struct SectionCapResult {
    // In the PLANE's frame (z = the cut); draw with SectionLocalToWorld.
    std::vector<StorySliceVertex> outline;
    std::vector<StorySliceFillVertex> fill;
    std::vector<uint32_t> fillIndices;
    SectionCapCounts counts;
};

SectionCapResult BuildSectionCaps (const SectionCapRequest& request);

// World xyz -> the plane's frame (u, v, along-axis), and the row-vector matrix back.
void ToSectionLocal (SectionAxis axis, const float world[3], double out[3]);
void SectionLocalToWorld (SectionAxis axis, float out[16]);

} // namespace archviz
} // namespace geomsrv

#endif
