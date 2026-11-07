#include "SunStudy/SunStudySolarCut.hpp"

#include "SunStudy/SunStudyPointProfile.hpp"

#include <algorithm>
#include <cmath>

namespace evp::sunstudy {

namespace {

constexpr double kPi = 3.14159265358979323846;

double Wrap (double angle)
{
    while (angle <= -kPi)
        angle += 2.0 * kPi;
    while (angle > kPi)
        angle -= 2.0 * kPi;
    return angle;
}

struct SunAngle {
    double azimuth, tan;
};

SunAngle AngleOf (const SunStep& step)
{
    const double h = std::hypot (step.direction[0], step.direction[1]);
    return { std::atan2 (step.direction[1], step.direction[0]), h > 1.0e-9 ? step.direction[2] / h : 1.0e9 };
}

// A voxel grid over one element's bounds; `inside` per voxel.
struct Grid {
    double origin[3] = {};
    double size = 0.0;
    int n[3] = {};
    std::vector<uint8_t> inside;
    size_t Index (int i, int j, int k) const
    {
        return (size_t (k) * size_t (n[1]) + size_t (j)) * size_t (n[0]) + size_t (i);
    }
};

bool Bounds (const geomsrv::Mesh& mesh, double lo[3], double hi[3])
{
    if (mesh.vertices.size () < 9)
        return false;
    for (int axis = 0; axis < 3; ++axis) {
        lo[axis] = 1.0e300;
        hi[axis] = -1.0e300;
    }
    for (size_t v = 0; v + 2 < mesh.vertices.size (); v += 3)
        for (int axis = 0; axis < 3; ++axis) {
            lo[axis] = (std::min) (lo[axis], mesh.vertices[v + size_t (axis)]);
            hi[axis] = (std::max) (hi[axis], mesh.vertices[v + size_t (axis)]);
        }
    return std::isfinite (lo[0]) && std::isfinite (hi[0]);
}

bool Corner (const geomsrv::Mesh& mesh, uint32_t index, double out[3])
{
    if (3ull * index + 2 >= mesh.vertices.size ())
        return false;
    for (int axis = 0; axis < 3; ++axis)
        out[axis] = mesh.vertices[3ull * index + size_t (axis)];
    return true;
}

// Surface voxels: those a triangle's plane passes within half a diagonal of,
// inside the triangle's bounds. Conservative, so a wall or slab thinner than
// a voxel is never lost between voxel centres.
void MarkSurface (const geomsrv::Mesh& mesh, Grid& grid, const std::function<bool ()>& cancelled)
{
    const double half = grid.size * 0.8660254;
    for (size_t t = 0; t < mesh.TriangleCount (); ++t) {
        if ((t & 1023) == 0 && cancelled && cancelled ())
            return;
        double a[3], b[3], c[3];
        if (!Corner (mesh, mesh.triangles[3 * t], a) || !Corner (mesh, mesh.triangles[3 * t + 1], b) ||
            !Corner (mesh, mesh.triangles[3 * t + 2], c))
            continue;
        const double u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
        const double v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        double normal[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
        const double length = std::sqrt (normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
        if (!(length > 0.0))
            continue;
        for (double& component : normal)
            component /= length;
        int from[3], to[3];
        for (int axis = 0; axis < 3; ++axis) {
            const double lo = (std::min) ({ a[axis], b[axis], c[axis] }) - grid.origin[axis];
            const double hi = (std::max) ({ a[axis], b[axis], c[axis] }) - grid.origin[axis];
            from[axis] = std::clamp (int (std::floor (lo / grid.size)), 0, grid.n[axis] - 1);
            to[axis] = std::clamp (int (std::floor (hi / grid.size)), 0, grid.n[axis] - 1);
        }
        for (int k = from[2]; k <= to[2]; ++k)
            for (int j = from[1]; j <= to[1]; ++j)
                for (int i = from[0]; i <= to[0]; ++i) {
                    const double p[3] = { grid.origin[0] + (i + 0.5) * grid.size - a[0],
                                          grid.origin[1] + (j + 0.5) * grid.size - a[1],
                                          grid.origin[2] + (k + 0.5) * grid.size - a[2] };
                    if (std::abs (p[0] * normal[0] + p[1] * normal[1] + p[2] * normal[2]) <= half)
                        grid.inside[grid.Index (i, j, k)] = 1;
                }
    }
}

// Interior voxels: one ray along +x per voxel row, inside between hit pairs.
// A row with an odd count (an open mesh) adds nothing beyond its surface.
void FillInterior (const geomsrv::Mesh& mesh, Grid& grid, const std::function<bool ()>& cancelled)
{
    // Bin each triangle to the rows its y-z bounds cover.
    std::vector<std::vector<uint32_t>> rows (size_t (grid.n[1]) * size_t (grid.n[2]));
    for (size_t t = 0; t < mesh.TriangleCount (); ++t) {
        double corner[3][3];
        bool valid = true;
        for (int c = 0; c < 3; ++c)
            valid = valid && Corner (mesh, mesh.triangles[3 * t + size_t (c)], corner[c]);
        if (!valid)
            continue;
        int from[2], to[2];
        for (int axis = 1; axis <= 2; ++axis) {
            const double lo = (std::min) ({ corner[0][axis], corner[1][axis], corner[2][axis] }) - grid.origin[axis];
            const double hi = (std::max) ({ corner[0][axis], corner[1][axis], corner[2][axis] }) - grid.origin[axis];
            from[axis - 1] = std::clamp (int (std::floor (lo / grid.size - 0.5)), 0, grid.n[axis] - 1);
            to[axis - 1] = std::clamp (int (std::ceil (hi / grid.size - 0.5)), 0, grid.n[axis] - 1);
        }
        for (int k = from[1]; k <= to[1]; ++k)
            for (int j = from[0]; j <= to[0]; ++j)
                rows[size_t (k) * size_t (grid.n[1]) + size_t (j)].push_back (uint32_t (t));
    }
    std::vector<double> hits;
    for (int k = 0; k < grid.n[2]; ++k) {
        if (cancelled && cancelled ())
            return;
        for (int j = 0; j < grid.n[1]; ++j) {
            const auto& candidates = rows[size_t (k) * size_t (grid.n[1]) + size_t (j)];
            if (candidates.size () < 2)
                continue;
            // Off the exact centre, so a row never runs along a shared edge.
            const double y = grid.origin[1] + (j + 0.5) * grid.size + 1.3e-7 * grid.size;
            const double z = grid.origin[2] + (k + 0.5) * grid.size + 0.7e-7 * grid.size;
            hits.clear ();
            for (const uint32_t t : candidates) {
                double a[3], b[3], c[3];
                Corner (mesh, mesh.triangles[3 * t], a);
                Corner (mesh, mesh.triangles[3 * t + 1], b);
                Corner (mesh, mesh.triangles[3 * t + 2], c);
                // Barycentrics of (y, z) in the triangle's y-z projection.
                const double d = (b[1] - a[1]) * (c[2] - a[2]) - (c[1] - a[1]) * (b[2] - a[2]);
                if (std::abs (d) < 1.0e-14)
                    continue;
                const double s = ((y - a[1]) * (c[2] - a[2]) - (c[1] - a[1]) * (z - a[2])) / d;
                const double r = ((b[1] - a[1]) * (z - a[2]) - (y - a[1]) * (b[2] - a[2])) / d;
                if (s < 0.0 || r < 0.0 || s + r > 1.0)
                    continue;
                hits.push_back (a[0] + s * (b[0] - a[0]) + r * (c[0] - a[0]));
            }
            if (hits.size () < 2 || (hits.size () & 1) != 0)
                continue;
            std::sort (hits.begin (), hits.end ());
            for (size_t h = 0; h + 1 < hits.size (); h += 2) {
                const int i0 = (std::max) (0, int (std::ceil ((hits[h] - grid.origin[0]) / grid.size - 0.5)));
                const int i1 =
                    (std::min) (grid.n[0] - 1, int (std::floor ((hits[h + 1] - grid.origin[0]) / grid.size - 0.5)));
                for (int i = i0; i <= i1; ++i)
                    grid.inside[grid.Index (i, j, k)] = 1;
            }
        }
    }
}

} // namespace

bool SolarEnvelopeTan (const std::vector<SunStep>& steps, double a0, double a1, double& tanMin, double& tanMax)
{
    tanMin = 1.0e300;
    tanMax = -1.0e300;
    bool covered = false;
    const double centre = 0.5 * (a0 + a1), half = 0.5 * (a1 - a0);
    const auto take = [&] (double tan) {
        tanMin = (std::min) (tanMin, tan);
        tanMax = (std::max) (tanMax, tan);
        covered = true;
    };
    if (steps.size () == 1) {
        const SunAngle sun = AngleOf (steps[0]);
        if (half >= kPi || std::abs (Wrap (sun.azimuth - centre)) <= half)
            take (sun.tan);
        return covered;
    }
    for (size_t s = 0; s + 1 < steps.size (); ++s) {
        const SunAngle from = AngleOf (steps[s]), to = AngleOf (steps[s + 1]);
        const double span = Wrap (to.azimuth - from.azimuth);
        const double lo = (std::min) (0.0, span), hi = (std::max) (0.0, span);
        // The query relative to the segment's start, unwrapped near it.
        const double q = Wrap (centre - from.azimuth);
        const double qLo = half >= kPi ? lo : (std::max) (lo, q - half);
        const double qHi = half >= kPi ? hi : (std::min) (hi, q + half);
        if (qLo > qHi)
            continue;
        const auto tanAt = [&] (double x) {
            return std::abs (span) < 1.0e-12 ? (std::min) (from.tan, to.tan)
                                             : from.tan + (to.tan - from.tan) * (x / span);
        };
        take (tanAt (qLo));
        take (tanAt (qHi));
    }
    return covered;
}

bool ComputeSolarCut (const geomsrv::Snapshot& snapshot, const SolarCutInput& input, SolarCutResult& result,
                      const std::function<bool (const double direction[3])>& lit,
                      const std::function<bool ()>& cancelled)
{
    result = SolarCutResult ();
    const double* p = input.point;
    for (const SunStep& step : input.steps) {
        double direction[3] = { step.direction[0], step.direction[1], step.direction[2] };
        const double length =
            std::sqrt (direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
        if (!(length > 0.0) || !std::isfinite (length)) {
            result.error = "a sun direction in the slice is not finite";
            return false;
        }
        for (double& component : direction)
            component /= length;
        if (direction[2] <= 0.0 || !FacesSun (input.normal, direction))
            continue;
        result.facingHours += input.hoursPerStep;
        if (lit && lit (direction))
            result.litHours += input.hoursPerStep;
        SunStep facing = step;
        std::copy (direction, direction + 3, facing.direction);
        result.facingSteps.push_back (facing);
    }
    const auto& suns = result.facingSteps;
    if (suns.empty ())
        return true; // the sun is never in front of the point in this slice: nothing to remove helps

    for (size_t m = 0; m < snapshot.meshes.size (); ++m) {
        if (cancelled && cancelled ())
            return false;
        if (m == input.ownMesh || m >= input.blocks.size () || input.blocks[m] == 0)
            continue;
        const geomsrv::Mesh& mesh = snapshot.meshes[m];
        double lo[3], hi[3];
        if (mesh.TriangleCount () == 0 || !Bounds (mesh, lo, hi) || hi[2] <= p[2])
            continue;
        // Quick rejects: outside the slice's azimuths, or wholly under the envelope.
        const double nearX = std::clamp (p[0], lo[0], hi[0]), nearY = std::clamp (p[1], lo[1], hi[1]);
        const double nearest = std::hypot (nearX - p[0], nearY - p[1]);
        double a0 = -kPi, a1 = kPi;
        if (nearest > 1.0e-9) {
            const double corners[4][2] = { { lo[0], lo[1] }, { hi[0], lo[1] }, { hi[0], hi[1] }, { lo[0], hi[1] } };
            const double reference = std::atan2 (0.5 * (lo[1] + hi[1]) - p[1], 0.5 * (lo[0] + hi[0]) - p[0]);
            double minRel = 0.0, maxRel = 0.0;
            for (const auto& corner : corners) {
                const double rel = Wrap (std::atan2 (corner[1] - p[1], corner[0] - p[0]) - reference);
                minRel = (std::min) (minRel, rel);
                maxRel = (std::max) (maxRel, rel);
            }
            a0 = reference + minRel;
            a1 = reference + maxRel;
        }
        double tanMin = 0.0, tanMax = 0.0;
        if (!SolarEnvelopeTan (suns, a0, a1, tanMin, tanMax) || hi[2] - p[2] <= nearest * tanMin)
            continue;

        Grid grid;
        const double extent[3] = { hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] };
        grid.size = (std::max) (input.voxel, 0.01);
        const double volume = (extent[0] + grid.size) * (extent[1] + grid.size) * (extent[2] + grid.size);
        if (volume / (grid.size * grid.size * grid.size) > double (input.voxelCapPerMesh))
            grid.size = std::cbrt (volume / double (input.voxelCapPerMesh));
        for (int axis = 0; axis < 3; ++axis) {
            grid.n[axis] = (std::max) (1, int (std::ceil (extent[axis] / grid.size)) + 1);
            grid.origin[axis] = 0.5 * (lo[axis] + hi[axis]) - 0.5 * grid.n[axis] * grid.size;
        }
        grid.inside.assign (size_t (grid.n[0]) * size_t (grid.n[1]) * size_t (grid.n[2]), 0);
        MarkSurface (mesh, grid, cancelled);
        FillInterior (mesh, grid, cancelled);
        if (cancelled && cancelled ())
            return false;

        SolarCutObject object;
        object.mesh = m;
        object.guid = mesh.guid;
        object.elemType = mesh.elemType;
        object.voxel = grid.size;
        const double r = grid.size * 0.70710678;
        const double cube = grid.size * grid.size * grid.size;
        for (int k = 0; k < grid.n[2]; ++k)
            for (int j = 0; j < grid.n[1]; ++j)
                for (int i = 0; i < grid.n[0]; ++i) {
                    if (grid.inside[grid.Index (i, j, k)] == 0)
                        continue;
                    ++object.voxels;
                    const double c[3] = { grid.origin[0] + (i + 0.5) * grid.size,
                                          grid.origin[1] + (j + 0.5) * grid.size,
                                          grid.origin[2] + (k + 0.5) * grid.size };
                    const double top = c[2] + 0.5 * grid.size - p[2], bottom = top - grid.size;
                    if (top <= 0.0)
                        continue;
                    const double d = std::hypot (c[0] - p[0], c[1] - p[1]);
                    const double halfAngle = d > r ? std::asin (r / d) : kPi;
                    const double azimuth = std::atan2 (c[1] - p[1], c[0] - p[0]);
                    double lowTan = 0.0, highTan = 0.0;
                    if (!SolarEnvelopeTan (suns, azimuth - halfAngle, azimuth + halfAngle, lowTan, highTan))
                        continue;
                    const double lowest = (std::max) (0.0, d - r) * lowTan;
                    if (top <= lowest)
                        continue; // wholly under every ray: it stays
                    SolarCutVoxel voxel;
                    voxel.centre[0] = float (c[0]);
                    voxel.centre[1] = float (c[1]);
                    voxel.centre[2] = float (c[2]);
                    voxel.size = float (grid.size);
                    voxel.mesh = uint32_t (m);
                    voxel.index[0] = i;
                    voxel.index[1] = j;
                    voxel.index[2] = k;
                    voxel.ray = bottom <= (d + r) * highTan; // a ray may pass through it
                    ++object.envelopeVoxels;
                    result.envelopeVolume += cube;
                    if (voxel.ray) {
                        ++object.rayVoxels;
                        result.rayVolume += cube;
                    }
                    result.removed.push_back (voxel);
                }
        if (object.envelopeVoxels > 0)
            result.objects.push_back (std::move (object));
    }
    std::stable_sort (result.objects.begin (), result.objects.end (),
                      [] (const SolarCutObject& a, const SolarCutObject& b) {
                          return double (a.envelopeVoxels) * a.voxel * a.voxel * a.voxel >
                                 double (b.envelopeVoxels) * b.voxel * b.voxel * b.voxel;
                      });
    return true;
}

} // namespace evp::sunstudy
