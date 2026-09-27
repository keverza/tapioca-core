#ifndef EVP_ARCHVIZ_DXGI_CAMERALAYOUT_HPP
#define EVP_ARCHVIZ_DXGI_CAMERALAYOUT_HPP

// See OVERLAY-INVARIANTS.md, section 1, findings 1 and 2. What Archicad's `b2`
// holds, decided from the sixteen constants alone.
//
// ⚠️ TWO LAYOUTS, BOTH MEASURED, AND ONE FRAME CAN CARRY BOTH.
//
//     separate   b1 = view, b2 = projection            p * b1 * b2
//     combined   b1 = view, b2 = view x projection     p * b2
//
// The MODEL's draws carry the combined layout. The separate one belongs to the
// 3D editing plane's own draws (6 and 142 indices), which Archicad issues only
// while its editing plane display is on. The census used to accept a projection
// in `b2` and nothing else, so it could only ever lock onto the plane: on
// 2026-09-26 at 19:15 the display was turned off, and from then on no group
// qualified and the camera never locked -- on every build, including the one
// that had locked at 15:47 (2b86174, rebuilt and rerun on 2026-09-27). Turning the
// display back on (2026-09-27 16:12) brought the lock back.
//
// The evidence is the draw recorder at five still views: `b2` alone put the
// model's corners where Archicad's own ModelToScreen puts them to within 0.04% of
// the viewport, at every draw of the scene pass, and `b1` was the view whose
// translation row reproduces the camera position Archicad reports.
//
// ⚠️ IN THE COMBINED LAYOUT ARCHICAD'S DEPTH IS NOT USED; OURS IS. With the plane
// hidden, Archicad's near and far do not enclose the model: every model corner
// sat beyond the far plane (z/w 1.0000 to 1.0019, far 37-56 m against a model 38
// to 127 m away) at four views, so Archicad draws it with depth clipping off. The
// census anchor (the model centre) is refused and our own draws would be clipped
// away. `DecodedDepth` replaces z with a projective depth of our own, near `kNear`
// and far `kFar` in view metres -- `w` is the view distance in both layouts -- and
// `camerashader::Compose` writes the same line into every shader, so what the
// census scores is what the overlay draws. The separate layout keeps Archicad's.
//
// ⚠️ THE LAYOUT IS READ FROM `b2` ALONE, NEVER FROM `b1 ^ -1 * b2`. While the
// camera moves, `b2` and `b1` of one draw can belong to different cameras -- the
// census logged it at 3 of 10 groups, and at every group of one frame -- and in
// the combined layout Archicad positions with `b2` alone, so `b2` IS the camera of
// the image and `b1` is not. Requiring the two to agree would refuse every moving
// sample, and the census gate refuses a group for a single refused sample.
//
// ⚠️ A SHAPE, NOT A CANDIDATE. `b2` is accepted as combined only when its columns
// have the shape a rigid view times a perspective projection must have: the w
// column is the negated view axis, a unit vector; the depth column is parallel to
// it; x and y, less their part along it, are orthogonal. A screen map, a parallel
// projection and anything that is not a camera fail it. Whether a camera that
// passes is the MODEL's camera stays the census's question, asked as before.
//
// Conventions are interpretation 0 (§1.2): sixteen floats as stored, row-major,
// multiplied `p * M`.

#include <cmath>
#include <cstdint>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace cameralayout {

// The interpretation bit that says `b2` holds view x projection. Bits 0 and 1 are
// the transposes and bit 2 the reversed order (the oracle's numbering), so the
// layout rides in the same number the selection already commits as one
// transaction (§4) and every draw already receives.
constexpr uint32_t kCombined = 8u;

// Our depth in the combined layout: z = kDepthA * w + kDepthB. The near is the one
// Archicad used in every session that locked (0.034-0.040 m), so the overlay's
// NDC depth bias keeps the reach it was tuned with and nothing Archicad draws is
// cut by our near plane (its own is 0.1 m in this layout); the far encloses any
// model. D32_FLOAT resolves 1.5 cm at 100 m with these.
constexpr double kNear = 0.04;
constexpr double kFar = 100000.0;
constexpr double kDepthA = kFar / (kFar - kNear);
constexpr double kDepthB = -kFar * kNear / (kFar - kNear);

