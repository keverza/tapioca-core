#include "QueryEngine.hpp"

#include <nanort.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace geomsrv {

// nanort BVH lives here so nanort.h never leaks into the public header (and its
// heavy template instantiation is confined to this translation unit).
struct QueryEngine::Impl {
    nanort::BVHAccel<double> accel;
};

namespace {

inline void Sub (const double a[3], const double b[3], double o[3])
{
    o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2];
}
inline double Dot (const double a[3], const double b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline void Cross (const double a[3], const double b[3], double o[3])
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

// Distance from point p to AABB (0 if inside).
double PointAabbDist (const double p[3], const Aabb& b)
{
    double d2 = 0.0;
    for (int i = 0; i < 3; ++i) {
        const double e = p[i] < b.mn[i] ? b.mn[i] - p[i]
                       : (p[i] > b.mx[i] ? p[i] - b.mx[i] : 0.0);
        d2 += e * e;
    }
    return std::sqrt (d2);
}

} // namespace

QueryEngine::QueryEngine (std::shared_ptr<const Snapshot> snap)
    : snapshot (std::move (snap))
{
    // Concatenate every mesh's geometry into flat arrays, offsetting each mesh's
    // local vertex indices by its base so the BVH sees one global triangle soup.
    size_t totalVerts = 0, totalTris = 0;
    for (const auto& m : snapshot->meshes) {
        totalVerts += m.VertexCount ();
        totalTris  += m.TriangleCount ();
    }
    verts.reserve (totalVerts * 3);
    norms.reserve (totalVerts * 3);
    faces.reserve (totalTris * 3);
    triToMesh.reserve (totalTris);

    for (size_t mi = 0; mi < snapshot->meshes.size (); ++mi) {
        const Mesh& m = snapshot->meshes[mi];
        const uint32_t base = static_cast<uint32_t> (verts.size () / 3);

        verts.insert (verts.end (), m.vertices.begin (), m.vertices.end ());
        // Normals may be absent/short for some meshes; pad to vertex count.
        norms.insert (norms.end (), m.normals.begin (), m.normals.end ());
        norms.resize (verts.size (), 0.0f);

        for (size_t t = 0; t < m.triangles.size (); t += 3) {
            faces.push_back (base + m.triangles[t + 0]);
            faces.push_back (base + m.triangles[t + 1]);
            faces.push_back (base + m.triangles[t + 2]);
            triToMesh.push_back (static_cast<uint32_t> (mi));
        }
    }

    impl = std::make_unique<Impl> ();
    if (!triToMesh.empty ()) {
        nanort::TriangleMesh<double> tm (verts.data (), faces.data (), sizeof (double) * 3);
        nanort::TriangleSAHPred<double> pred (verts.data (), faces.data (), sizeof (double) * 3);
        nanort::BVHBuildOptions<double> opts;   // defaults are fine
        impl->accel.Build (static_cast<unsigned int> (triToMesh.size ()), tm, pred, opts);
    }
}

QueryEngine::~QueryEngine () = default;

QueryEngine::RayHit QueryEngine::Raycast (const double org[3], const double dir[3], double maxDist) const
{
    RayHit out;
    if (triToMesh.empty ())
        return out;

    double d[3] = { dir[0], dir[1], dir[2] };
    const double len = std::sqrt (Dot (d, d));
    if (len <= 0.0)
        return out;
    d[0] /= len; d[1] /= len; d[2] /= len;   // normalise so t is a true distance

    nanort::Ray<double> ray;
    ray.org[0] = org[0]; ray.org[1] = org[1]; ray.org[2] = org[2];
    ray.dir[0] = d[0];   ray.dir[1] = d[1];   ray.dir[2] = d[2];
    ray.min_t = 0.0;
    ray.max_t = (maxDist > 0.0) ? maxDist : std::numeric_limits<double>::max ();

    nanort::TriangleIntersector<double, nanort::TriangleIntersection<double>>
        isector (verts.data (), faces.data (), sizeof (double) * 3);
    nanort::TriangleIntersection<double> isect;

    if (!impl->accel.Traverse (ray, isector, &isect))
        return out;

    out.hit = true;
    out.tri = isect.prim_id;
    out.meshIndex = triToMesh[isect.prim_id];
    out.t = isect.t;
    for (int i = 0; i < 3; ++i)
        out.point[i] = org[i] + d[i] * isect.t;

    SurfaceNormal (isect.prim_id, isect.u, isect.v, out.normal);
    return out;
}

