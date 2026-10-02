#ifndef EVP_ARCHVIZ_DXGI_CAMERASHAPE_HPP
#define EVP_ARCHVIZ_DXGI_CAMERASHAPE_HPP

// ArchViz/Dxgi/CameraShape -- what a `b1`/`b0` pair says about the camera it draws with, in
// terms that can be held against Archicad's own projection settings: how far above the
// horizon it looks, how wide, and whether its frustum is OFF AXIS -- the lens shift a
// two-point perspective needs to keep verticals vertical while the target stays where it was.
//
// ⚠️ READ private/docs/architecture/diligent/OVERLAY-INVARIANTS.md BEFORE EDITING. This
// DESCRIBES a pair and decides nothing: `cameralayout::Decode` is what accepts or refuses one
// (§1, findings 1 and 2). It reports Decode's terms beside its verdict, so a refusal can be
// read rather than inferred (§7).
//
// Pure and header-only, so tests/cpp pins it.

#include "ArchViz/Dxgi/CameraLayout.hpp"

#include <cmath>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace camerashape {

struct Shape {
    bool view = false;               // `cameralayout::IsView (b1)`
    bool rotationProjection = false; // `cameralayout::IsRotationProjection (b0)`
    bool decodes = false;            // `cameralayout::Decode` took the pair as one camera
    double mismatch = 0.0;           // `cameralayout::RotationMismatch`: b0's view axis against b1's
    // Degrees above the horizon each one looks along, model Z up. b0's axis is its w column;
    // b1's is its negated column 2, which a camera that decodes has equal to it.
    double pitchB0 = 0.0;
    double pitchB1 = 0.0;
    // How far the frustum is moved off its axis, in NDC: the part of the x and y columns
    // along the w column. Zero for a centred perspective; a two-point one keeps its view axis
    // level and moves the image vertically instead of tilting it.
    double shiftX = 0.0, shiftY = 0.0;
    double fovX = 0.0, fovY = 0.0; // degrees, the full width and height
    double eye[3] = {};
};

namespace detail {

constexpr double kDegrees = 57.29577951308232;

inline double PitchOf (const double axis[3])
{
    const double length = std::sqrt (cameralayout::detail::Dot (axis, axis));
    if (!(length > 1e-12))
        return 0.0;
    const double sine = axis[2] / length;
    return std::asin (sine > 1.0 ? 1.0 : (sine < -1.0 ? -1.0 : sine)) * kDegrees;
}

} // namespace detail

inline Shape Of (const float view[16], const float b0[16])
{
    namespace cl = cameralayout;
    Shape shape;
    shape.view = cl::IsView (view);
    shape.rotationProjection = cl::IsRotationProjection (b0);
    shape.mismatch = cl::RotationMismatch (b0, view);
    float decoded[16];
    shape.decodes = cl::Decode (view, b0, decoded) != cl::Layout::Neither;
    cl::Eye (view, shape.eye);

    double w[3], x[3], y[3], back[3];
    cl::detail::Column (b0, 3, w);
    cl::detail::Column (b0, 0, x);
    cl::detail::Column (b0, 1, y);
    cl::detail::Column (view, 2, back);
    const double forward1[3] = { -back[0], -back[1], -back[2] };
    shape.pitchB0 = detail::PitchOf (w);
    shape.pitchB1 = detail::PitchOf (forward1);

    const double ww = cl::detail::Dot (w, w);
    if (!(ww > 1e-12))
        return shape;
    shape.shiftX = cl::detail::Dot (x, w) / ww;
    shape.shiftY = cl::detail::Dot (y, w) / ww;
    const double length = std::sqrt (ww);
    const double axis[3] = { w[0] / length, w[1] / length, w[2] / length };
    double xs[3], ys[3];
    cl::detail::Reject (x, axis, xs);
    cl::detail::Reject (y, axis, ys);
    const double xLength = std::sqrt (cl::detail::Dot (xs, xs));
    const double yLength = std::sqrt (cl::detail::Dot (ys, ys));
    if (xLength > 1e-12)
        shape.fovX = 2.0 * std::atan (length / xLength) * detail::kDegrees;
    if (yLength > 1e-12)
        shape.fovY = 2.0 * std::atan (length / yLength) * detail::kDegrees;
    return shape;
}

// Where a model point lands through the pair -- `(p - eye) * b0`, the image the overlay
// draws (finding 2) -- in NDC. False when the point is not in front of the eye.
inline bool ToNdc (const float view[16], const float b0[16], const double point[3], double ndc[2])
{
    double m[16];
    cameralayout::ViewProjection (b0, view, m);
    double clip[4];
    for (int c = 0; c < 4; ++c)
        clip[c] = point[0] * m[c] + point[1] * m[4 + c] + point[2] * m[8 + c] + m[12 + c];
    if (!(clip[3] > 1e-9))
        return false;
    ndc[0] = clip[0] / clip[3];
    ndc[1] = clip[1] / clip[3];
    return true;
}

} // namespace camerashape
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
