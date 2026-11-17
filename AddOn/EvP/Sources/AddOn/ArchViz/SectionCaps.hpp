#ifndef EVP_ARCHVIZ_SECTIONCAPS_HPP
#define EVP_ARCHVIZ_SECTIONCAPS_HPP

// ArchViz/SectionCaps -- the section plane's cap fill, hatch and section line (issue #14 P2).
//
// The clip (SectionPlane.hpp) removes everything beyond the plane; this closes the cut. Every
// element the plane crosses is cut with the SAME kernel the storey slices and the plan cut use
// (Geometry/SliceEngine `SliceMesh`); its closed loops are resolved even-odd -- holes and islands
// included -- by Clipper2, and the result is three filled meshes drawn through StorySliceLayers:
//
//     poche   the cut solid, inset by the section line
//     hatch   optional parallel strokes clipped to the poche
//     line    the section line: the band between the cut's boundary and its inset
//
// ⚠️ THE SECTION LINE IS A WORLD-WIDTH BAND INSIDE THE CUT, NOT A PIXEL RIBBON ON ITS EDGE. The
// first version stroked the boundary with the storey slices' pixel ribbon: centred on the edge it
// made every cut solid look half a line fatter, its width grew against the model when zooming
// out, and its per-segment quads with square caps overlapped at every corner. The user's verdict
// from the 2026-11-17 live run asked for the drafting convention instead: a line of constant MODEL
// width drawn inside the solid. So the line is `region minus region-inset-by-width`, computed by
// Clipper2's offset with MITRE joins -- one polygon per contour, clean corners, never overlapping,
// and a solid thinner than two widths becomes all line, as on a drawing.
//
// ⚠️ CLOSED, OPEN AND FAILED ARE COUNTED, NEVER GUESSED. A closed loop is filled. An open chain --
// a mesh that is not watertight at the plane, which Archicad's tessellation produces -- has no
// inside, so it gets a centred line of the same width and NO fill, and is counted, because closing
// it would invent a solid. A fill the triangulator refuses is counted too. The HUD prints all
// three, so an unsuitable mesh fails visibly.
//
// ⚠️ BUILT IN THE PLANE'S OWN FRAME. The kernels cut at z = const and offset in xy, so a vertical
// plane is handled by rotating the mesh so the plane's axis becomes z (a CYCLIC permutation, which
// keeps handedness), cutting there, and drawing with `viewProj` premultiplied by
// SectionLocalToWorld. The hatch is anchored in that frame's ABSOLUTE coordinates, so strokes
// line up across neighbouring elements.
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

// What the cut's drawables are SHAPED like: a change here is a rebuild, a colour change is not.
// Lengths in world metres, the angle in degrees from the plane frame's u axis.
struct SectionCapShape {
    float lineWidth = 0.02f;
    bool hatch = false;
    float hatchSpacing = 0.15f;
    float hatchAngleDegrees = 45.0f;
    float hatchWidth = 0.012f;
};

// The HUD's look for the cut; colours are sRGB 0xRRGGBBAA like the storey slices'.
struct SectionCapStyle {
    // ⚠️ THE BACK FACES ARE THE CAP THAT CANNOT FAIL. With the clip on, the inside of every cut
    // solid shows its back faces -- culled by default, which reads as a model made of paper
    // (2026-11-17 live run). Drawn flat in `fillRgba` they close any watertight solid exactly
    // and instantly, before the caps' worker lands and whether or not caps are on at all.
    bool backFaces = true;
    bool caps = true;
    uint32_t fillRgba = 0x4A4A4AFFu;  // poche
    uint32_t lineRgba = 0x141414FFu;  // the section line
    uint32_t hatchRgba = 0x1E1E1EFFu; // the hatch strokes
    SectionCapShape shape;
};

struct SectionCapRequest {
    SectionAxis axis = SectionAxis::Z;
    float coordinate = 0.0f; // world, along the axis
    bool flipped = false;    // which side is kept: the tangency nudge goes into it
    SectionCapShape shape;
    std::vector<std::shared_ptr<const SectionMesh>> meshes;
};

// What a build found, for the HUD (DiligentSceneStats::sectionCaps).
struct SectionCapCounts {
    uint32_t elementsCut = 0;  // elements with at least one chain
    uint32_t closedLoops = 0;  // filled
    uint32_t openChains = 0;   // lined, not filled
    uint32_t fillFailures = 0; // elements whose region, inset, hatch or line the geometry refused
    double areaM2 = 0.0;       // the cut solids' area, line included
    bool building = false;     // a newer plane is being cut; the drawn caps are the previous one
};

// One filled drawable, in the PLANE's frame (z = the cut); draw with SectionLocalToWorld.
struct SectionCapMesh {
    std::vector<StorySliceFillVertex> vertices;
    std::vector<uint32_t> indices;
};

struct SectionCapResult {
    SectionCapMesh poche;
    SectionCapMesh hatch;
    SectionCapMesh line;
    SectionCapCounts counts;
};

SectionCapResult BuildSectionCaps (const SectionCapRequest& request);

// Triangle area of a cap mesh, for tests and the HUD.
double SectionCapMeshArea (const SectionCapMesh& mesh);

// World xyz -> the plane's frame (u, v, along-axis), and the row-vector matrix back.
void ToSectionLocal (SectionAxis axis, const float world[3], double out[3]);
void SectionLocalToWorld (SectionAxis axis, float out[16]);

} // namespace archviz
} // namespace geomsrv

#endif