void QueryEngine::SurfaceNormal (uint32_t globalTri, double u, double v, double out[3]) const
{
    // Interpolate the smooth per-vertex normal by barycentric (u,v):
    //   P = (1-u-v)*v0 + u*v1 + v*v2   (nanort / Möller–Trumbore convention)
    const uint32_t i0 = faces[globalTri * 3 + 0];
    const uint32_t i1 = faces[globalTri * 3 + 1];
    const uint32_t i2 = faces[globalTri * 3 + 2];
    const double w0 = 1.0 - u - v, w1 = u, w2 = v;
    double n[3];
    for (int i = 0; i < 3; ++i)
        n[i] = w0 * norms[i0 * 3 + i] + w1 * norms[i1 * 3 + i] + w2 * norms[i2 * 3 + i];
    double nl = std::sqrt (Dot (n, n));
    if (nl < 1e-12) {
        // Fall back to the geometric face normal if smooth normals were absent.
        double e1[3], e2[3];
        Sub (&verts[i1 * 3], &verts[i0 * 3], e1);
        Sub (&verts[i2 * 3], &verts[i0 * 3], e2);
        Cross (e1, e2, n);
        nl = std::sqrt (Dot (n, n));
    }
    if (nl > 0.0) { n[0] /= nl; n[1] /= nl; n[2] /= nl; }
    out[0] = n[0]; out[1] = n[1]; out[2] = n[2];
}

// ---------------------------------------------------------------------------
// All-hits ("pierce") raycast.
//
// nanort's own MultiHitTraverse is `#if 0`-ed out and unfinished, so we walk the
// BVH it already built (GetNodes/GetIndices are public) and collect every
// triangle intersection ourselves. This reuses the existing acceleration
// structure — no second BVH — and lets us keep exact semantics: no back-face
// culling, no epsilon ray-advancing, and coincident hits preserved.
// ---------------------------------------------------------------------------

