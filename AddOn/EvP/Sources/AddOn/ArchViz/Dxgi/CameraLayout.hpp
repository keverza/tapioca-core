#ifndef EVP_ARCHVIZ_DXGI_CAMERALAYOUT_HPP
#define EVP_ARCHVIZ_DXGI_CAMERALAYOUT_HPP

// See OVERLAY-INVARIANTS.md, section 1, findings 1 and 2. Where Archicad's camera
// is, decided from the sixteen constants of two windows.
//
// ⚠️ THE CAMERA IS b1 AND b0. b2 IS NOT THE CAMERA (2026-09-27, 21:00).
//
//     b1   the view V = [R 0; t 1], one window per draw
//     b0   R x P: the view's ROTATION times the projection, no translation,
//          one window per frame
//     the image   p * V * P  =  (p - eye) * b0,   eye = -t * R^T
//
// Archicad draws camera-relative: the model's vertices go through `b0` less the
// eye, which is the precision a model far from the origin needs. At the model's
// draws `b0`'s rotation IS `b1`'s -- identical floats at every draw recorded --
// and `(p - eye) * b0` reproduced x, y and w of the camera that put the model where
// Archicad's own ModelToScreen does to 1e-5, in every state measured: plane hidden
// and still (against `b2`), plane shown (against `b1 * b2`), and orbiting with the
// plane hidden, where `b2` held a 2D screen map at every draw of every frame,
// copied before the draw and after it (three runs, 21:00:33-57). `b2` is whatever
// was last written to its window: the projection, view x projection, the previous
// image's camera, or a screen map. The census read it for two days and could only
// ever lock where it happened to be right -- the editing plane's draws.
//
// ⚠️ ONE CAMERA, SO THE ROTATIONS MUST AGREE. `b0` belongs to the frame and `b1` to
// the draw. The 24-index helper's `b1` is the PREVIOUS image's camera (500 of 500
// moving images, 16:56): while the camera turns its rotation differs from `b0`'s
// (2e-3 to 1e-2 in the orbit captures) and the pair is refused. At the model's
// draws they are equal.
//
// ⚠️ THE DEPTH IS OURS. Archicad's near and far do not enclose the model with the
// editing plane hidden (every corner beyond the far plane, z/w 1.0000-1.0019, at
// four views), and `b0`'s own depth is another mapping again. `DecodedDepth` and
// `camerashader::Compose` both write z = kDepthA * w + kDepthB -- w is the view
// distance -- so what the census scores is what the overlay draws. The overlay
// occludes against its own depth (host occluders drawn with this same camera,
// invariant 7), never against Archicad's values.
//
// ⚠️ A SHAPE, NOT A CANDIDATE. `b1` must be a rigid view and `b0` a rigid rotation
// times a perspective projection with no translation; a screen map, a parallel
// projection and anything else is refused. Parallel projections stay unsupported,
// as they were.
//
// Conventions are interpretation 0 (§1.2): sixteen floats as stored, row-major,
// multiplied `p * M`.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace cameralayout {

// The window the camera's projection is COPIED from. `b2` keeps its place in the
// identity terms (window sizes, the pinned buffer): it is bound as a 16-constant
// window at every model draw in every state measured. Nothing reads its bytes.
constexpr size_t kProjectionWindow = 0;

// The interpretation bit that says the camera is `(p - eye) * b0`. Bits 0 and 1
// are the transposes and bit 2 the reversed order (the oracle's numbering), so
// the layout rides in the number the selection already commits as one transaction
// (§4) and every draw already receives.
constexpr uint32_t kRelative = 8u;

// Our depth: z = kDepthA * w + kDepthB. The near is the one Archicad used in every
// session that locked (0.034-0.040 m), so the overlay's NDC depth bias keeps the
// reach it was tuned with; the far encloses any model. D32_FLOAT resolves 1.5 cm
// at 100 m with these.
constexpr double kNear = 0.04;
constexpr double kFar = 100000.0;
constexpr double kDepthA = kFar / (kFar - kNear);
constexpr double kDepthB = -kFar * kNear / (kFar - kNear);

enum class Layout : uint32_t { Relative = 1, Neither = 2 };

