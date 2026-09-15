#include "SunStudy/SunStudyPatchSampler.hpp"

#include <algorithm>
#include <cmath>

namespace evp::sunstudy {

double PatchSampleGrid::TotalArea () const
{
    // ⚠️ SUMMED IN PATCH ORDER RATHER THAN BY std::accumulate OVER A REORDERING.
    // The total is compared against the triangle sampler's, and a different
    // summation order changes the last bits -- which turns a passing invariant
    // into a flaky one for no reason anybody can see.
    double total = 0.0;
    for (const double area : areas)
        total += area;
    return total;
}

PatchSampleGrid BuildPatchSampleGrid (const double* vertices, size_t vertexCount, const uint32_t* triangles,
                                      size_t faceCount, const uint32_t* groups,
                                      const std::vector<std::string>& elementOf, const PatchSamplerOptions& options)
{
    PatchSampleGrid grid;
    if (vertices == nullptr || triangles == nullptr || faceCount == 0 || !(options.spacing > 0.0))
        return grid;

    const std::vector<SurfacePatch> patches =
        BuildSurfacePatches (vertices, vertexCount, triangles, faceCount, groups, options.patch);
    if (patches.empty ())
        return grid;

    // ⚠️ COUNTED BEFORE ANYTHING IS BUILT. A study that would exceed the ceiling
    // must be REFUSED, not truncated: a truncated sample set is a study of part
    // of the model reported as a study of the model, and every total it produces
    // looks entirely reasonable.
    size_t projected = 0;
    std::vector<PatchGrid> lattices;
    lattices.reserve (patches.size ());
    for (const SurfacePatch& patch : patches) {
        lattices.push_back (BuildPatchGrid (patch, vertices, triangles, options.spacing));
        projected += lattices.back ().cells.size ();
        if (projected > options.maxSamples)
            return grid; // invalid; the caller asks for a coarser grid
    }

    grid.spans.reserve (patches.size ());
    for (size_t index = 0; index < patches.size (); ++index) {
        const SurfacePatch& patch = patches[index];
        const PatchGrid& lattice = lattices[index];
        if (lattice.cells.empty ())
            continue;
        if (lattice.fromCentroid)
            ++grid.centroidPatches;

        PatchSampleSpan span;
        span.key = MakePatchKey (patch.group < elementOf.size () ? elementOf[patch.group] : std::string (), patch);
        span.first = grid.areas.size ();
        span.count = lattice.cells.size ();
        span.columns = lattice.columns;
        span.rows = lattice.rows;
        span.area = patch.area;
        for (int axis = 0; axis < 3; ++axis) {
            span.origin[axis] = patch.origin[axis];
            span.uAxis[axis] = patch.uAxis[axis];
            span.vAxis[axis] = patch.vAxis[axis];
            span.normal[axis] = patch.normal[axis];
        }

        bool haveBounds = false;
        for (const PatchCell& cell : lattice.cells) {
            for (int axis = 0; axis < 3; ++axis) {
                // ⚠️ LIFTED ALONG THE PATCH'S NORMAL, not the triangle's. They
                // agree to within the coplanarity tolerance by construction, and
                // using the patch's keeps every sample of one surface offset the
                // same way -- so a ray's `tmin` means the same thing everywhere
                // on it.
                grid.positions.push_back (cell.centre[axis] + patch.normal[axis] * options.normalOffset);
            }
            for (int axis = 0; axis < 3; ++axis)
                grid.normals.push_back (patch.normal[axis]);
            grid.areas.push_back (cell.area);
            grid.cellColumns.push_back (cell.column);
            grid.cellRows.push_back (cell.row);
            grid.spanOf.push_back (static_cast<uint32_t> (grid.spans.size ()));

            // The patch's AABB from its SAMPLES would be too small -- a sample
            // sits inside the surface, and an occluder test needs the surface.
            // Take it from the cells' own extent instead, which is the lattice.
            if (!haveBounds) {
                for (int axis = 0; axis < 3; ++axis)
                    span.boundsMin[axis] = span.boundsMax[axis] = cell.centre[axis];
                haveBounds = true;
            }
            else {
                for (int axis = 0; axis < 3; ++axis) {
                    span.boundsMin[axis] = std::min (span.boundsMin[axis], cell.centre[axis]);
                    span.boundsMax[axis] = std::max (span.boundsMax[axis], cell.centre[axis]);
                }
            }
        }

        // ⚠️ AND THEN GROWN TO THE PATCH'S OWN CORNERS. The cells' extent stops
        // half a cell inside the surface; a dirty classifier testing that box
        // against a shadow envelope would miss a shadow that falls only on the
        // outer strip -- an under-dirty, which is the one failure the classifier
        // must never produce.
        for (const uint32_t face : patch.triangles) {
            for (int corner = 0; corner < 3; ++corner) {
                const uint32_t vertex = triangles[face * 3 + corner];
                for (int axis = 0; axis < 3; ++axis) {
                    const double value = vertices[vertex * 3 + axis];
                    span.boundsMin[axis] = std::min (span.boundsMin[axis], value);
                    span.boundsMax[axis] = std::max (span.boundsMax[axis], value);
                }
            }
        }

        grid.spans.push_back (span);
    }

    grid.valid = !grid.spans.empty ();
    return grid;
}

} // namespace evp::sunstudy
