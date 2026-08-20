#include "ClashEngine.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace geomsrv {

namespace {

// ---- small double[3] vector helpers ---------------------------------------
inline void  Sub (const double a[3], const double b[3], double o[3]) { o[0]=a[0]-b[0]; o[1]=a[1]-b[1]; o[2]=a[2]-b[2]; }
inline double Dot (const double a[3], const double b[3]) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
inline void  Cross (const double a[3], const double b[3], double o[3]) {
    o[0]=a[1]*b[2]-a[2]*b[1]; o[1]=a[2]*b[0]-a[0]*b[2]; o[2]=a[0]*b[1]-a[1]*b[0];
}

const double* V (const Mesh& m, uint32_t vi) { return &m.vertices[vi * 3]; }

// Per-triangle AABB into mn/mx.
void TriAabb (const Mesh& m, size_t t, double mn[3], double mx[3])
{
    const double* p0 = V (m, m.triangles[t + 0]);
    const double* p1 = V (m, m.triangles[t + 1]);
    const double* p2 = V (m, m.triangles[t + 2]);
    for (int k = 0; k < 3; ++k) {
        mn[k] = std::min (p0[k], std::min (p1[k], p2[k]));
        mx[k] = std::max (p0[k], std::max (p1[k], p2[k]));
    }
}

// Gap between two AABBs (0 if they overlap), squared not taken.
double AabbGap (const double amn[3], const double amx[3], const double bmn[3], const double bmx[3])
{
    double d2 = 0.0;
    for (int k = 0; k < 3; ++k) {
        const double e = amn[k] > bmx[k] ? amn[k] - bmx[k]
                       : (bmn[k] > amx[k] ? bmn[k] - amx[k] : 0.0);
        d2 += e * e;
    }
    return std::sqrt (d2);
}

// ===========================================================================
// Möller "A Fast Triangle-Triangle Intersection Test" (1997), no-division
// variant with coplanar handling. Returns true if the triangles intersect.
// ===========================================================================
constexpr double kEps = 1e-12;

bool CoplanarTriTri (const double N[3],
                     const double V0[3], const double V1[3], const double V2[3],
                     const double U0[3], const double U1[3], const double U2[3]);

inline bool EdgeEdgeTest (const double* V0, const double* U0, const double* U1,
                          double Ax, double Ay, int i0, int i1)
{
    const double Bx = U0[i0] - U1[i0];
    const double By = U0[i1] - U1[i1];
    const double Cx = V0[i0] - U0[i0];
    const double Cy = V0[i1] - U0[i1];
    const double f = Ay * Bx - Ax * By;
    const double d = By * Cx - Bx * Cy;
    if ((f > 0 && d >= 0 && d <= f) || (f < 0 && d <= 0 && d >= f)) {
        const double e = Ax * Cy - Ay * Cx;
        if (f > 0) { if (e >= 0 && e <= f) return true; }
        else       { if (e <= 0 && e >= f) return true; }
    }
    return false;
}

bool EdgeAgainstTriEdges (const double* v0, const double* v1,
                          const double* u0, const double* u1, const double* u2,
                          int i0, int i1)
{
    const double Ax = v1[i0] - v0[i0];
    const double Ay = v1[i1] - v0[i1];
    if (EdgeEdgeTest (v0, u0, u1, Ax, Ay, i0, i1)) return true;
    if (EdgeEdgeTest (v0, u1, u2, Ax, Ay, i0, i1)) return true;
    if (EdgeEdgeTest (v0, u2, u0, Ax, Ay, i0, i1)) return true;
    return false;
}

bool PointInTri (const double* V0, const double* u0, const double* u1, const double* u2,
                 int i0, int i1)
{
    double a = u1[i1] - u0[i1];
    double b = -(u1[i0] - u0[i0]);
    double c = -a * u0[i0] - b * u0[i1];
    const double d0 = a * V0[i0] + b * V0[i1] + c;

    a = u2[i1] - u1[i1];
    b = -(u2[i0] - u1[i0]);
    c = -a * u1[i0] - b * u1[i1];
    const double d1 = a * V0[i0] + b * V0[i1] + c;

    a = u0[i1] - u2[i1];
    b = -(u0[i0] - u2[i0]);
    c = -a * u2[i0] - b * u2[i1];
    const double d2 = a * V0[i0] + b * V0[i1] + c;

    return (d0 * d1 > 0.0) && (d0 * d2 > 0.0);
}

bool CoplanarTriTri (const double N[3],
                     const double V0[3], const double V1[3], const double V2[3],
                     const double U0[3], const double U1[3], const double U2[3])
{
    // Project onto the axis plane where the triangles are largest.
    double A[3] = { std::fabs (N[0]), std::fabs (N[1]), std::fabs (N[2]) };
    int i0, i1;
    if (A[0] > A[1]) {
        if (A[0] > A[2]) { i0 = 1; i1 = 2; } else { i0 = 0; i1 = 1; }
    } else {
        if (A[2] > A[1]) { i0 = 0; i1 = 1; } else { i0 = 0; i1 = 2; }
    }
    if (EdgeAgainstTriEdges (V0, V1, U0, U1, U2, i0, i1)) return true;
    if (EdgeAgainstTriEdges (V1, V2, U0, U1, U2, i0, i1)) return true;
    if (EdgeAgainstTriEdges (V2, V0, U0, U1, U2, i0, i1)) return true;
    if (PointInTri (V0, U0, U1, U2, i0, i1)) return true;
    if (PointInTri (U0, V0, V1, V2, i0, i1)) return true;
    return false;
}

// Compute the parametric interval of triangle projected onto the intersection
// line and combine into isect[0..1]. Returns false if coplanar (caller handles).
bool ComputeIntervals (double VV0, double VV1, double VV2,
                       double D0, double D1, double D2,
                       double D0D1, double D0D2,
                       double& a, double& b, double& c, double& x0, double& x1)
{
    if (D0D1 > 0.0)      { a = VV2; b = (VV0 - VV2) * D2; c = (VV1 - VV2) * D2; x0 = D2 - D0; x1 = D2 - D1; }
    else if (D0D2 > 0.0) { a = VV1; b = (VV0 - VV1) * D1; c = (VV2 - VV1) * D1; x0 = D1 - D0; x1 = D1 - D2; }
    else if (D1 * D2 > 0.0 || D0 != 0.0) { a = VV0; b = (VV1 - VV0) * D0; c = (VV2 - VV0) * D0; x0 = D0 - D1; x1 = D0 - D2; }
    else if (D1 != 0.0)  { a = VV1; b = (VV0 - VV1) * D1; c = (VV2 - VV1) * D1; x0 = D1 - D0; x1 = D1 - D2; }
    else if (D2 != 0.0)  { a = VV2; b = (VV0 - VV2) * D2; c = (VV1 - VV2) * D2; x0 = D2 - D0; x1 = D2 - D1; }
    else                 { return false; }   // coplanar
    return true;
}

bool TriTriIntersect (const double V0[3], const double V1[3], const double V2[3],
                      const double U0[3], const double U1[3], const double U2[3])
{
    // Plane of triangle 1.
    double E1[3], E2[3], N1[3];
    Sub (V1, V0, E1); Sub (V2, V0, E2); Cross (E1, E2, N1);
    const double d1 = -Dot (N1, V0);
    double du0 = Dot (N1, U0) + d1, du1 = Dot (N1, U1) + d1, du2 = Dot (N1, U2) + d1;
    if (std::fabs (du0) < kEps) du0 = 0.0;
    if (std::fabs (du1) < kEps) du1 = 0.0;
    if (std::fabs (du2) < kEps) du2 = 0.0;
    const double du0du1 = du0 * du1, du0du2 = du0 * du2;
    if (du0du1 > 0.0 && du0du2 > 0.0) return false;   // U entirely on one side

    // Plane of triangle 2.
    double N2[3];
    Sub (U1, U0, E1); Sub (U2, U0, E2); Cross (E1, E2, N2);
    const double d2 = -Dot (N2, U0);
    double dv0 = Dot (N2, V0) + d2, dv1 = Dot (N2, V1) + d2, dv2 = Dot (N2, V2) + d2;
    if (std::fabs (dv0) < kEps) dv0 = 0.0;
    if (std::fabs (dv1) < kEps) dv1 = 0.0;
    if (std::fabs (dv2) < kEps) dv2 = 0.0;
    const double dv0dv1 = dv0 * dv1, dv0dv2 = dv0 * dv2;
    if (dv0dv1 > 0.0 && dv0dv2 > 0.0) return false;

    // Direction of intersection line; pick the largest component to project on.
    double D[3]; Cross (N1, N2, D);
    double maxv = std::fabs (D[0]); int idx = 0;
    if (std::fabs (D[1]) > maxv) { maxv = std::fabs (D[1]); idx = 1; }
    if (std::fabs (D[2]) > maxv) { idx = 2; }

    const double vp0 = V0[idx], vp1 = V1[idx], vp2 = V2[idx];
    const double up0 = U0[idx], up1 = U1[idx], up2 = U2[idx];

    double a, b, c, x0, x1;
    if (!ComputeIntervals (vp0, vp1, vp2, dv0, dv1, dv2, dv0dv1, dv0dv2, a, b, c, x0, x1))
        return CoplanarTriTri (N1, V0, V1, V2, U0, U1, U2);

    double d, e, f, y0, y1;
    if (!ComputeIntervals (up0, up1, up2, du0, du1, du2, du0du1, du0du2, d, e, f, y0, y1))
        return CoplanarTriTri (N1, V0, V1, V2, U0, U1, U2);

    const double xx = x0 * x1, yy = y0 * y1, xxyy = xx * yy;
    double isect1[2], isect2[2];
    double tmp = a * xxyy;
    isect1[0] = tmp + b * x1 * yy;
    isect1[1] = tmp + c * x0 * yy;
    tmp = d * xxyy;
    isect2[0] = tmp + e * xx * y1;
    isect2[1] = tmp + f * xx * y0;

    if (isect1[0] > isect1[1]) std::swap (isect1[0], isect1[1]);
    if (isect2[0] > isect2[1]) std::swap (isect2[0], isect2[1]);

    if (isect1[1] < isect2[0] || isect2[1] < isect1[0]) return false;
    return true;
}

// ===========================================================================
// Distances (used only when triangles do NOT intersect).
// ===========================================================================

// Closest point on triangle (a,b,c) to p (Ericson). Returns squared distance.
double ClosestPtTri (const double p[3], const double a[3], const double b[3], const double c[3], double out[3])
{
    double ab[3], ac[3], ap[3];
    Sub (b, a, ab); Sub (c, a, ac); Sub (p, a, ap);
    const double d1 = Dot (ab, ap), d2 = Dot (ac, ap);
    auto set = [&] (double u, double v, double w) {
        out[0] = u*a[0]+v*b[0]+w*c[0]; out[1] = u*a[1]+v*b[1]+w*c[1]; out[2] = u*a[2]+v*b[2]+w*c[2];
    };
    if (d1 <= 0 && d2 <= 0) set (1,0,0);
    else {
        double bp[3]; Sub (p, b, bp);
        const double d3 = Dot (ab, bp), d4 = Dot (ac, bp);
        if (d3 >= 0 && d4 <= d3) set (0,1,0);
        else {
            const double vc = d1*d4 - d3*d2;
            if (vc <= 0 && d1 >= 0 && d3 <= 0) { const double v = d1/(d1-d3); set (1-v,v,0); }
            else {
                double cp[3]; Sub (p, c, cp);
                const double d5 = Dot (ab, cp), d6 = Dot (ac, cp);
                if (d6 >= 0 && d5 <= d6) set (0,0,1);
                else {
                    const double vb = d5*d2 - d1*d6;
                    if (vb <= 0 && d2 >= 0 && d6 <= 0) { const double w = d2/(d2-d6); set (1-w,0,w); }
                    else {
                        const double va = d3*d6 - d5*d4;
                        if (va <= 0 && (d4-d3) >= 0 && (d5-d6) >= 0) { const double w = (d4-d3)/((d4-d3)+(d5-d6)); set (0,1-w,w); }
                        else { const double den = 1.0/(va+vb+vc); const double v = vb*den, w = vc*den; set (1-v-w,v,w); }
                    }
                }
            }
        }
    }
    const double dx = p[0]-out[0], dy = p[1]-out[1], dz = p[2]-out[2];
    return dx*dx + dy*dy + dz*dz;
}

// Squared distance between segments p1p2 and p3p4, with witness points.
double SegSegDist2 (const double p1[3], const double p2[3], const double p3[3], const double p4[3],
                    double c1[3], double c2[3])
{
    double d1[3], d2[3], r[3];
    Sub (p2, p1, d1); Sub (p4, p3, d2); Sub (p1, p3, r);
    const double a = Dot (d1, d1), e = Dot (d2, d2), f = Dot (d2, r);
    double s, t;
    if (a <= kEps && e <= kEps) { s = t = 0.0; }
    else if (a <= kEps) { s = 0.0; t = std::clamp (f / e, 0.0, 1.0); }
    else {
        const double cc = Dot (d1, r);
        if (e <= kEps) { t = 0.0; s = std::clamp (-cc / a, 0.0, 1.0); }
        else {
            const double bb = Dot (d1, d2);
            const double den = a * e - bb * bb;
            s = (den > kEps) ? std::clamp ((bb * f - cc * e) / den, 0.0, 1.0) : 0.0;
            t = (bb * s + f) / e;
            if (t < 0.0)      { t = 0.0; s = std::clamp (-cc / a, 0.0, 1.0); }
            else if (t > 1.0) { t = 1.0; s = std::clamp ((bb - cc) / a, 0.0, 1.0); }
        }
    }
    for (int k = 0; k < 3; ++k) { c1[k] = p1[k] + d1[k] * s; c2[k] = p3[k] + d2[k] * t; }
    const double dx = c1[0]-c2[0], dy = c1[1]-c2[1], dz = c1[2]-c2[2];
    return dx*dx + dy*dy + dz*dz;
}

// Minimum squared distance between two (non-intersecting) triangles + witnesses.
double TriTriDist2 (const double A0[3], const double A1[3], const double A2[3],
                    const double B0[3], const double B1[3], const double B2[3],
                    double wa[3], double wb[3])
{
    const double* A[3] = { A0, A1, A2 };
    const double* B[3] = { B0, B1, B2 };
    double best = std::numeric_limits<double>::max ();
    double c1[3], c2[3];

    // 9 edge-edge pairs.
    for (int i = 0; i < 3; ++i) {
        const double* a0 = A[i]; const double* a1 = A[(i + 1) % 3];
        for (int j = 0; j < 3; ++j) {
            const double* b0 = B[j]; const double* b1 = B[(j + 1) % 3];
            const double d = SegSegDist2 (a0, a1, b0, b1, c1, c2);
            if (d < best) { best = d; wa[0]=c1[0];wa[1]=c1[1];wa[2]=c1[2]; wb[0]=c2[0];wb[1]=c2[1];wb[2]=c2[2]; }
        }
    }
    // 3 A-vertices vs B, 3 B-vertices vs A.
    double cp[3];
    for (int i = 0; i < 3; ++i) {
        double d = ClosestPtTri (A[i], B0, B1, B2, cp);
        if (d < best) { best = d; wa[0]=A[i][0];wa[1]=A[i][1];wa[2]=A[i][2]; wb[0]=cp[0];wb[1]=cp[1];wb[2]=cp[2]; }
        d = ClosestPtTri (B[i], A0, A1, A2, cp);
        if (d < best) { best = d; wb[0]=B[i][0];wb[1]=B[i][1];wb[2]=B[i][2]; wa[0]=cp[0];wa[1]=cp[1];wa[2]=cp[2]; }
    }
    return best;
}

} // namespace