QueryEngine::PierceResult QueryEngine::RaycastAll (const double org[3], const double dir[3],
                                                   double maxDist, size_t maxHits) const
{
    PierceResult out;
    if (triToMesh.empty ())
        return out;

    double d[3] = { dir[0], dir[1], dir[2] };
    const double len = std::sqrt (Dot (d, d));
    if (len <= 0.0)
        return out;
    d[0] /= len; d[1] /= len; d[2] /= len;   // normalise so t is a true distance

    const double tMin = 0.0;
    const double tMax = (maxDist > 0.0) ? maxDist : std::numeric_limits<double>::max ();

    // Safe reciprocal: a zero component yields +/-inf, which the slab test below
    // handles correctly as long as we order with min/max (never 0 * inf).
    double inv[3];
    for (int i = 0; i < 3; ++i)
        inv[i] = (d[i] != 0.0) ? 1.0 / d[i] : std::numeric_limits<double>::infinity ();

    auto hitsAabb = [&] (const double bmin[3], const double bmax[3]) {
        double t0 = tMin, t1 = tMax;
        for (int i = 0; i < 3; ++i) {
            double a = (bmin[i] - org[i]) * inv[i];
            double b = (bmax[i] - org[i]) * inv[i];
            if (a > b) std::swap (a, b);
            if (a > t0) t0 = a;
            if (b < t1) t1 = b;
            if (t0 > t1) return false;
        }
        return true;
    };

    const auto& nodes = impl->accel.GetNodes ();
    const auto& prims = impl->accel.GetIndices ();
    if (nodes.empty ())
        return out;

    // Möller–Trumbore, two-sided (no back-face culling) so we see exits too.
    auto hitTri = [&] (uint32_t gt, double& t, double& u, double& v) {
        const uint32_t i0 = faces[gt * 3 + 0], i1 = faces[gt * 3 + 1], i2 = faces[gt * 3 + 2];
        const double* a = &verts[i0 * 3];
        const double* b = &verts[i1 * 3];
        const double* c = &verts[i2 * 3];
        double e1[3], e2[3];
        Sub (b, a, e1); Sub (c, a, e2);
        double pv[3]; Cross (d, e2, pv);
        const double det = Dot (e1, pv);
        if (std::fabs (det) < 1e-15) return false;      // ray parallel to triangle
        const double invDet = 1.0 / det;
        double tv[3]; Sub (org, a, tv);
        u = Dot (tv, pv) * invDet;
        if (u < 0.0 || u > 1.0) return false;
        double qv[3]; Cross (tv, e1, qv);
        v = Dot (d, qv) * invDet;
        if (v < 0.0 || u + v > 1.0) return false;
        t = Dot (e2, qv) * invDet;
        return t >= tMin && t <= tMax;
    };

    std::vector<uint32_t> stack;
    stack.reserve (64);
    stack.push_back (0);

    while (!stack.empty ()) {
        const uint32_t ni = stack.back ();
        stack.pop_back ();
        const auto& node = nodes[ni];
        if (!hitsAabb (node.bmin, node.bmax))
            continue;

        if (node.flag == 0) {              // branch: data[] are child node indices
            stack.push_back (node.data[0]);
            stack.push_back (node.data[1]);
            continue;
        }
        // leaf: data[0] = primitive count, data[1] = offset into indices_
        const uint32_t n   = node.data[0];
        const uint32_t off = node.data[1];
        for (uint32_t k = 0; k < n; ++k) {
            const uint32_t gt = prims[off + k];
            double t, u, v;
            if (!hitTri (gt, t, u, v))
                continue;

            PierceHit h;
            h.tri = gt;
            h.meshIndex = triToMesh[gt];
            h.t = t;
            for (int i = 0; i < 3; ++i)
                h.point[i] = org[i] + d[i] * t;

            double nrm[3];
            SurfaceNormal (gt, u, v, nrm);
            h.normal[0] = nrm[0]; h.normal[1] = nrm[1]; h.normal[2] = nrm[2];
            h.enter = Dot (nrm, d) < 0.0;   // facing the ray -> entering the solid
            out.hits.push_back (h);
        }
    }

    std::sort (out.hits.begin (), out.hits.end (),
               [] (const PierceHit& a, const PierceHit& b) { return a.t < b.t; });

    // Collapse triangulation seams: when a ray crosses the shared edge (or vertex)
    // of two triangles, BOTH report a hit at the same t. A single element cannot
    // legitimately present two distinct surfaces at one t, so same-element hits at
    // the same t are a tessellation artifact and are merged. Hits from DIFFERENT
    // elements at the same t are real (coincident faces) and are kept — we never
    // silently drop an element from the stack.
    if (out.hits.size () > 1) {
        constexpr double kSameT = 1e-9;
        std::vector<PierceHit> dedup;
        dedup.reserve (out.hits.size ());
        for (const auto& h : out.hits) {
            bool dup = false;
            for (auto it = dedup.rbegin (); it != dedup.rend (); ++it) {
                if (h.t - it->t > kSameT)
                    break;                       // sorted: no earlier hit can match
                if (it->meshIndex == h.meshIndex) { dup = true; break; }
            }
            if (!dup)
                dedup.push_back (h);
        }
        out.hits.swap (dedup);
    }

    if (maxHits > 0 && out.hits.size () > maxHits) {
        out.hits.resize (maxHits);     // keep the NEAREST maxHits
        out.truncated = true;          // and say so — never a silent wrong answer
    }
    return out;
}

