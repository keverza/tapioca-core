#ifndef EVP_ARCHVIZ_DXGI_CAMERAANCHOR_HPP
#define EVP_ARCHVIZ_DXGI_CAMERAANCHOR_HPP

// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings and each cost
// at least one. The camera is `b1` and `b0` (finding 1); this only reads it.
//
// ArchViz/Dxgi/CameraAnchor -- where the census's anchor stands while it learns: on the
// centre ray of the camera the census is decoding.
//
// ⚠️ THE ANCHOR MUST BE ON SCREEN, AND ONLY THE CAMERA KNOWS WHERE THE SCREEN IS. The census
// admits a camera only if the anchor it scores lands inside the image in 95% of its samples
// (`Eligibility::minInsideClip`). The anchor was the model's bounding-box centre, or before
// extraction Archicad's stored view target -- neither of which follows the view. On a site
// a kilometre from the origin, orbited (2026-10-01 13:51-13:52, archviz.1.log), the anchor was
// inside in 17-69% of 469 samples, between 0.1 and 0.9 of the half-view off centre, and no
// camera was ever admitted. The point a camera puts at the centre of its own image is on
// screen by construction, and every model draw of a frame carries that same camera (finding
// 1), so a learning camera decoded from any of them places it.
//
// ⚠️ DEEP, BECAUSE THE ANCHOR IS PLACED FROM A CAMERA A FEW FRAMES OLDER THAN THE SAMPLES
// SCORED AGAINST IT. An orbit turns the view by an angle a frame about a pivot at some
// distance R; a point on the old centre ray at depth D then drifts off centre by that angle
// times |R/D - 1| -- at most the turn itself when D is past the pivot, and without bound as
// D shrinks. The same run orbited 34 degrees in nine frames about a pivot ~480 m out: an
// anchor placed on the first frame sat, on the tenth, 3.5 of the half-view off centre at
// 10 m, 1.1 at 146 m, 0.04 at 477 m and 0.38 at 1069 m. So it stands past any pivot the
// model can hold: a kilometre, or the model's farthest corner when that is farther.
//
// ⚠️ IT TESTS THE SAME THINGS. Only a pair that decodes as one camera (`cameralayout::Decode`)
// places an anchor; a screen map still fails `projectionDivides`, and a collapsing transform
// still fails area and edge -- those terms read the anchor's triangle, whose size here is a
// fixed share of its depth, so it is the same size on screen at any depth.

#include "ArchViz/Dxgi/CameraLayout.hpp"

#include <cmath>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace cameraanchor {

// The anchor's least depth: past the pivot of any orbit of a building or a site.
inline constexpr double kDeepMetres = 1000.0;
// And its most: half our far plane (`cameralayout::kFar`), so an element stranded far out --
// at survey coordinates, say -- cannot push it out of the depth range the gate checks.
inline constexpr double kDeepestMetres = cameralayout::kFar * 0.5;
// The anchor triangle's size as a share of its depth: about a twentieth of the view across.
inline constexpr double kSizeOfDepth = 0.05;

struct Anchor {
    double at[3] = {};
    double size = 0.0;  // metres
    double depth = 0.0; // metres from the eye along the view
};

// The point the camera `view` (b1) and `b0` puts at the centre of its image, `kDeepMetres`
// out or as far as the farthest corner of the model's bounds (`boundsMin`, `boundsMax`;
// nullptr when there are none), never past `kDeepestMetres`. False when the pair is not one
// camera.
inline bool OnCentreRay (const float view[16], const float b0[16], const double* boundsMin, const double* boundsMax,
                         Anchor& out)
{
    float decoded[16];
    if (cameralayout::Decode (view, b0, decoded) == cameralayout::Layout::Neither)
        return false;
    // The image is (p - eye) * b0 plus b0's last row: x and y stay at the centre along the
    // direction both their columns are blind to, and w -- the depth -- grows along it.
    double x[3], y[3], w[3];
    cameralayout::detail::Column (b0, 0, x);
    cameralayout::detail::Column (b0, 1, y);
    cameralayout::detail::Column (b0, 3, w);
    double ray[3] = { x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0] };
    const double length = std::sqrt (cameralayout::detail::Dot (ray, ray));
    if (!(length > 1e-12))
        return false;
    const double sign = cameralayout::detail::Dot (ray, w) < 0.0 ? -1.0 : 1.0;
    for (double& r : ray)
        r *= sign / length;

    double eye[3];
    cameralayout::Eye (view, eye);
    double depth = kDeepMetres;
    if (boundsMin != nullptr && boundsMax != nullptr) {
        for (int corner = 0; corner < 8; ++corner) {
            const double to[3] = { ((corner & 1) ? boundsMax[0] : boundsMin[0]) - eye[0],
                                   ((corner & 2) ? boundsMax[1] : boundsMin[1]) - eye[1],
                                   ((corner & 4) ? boundsMax[2] : boundsMin[2]) - eye[2] };
            const double distance = std::sqrt (cameralayout::detail::Dot (to, to));
            depth = distance > depth ? distance : depth;
        }
        depth = depth < kDeepestMetres ? depth : kDeepestMetres;
    }
    for (int i = 0; i < 3; ++i)
        out.at[i] = eye[i] + ray[i] * depth;
    out.depth = depth;
    out.size = depth * kSizeOfDepth;
    return true;
}

} // namespace cameraanchor
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