enum class Layout : uint32_t { Separate = 0, Combined = 1, Neither = 2 };

namespace detail {

constexpr float kZeroTolerance = 1e-3f;  // separate: relative to the projection's own scale
constexpr double kShapeTolerance = 1e-2; // combined: the rotation inside a product of two floats

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

} // namespace detail

// `b2` IS a perspective projection: the w column is (0, 0, +-1, 0) and nothing
// rotates x or y. Off-centre terms (rows 2, columns 0 and 1) are allowed.
inline bool IsProjection (const float m[16])
{
    const float w = std::fabs (m[11]);
    if (!(w > 0.5f))
        return false;
    float scale = std::fabs (m[0]) > std::fabs (m[5]) ? std::fabs (m[0]) : std::fabs (m[5]);
    scale = w > scale ? w : scale;
    const int zero[] = { 1, 2, 3, 4, 6, 7, 12, 13, 15 };
    for (int index : zero) {
        if (!(std::fabs (m[index]) <= detail::kZeroTolerance * scale))
            return false;
    }
    return std::fabs (m[0]) > 1e-6f && std::fabs (m[5]) > 1e-6f;
}

// `b2` is a rigid view times a perspective projection -- the shape, whatever the
// camera. A pure projection passes too (the view is the identity); `Classify`
// asks `IsProjection` first.
inline bool IsViewProjection (const float m[16])
{
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite (m[i]))
            return false;
    }
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

inline Layout Classify (const float b2[16])
{
    if (IsProjection (b2))
        return Layout::Separate;
    if (IsViewProjection (b2))
        return Layout::Combined;
    return Layout::Neither;
}

// A general inverse, in double: `view` is rigid in every capture so far, but
// nothing here depends on it.
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

// `b2` with its depth column replaced by ours: z = kDepthA * w + kDepthB, for the
// combined layout (see the header). x, y and w -- the image -- are Archicad's.
inline void DecodedDepth (const float b2[16], double out[16])
{
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c)
            out[r * 4 + c] = b2[r * 4 + c];
        out[r * 4 + 2] = kDepthA * double (b2[r * 4 + 3]) + (r == 3 ? kDepthB : 0.0);
    }
}

// ⚠️ THE PAIR EVERY CPU SCORER READS. `decoded` is `b2` for the separate layout
// (and for anything that is not a camera, unchanged), and `view ^ -1 * b2'` for the
// combined one, where `b2'` is `b2` with our depth (`DecodedDepth`). So `view *
// decoded` IS what the overlay's shaders draw with -- Archicad's own image, at our
// depth -- and every consumer of the pair keeps its meaning without knowing a
// layout exists. When `b1` and `b2` belong to one camera, `decoded` is that
// camera's projection. A combined `b2` over a view that cannot be inverted is
// reported as `Neither`: it cannot be decoded, so it is not accepted.
inline Layout Decode (const float view[16], const float b2[16], float decoded[16])
{
    std::memcpy (decoded, b2, sizeof (float) * 16);
    const Layout layout = Classify (b2);
    if (layout != Layout::Combined)
        return layout;
    double inverse[16];
    if (!Invert (view, inverse))
        return Layout::Neither;
    double drawn[16];
    DecodedDepth (b2, drawn);
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k)
                sum += inverse[r * 4 + k] * drawn[k * 4 + c];
            decoded[r * 4 + c] = float (sum);
        }
    }
    return Layout::Combined;
}

// The census gate's projection term: every sample decoded as a camera, and all of
// them in ONE layout. A group that saw both was watching Archicad switch, and no
// single interpretation draws both correctly.
inline bool Decodes (uint32_t samples, uint32_t decoded, uint32_t combined)
{
    return samples > 0 && decoded == samples && (combined == 0 || combined == decoded);
}

inline uint32_t Interpretation (uint32_t variant, bool combined)
{
    return combined ? (variant | kCombined) : (variant & ~kCombined);
}

inline bool IsCombined (uint32_t interpretation)
{
    return (interpretation & kCombined) != 0 && interpretation != 0xffffffffu;
}

} // namespace cameralayout
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