double QueryEngine::ClosestOnTri (const double p[3], uint32_t globalTri, double outPoint[3]) const
{
    // Closest point on a triangle to p (Ericson, Real-Time Collision Detection).
    const double* a = &verts[faces[globalTri * 3 + 0] * 3];
    const double* b = &verts[faces[globalTri * 3 + 1] * 3];
    const double* c = &verts[faces[globalTri * 3 + 2] * 3];

    double ab[3], ac[3], ap[3];
    Sub (b, a, ab); Sub (c, a, ac); Sub (p, a, ap);
    const double d1 = Dot (ab, ap), d2 = Dot (ac, ap);
    double bx, by, bz;   // barycentric-ish result stored as a point

    auto setFrom = [&] (double u, double v, double w) {
        bx = u * a[0] + v * b[0] + w * c[0];
        by = u * a[1] + v * b[1] + w * c[1];
        bz = u * a[2] + v * b[2] + w * c[2];
    };

    if (d1 <= 0.0 && d2 <= 0.0) { setFrom (1, 0, 0); }
    else {
        double bp[3]; Sub (p, b, bp);
        const double d3 = Dot (ab, bp), d4 = Dot (ac, bp);
        if (d3 >= 0.0 && d4 <= d3) { setFrom (0, 1, 0); }
        else {
            const double vc = d1 * d4 - d3 * d2;
            if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
                const double v = d1 / (d1 - d3);
                setFrom (1 - v, v, 0);
            } else {
                double cp[3]; Sub (p, c, cp);
                const double d5 = Dot (ab, cp), d6 = Dot (ac, cp);
                if (d6 >= 0.0 && d5 <= d6) { setFrom (0, 0, 1); }
                else {
                    const double vb = d5 * d2 - d1 * d6;
                    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
                        const double w = d2 / (d2 - d6);
                        setFrom (1 - w, 0, w);
                    } else {
                        const double va = d3 * d6 - d5 * d4;
                        if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
                            const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
                            setFrom (0, 1 - w, w);
                        } else {
                            const double denom = 1.0 / (va + vb + vc);
                            const double v = vb * denom, w = vc * denom;
                            setFrom (1 - v - w, v, w);
                        }
                    }
                }
            }
        }
    }
    outPoint[0] = bx; outPoint[1] = by; outPoint[2] = bz;
    const double dx = p[0] - bx, dy = p[1] - by, dz = p[2] - bz;
    return dx * dx + dy * dy + dz * dz;
}

QueryEngine::ClosestHit QueryEngine::ClosestPoint (const double p[3], double maxDist) const
{
    ClosestHit out;
    if (triToMesh.empty ())
        return out;

    double best2 = (maxDist > 0.0) ? maxDist * maxDist : std::numeric_limits<double>::max ();

    // Prune by element AABB: skip a mesh whose box is already farther than the
    // best surface point found so far. Cheap and exact for the closest surface
    // point. `gt` tracks the global triangle index (faces are grouped by mesh in
    // order), so we can skip a whole mesh's triangles without a lookup table.
    size_t gt = 0;
    for (size_t mi = 0; mi < snapshot->meshes.size (); ++mi) {
        const Mesh& m = snapshot->meshes[mi];
        const size_t triN = m.TriangleCount ();
        if (m.bounds.Valid ()) {
            const double bd = PointAabbDist (p, m.bounds);
            if (bd * bd > best2) { gt += triN; continue; }
        }
        for (size_t t = 0; t < triN; ++t, ++gt) {
            double cp[3];
            const double d2 = ClosestOnTri (p, static_cast<uint32_t> (gt), cp);
            if (d2 < best2) {
                best2 = d2;
                out.found = true;
                out.meshIndex = mi;
                out.point[0] = cp[0]; out.point[1] = cp[1]; out.point[2] = cp[2];
            }
        }
    }
    if (out.found)
        out.dist = std::sqrt (best2);
    return out;
}

std::vector<QueryEngine::Neighbor> QueryEngine::NearestElement (const double p[3], size_t k) const
{
    std::vector<Neighbor> all;
    all.reserve (snapshot->meshes.size ());
    for (size_t mi = 0; mi < snapshot->meshes.size (); ++mi) {
        const Mesh& m = snapshot->meshes[mi];
        if (!m.bounds.Valid ())
            continue;
        all.push_back ({ mi, PointAabbDist (p, m.bounds) });
    }
    if (k == 0 || k > all.size ())
        k = all.size ();
    std::partial_sort (all.begin (), all.begin () + k, all.end (),
                       [] (const Neighbor& a, const Neighbor& b) { return a.dist < b.dist; });
    all.resize (k);
    return all;
}

// ---------------------------------------------------------------------------

std::shared_ptr<const QueryEngine> QueryIndexCache::For (const std::shared_ptr<const Snapshot>& snap)
{
    if (!snap)
        return nullptr;
    std::lock_guard<std::mutex> lock (mtx);
    if (!cached || cached->SnapshotId () != snap->id)
        cached = std::make_shared<const QueryEngine> (snap);
    return cached;
}

} // namespace geomsrv