// ---------------------------------------------------------------------------

bool MeshesClash (const Mesh& a, const Mesh& b)
{
    if (a.bounds.Valid () && b.bounds.Valid ()) {
        const double amn[3] = { a.bounds.mn[0], a.bounds.mn[1], a.bounds.mn[2] };
        const double amx[3] = { a.bounds.mx[0], a.bounds.mx[1], a.bounds.mx[2] };
        if (!b.bounds.OverlapsBox (amn, amx)) return false;
    }
    for (size_t ta = 0; ta < a.triangles.size (); ta += 3) {
        double amn[3], amx[3]; TriAabb (a, ta, amn, amx);
        for (size_t tb = 0; tb < b.triangles.size (); tb += 3) {
            double bmn[3], bmx[3]; TriAabb (b, tb, bmn, bmx);
            if (amn[0] > bmx[0] || bmn[0] > amx[0] ||
                amn[1] > bmx[1] || bmn[1] > amx[1] ||
                amn[2] > bmx[2] || bmn[2] > amx[2]) continue;
            if (TriTriIntersect (V (a, a.triangles[ta]), V (a, a.triangles[ta+1]), V (a, a.triangles[ta+2]),
                                 V (b, b.triangles[tb]), V (b, b.triangles[tb+1]), V (b, b.triangles[tb+2])))
                return true;
        }
    }
    return false;
}

