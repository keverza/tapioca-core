#ifndef EVP_ARCHVIZ_SECTIONPLANE_HPP
#define EVP_ARCHVIZ_SECTIONPLANE_HPP

// ArchViz/SectionPlane -- the viewer's movable section plane (issue #14 P2).
//
// One axis-aligned plane through the model, movable along its axis, flippable and resettable.
// Everything on the cut-away side is removed by SV_ClipDistance0 in every vertex stage that
// feeds the rasterizer (DiligentSceneConstants::sectionPlane), so the image, the G-buffer, the
// shadows, the AO prepass, the outlines, the edges and the id pass agree: what is not drawn
// cannot be picked, cast a shadow or occlude.
//
// ⚠️ THE POSITION IS A FRACTION OF THE MODEL'S EXTENT, NOT METRES. The HUD does not know the
// model's bounds and a metre slider is wrong at every scale but one; 0..1 along the axis is
// right for a pavilion and a city block alike, and "reset" is simply the middle. The scene turns
// it into world metres against its own bounds every frame.
//
// ⚠️ A NEW CAPABILITY, NOT THE PLAN CUT. ViewerPlanMode's cut is the storey's cut, computed by
// the extraction at a fixed height and drawn as lines; this removes geometry at any position on
// any axis. Caps and section curves are the next step (they need the triangles on the CPU, which
// the renderer does not keep today), so the cut face is currently open: you see into the solid.
//
// PURE: no Diligent -- tested offline (test_sectionplane.cpp).

#include <cstdint>

namespace geomsrv {
namespace archviz {

enum class SectionAxis : uint8_t {
    X = 0, // a vertical cut facing east-west
    Y = 1, // a vertical cut facing north-south
    Z = 2, // a horizontal cut: the plan cut at any height
};

struct SectionPlaneState {
    bool enabled = false;
    SectionAxis axis = SectionAxis::Z;
    // 0 = the model's minimum along the axis, 1 = its maximum.
    float position = 0.5f;
    // Unflipped keeps the LOW side (below a Z cut, west of an X cut, south of a Y cut) -- the
    // usual reading: lift the roof off, look in. Flipped keeps the high side.
    bool flipped = false;
};

// The plane as the shader wants it: keep where dot(xyz, p) + w >= 0. A disabled state, or bounds
// with no extent, yields (0,0,0,1): nothing is cut.
void SectionPlaneEquation (const SectionPlaneState& state, const float boundsMin[3], const float boundsMax[3],
                           float out[4]);

// The world coordinate along the axis the plane sits at, for the HUD's readout.
float SectionPlaneCoordinate (const SectionPlaneState& state, const float boundsMin[3], const float boundsMax[3]);

const char* SectionAxisName (SectionAxis axis);

} // namespace archviz
} // namespace geomsrv

#endif