namespace detail {

constexpr double kShapeTolerance = 1e-2;    // the rotation inside a product of two floats
constexpr double kRigidTolerance = 1e-3;    // a view's rows: unit and orthogonal
constexpr double kRotationTolerance = 1e-4; // b0's view axis against b1's

inline void Column (const float m[16], int column, double out[3])
{
    out[0] = m[column];
    out[1] = m[4 + column];
    out[2] = m[8 + column];
}

inline double Dot (const double a[3], const double b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// `v` less its component along the unit vector `axis`.
inline void Reject (const double v[3], const double axis[3], double out[3])
{
    const double along = Dot (v, axis);
    for (int i = 0; i < 3; ++i)
        out[i] = v[i] - along * axis[i];
}

inline bool Finite (const float m[16])
{
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite (m[i]))
            return false;
    }
    return true;
}

} // namespace detail

// `b1`: a rigid view, row vectors -- the last column (0, 0, 0, 1) and the upper
// 3x3 a rotation.
inline bool IsView (const float m[16])
{
    if (!detail::Finite (m) || std::fabs (m[3]) > 1e-4 || std::fabs (m[7]) > 1e-4 || std::fabs (m[11]) > 1e-4 ||
        std::fabs (m[15] - 1.0f) > 1e-4)
        return false;
    const double rows[3][3] = { { m[0], m[1], m[2] }, { m[4], m[5], m[6] }, { m[8], m[9], m[10] } };
    for (int r = 0; r < 3; ++r) {
        if (std::fabs (std::sqrt (detail::Dot (rows[r], rows[r])) - 1.0) > detail::kRigidTolerance)
            return false;
    }
    return std::fabs (detail::Dot (rows[0], rows[1])) <= detail::kRigidTolerance &&
           std::fabs (detail::Dot (rows[0], rows[2])) <= detail::kRigidTolerance &&
           std::fabs (detail::Dot (rows[1], rows[2])) <= detail::kRigidTolerance;
}

// A rigid view times a perspective projection -- the shape, whatever the camera:
// the w column is the negated view axis, a unit vector; the depth column is
// parallel to it; x and y, less their part along it, are orthogonal.
inline bool IsViewProjection (const float m[16])
{
    if (!detail::Finite (m))
        return false;
    double w[3], depth[3], x[3], y[3], xs[3], ys[3], depthOff[3];
    detail::Column (m, 3, w);
    const double length = std::sqrt (detail::Dot (w, w));
    if (!(std::fabs (length - 1.0) <= detail::kShapeTolerance))
        return false;
    const double axis[3] = { w[0] / length, w[1] / length, w[2] / length };

    detail::Column (m, 2, depth);
    detail::Reject (depth, axis, depthOff);
    const double depthAlong = std::fabs (detail::Dot (depth, axis));
    if (!(std::sqrt (detail::Dot (depthOff, depthOff)) <=
          detail::kShapeTolerance * (depthAlong > 1.0 ? depthAlong : 1.0)))
        return false;

    detail::Column (m, 0, x);
    detail::Column (m, 1, y);
    detail::Reject (x, axis, xs);
    detail::Reject (y, axis, ys);
    const double xLength = std::sqrt (detail::Dot (xs, xs));
    const double yLength = std::sqrt (detail::Dot (ys, ys));
    if (!(xLength > 1e-6) || !(yLength > 1e-6))
        return false;
    return std::fabs (detail::Dot (xs, ys)) <= detail::kShapeTolerance * xLength * yLength;
}

// `b0`: that shape with the eye at the origin -- no x, y or w translation.
inline bool IsRotationProjection (const float m[16])
{
    return IsViewProjection (m) && std::fabs (m[12]) <= 1e-5 && std::fabs (m[13]) <= 1e-5 && std::fabs (m[15]) <= 1e-5;
}

// How far `b0`'s view axis (its negated w column) is from `b1`'s (its column 2).
inline double RotationMismatch (const float b0[16], const float view[16])
{
    double worst = 0.0;
    for (int r = 0; r < 3; ++r) {
        const double d = std::fabs (double (b0[r * 4 + 3]) + double (view[r * 4 + 2]));
        worst = d > worst ? d : worst;
    }
    return worst;
}