Clearance MeshClearance (const Mesh& a, const Mesh& b, double maxDist)
{
    Clearance out;
    double best2 = std::numeric_limits<double>::max ();
    const double cap2 = (maxDist > 0.0) ? maxDist * maxDist : std::numeric_limits<double>::max ();

    for (size_t ta = 0; ta < a.triangles.size (); ta += 3) {
        double amn[3], amx[3]; TriAabb (a, ta, amn, amx);
        for (size_t tb = 0; tb < b.triangles.size (); tb += 3) {
            double bmn[3], bmx[3]; TriAabb (b, tb, bmn, bmx);
            const double g = AabbGap (amn, amx, bmn, bmx);
            if (g * g >= best2 || g * g > cap2) continue;   // can't improve / beyond cap

            const double* pa0 = V (a, a.triangles[ta]);   const double* pa1 = V (a, a.triangles[ta+1]);   const double* pa2 = V (a, a.triangles[ta+2]);
            const double* pb0 = V (b, b.triangles[tb]);   const double* pb1 = V (b, b.triangles[tb+1]);   const double* pb2 = V (b, b.triangles[tb+2]);

            if (TriTriIntersect (pa0, pa1, pa2, pb0, pb1, pb2)) {
                out.clash = true; out.dist = 0.0;
                out.pointA[0]=out.pointB[0]=pa0[0]; out.pointA[1]=out.pointB[1]=pa0[1]; out.pointA[2]=out.pointB[2]=pa0[2];
                return out;
            }
            double wa[3], wb[3];
            const double d2 = TriTriDist2 (pa0, pa1, pa2, pb0, pb1, pb2, wa, wb);
            if (d2 < best2) {
                best2 = d2;
                for (int k = 0; k < 3; ++k) { out.pointA[k] = wa[k]; out.pointB[k] = wb[k]; }
            }
        }
    }
    out.dist = (best2 == std::numeric_limits<double>::max ()) ? -1.0 : std::sqrt (best2);
    return out;
}

std::vector<ClashPair> ClashAll (const Snapshot& snap, double gap)
{
    std::vector<ClashPair> out;
    const size_t n = snap.meshes.size ();
    for (size_t i = 0; i < n; ++i) {
        const Mesh& a = snap.meshes[i];
        if (a.triangles.empty () || !a.bounds.Valid ()) continue;
        // Broadphase: inflate A's box by gap and test AABB overlap.
        const double amn[3] = { a.bounds.mn[0]-gap, a.bounds.mn[1]-gap, a.bounds.mn[2]-gap };
        const double amx[3] = { a.bounds.mx[0]+gap, a.bounds.mx[1]+gap, a.bounds.mx[2]+gap };
        for (size_t j = i + 1; j < n; ++j) {
            const Mesh& b = snap.meshes[j];
            if (b.triangles.empty () || !b.bounds.Valid ()) continue;
            if (!b.bounds.OverlapsBox (amn, amx)) continue;

            if (gap > 0.0) {
                const Clearance c = MeshClearance (a, b, gap);
                if (c.dist >= 0.0 && c.dist <= gap)
                    out.push_back ({ i, j, c.dist });
            } else if (MeshesClash (a, b)) {
                out.push_back ({ i, j, 0.0 });
            }
        }
    }
    return out;
}

} // namespace geomsrv