// The camera's position: V = [R 0; t 1] maps it to the origin, so eye = -t * R^T.
inline void Eye (const float view[16], double eye[3])
{
    for (int i = 0; i < 3; ++i)
        eye[i] = -(double (view[12]) * view[i * 4 + 0] + double (view[13]) * view[i * 4 + 1] +
                   double (view[14]) * view[i * 4 + 2]);
}

// The image's view x projection: `b0` with the eye moved to its place, T(-eye) * b0.
// x, y and w are Archicad's; the depth column is `b0`'s own until `DecodedDepth`.
inline void ViewProjection (const float b0[16], const float view[16], double out[16])
{
    double eye[3];
    Eye (view, eye);
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 3; ++r)
            out[r * 4 + c] = b0[r * 4 + c];
        out[12 + c] = b0[12 + c] - (eye[0] * b0[c] + eye[1] * b0[4 + c] + eye[2] * b0[8 + c]);
    }
}

// A general inverse, in double.
inline bool Invert (const float m[16], double out[16])
{
    double a[4][8];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            a[r][c] = m[r * 4 + c];
            a[r][4 + c] = r == c ? 1.0 : 0.0;
        }
    }
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r) {
            if (std::fabs (a[r][c]) > std::fabs (a[pivot][c]))
                pivot = r;
        }
        if (!(std::fabs (a[pivot][c]) > 1e-12))
            return false;
        for (int k = 0; k < 8 && pivot != c; ++k) {
            const double t = a[c][k];
            a[c][k] = a[pivot][k];
            a[pivot][k] = t;
        }
        const double scale = a[c][c];
        for (int k = 0; k < 8; ++k)
            a[c][k] /= scale;
        for (int r = 0; r < 4; ++r) {
            if (r == c)
                continue;
            const double factor = a[r][c];
            for (int k = 0; k < 8; ++k)
                a[r][k] -= factor * a[c][k];
        }
    }
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c)
            out[r * 4 + c] = a[r][4 + c];
    }
    return true;
}

// A view x projection with its depth column replaced by ours: z = kDepthA * w +
// kDepthB (see the header). x, y and w -- the image -- are untouched.
inline void DecodedDepth (double m[16])
{
    for (int r = 0; r < 4; ++r)
        m[r * 4 + 2] = kDepthA * m[r * 4 + 3] + (r == 3 ? kDepthB : 0.0);
}

// ⚠️ THE PAIR EVERY CPU SCORER READS: `(b1, decoded)` with `b1 * decoded` the
// image's view x projection at our depth, `T(-eye) * b0'` -- exactly what the
// overlay's shaders draw (`camerashader::Compose`), so every consumer of the pair
// keeps its meaning. `decoded` is then the camera's own projection. Anything that
// is not one camera is `Neither`, and `decoded` is `b0` unchanged.
inline Layout Decode (const float view[16], const float b0[16], float decoded[16])
{
    std::memcpy (decoded, b0, sizeof (float) * 16);
    if (!IsView (view) || !IsRotationProjection (b0) || RotationMismatch (b0, view) > detail::kRotationTolerance)
        return Layout::Neither;
    double inverse[16];
    if (!Invert (view, inverse))
        return Layout::Neither;
    double drawn[16];
    ViewProjection (b0, view, drawn);
    DecodedDepth (drawn);
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k)
                sum += inverse[r * 4 + k] * drawn[k * 4 + c];
            decoded[r * 4 + c] = float (sum);
        }
    }
    return Layout::Relative;
}

// The census gate's projection term: every sample decoded as the camera.
inline bool Decodes (uint32_t samples, uint32_t decoded)
{
    return samples > 0 && decoded == samples;
}

// ⚠️ THE RELATIVE LAYOUT HAS ONE READING. `Decode` accepts `b1` as a row-vector
// view and `b0` as a row-vector rotation x projection -- interpretation 0 -- and
// the shader derives the eye from that same reading, so a transpose the scorer
// might prefer on a tie is not a camera this layout can draw.
inline uint32_t Interpretation (uint32_t variant, bool relative)
{
    return relative ? kRelative : (variant & ~kRelative);
}

inline bool IsRelative (uint32_t interpretation)
{
    return (interpretation & kRelative) != 0 && interpretation != 0xffffffffu;
}

} // namespace cameralayout
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
