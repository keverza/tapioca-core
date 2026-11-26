#include "SunStudy/SunStudySkyMask.hpp"
#include "SunStudy/ITraversal.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace evp::sunstudy {

namespace {

constexpr uint8_t kBlocked = static_cast<uint8_t> (rust::SkyPath::Blocked);
constexpr uint8_t kOpen = static_cast<uint8_t> (rust::SkyPath::Open);

bool Shapes (const std::vector<double>& positions, const std::vector<double>& normals, std::string& error)
{
    if (positions.size () % 3 != 0 || positions.size () != normals.size ()) {
        error = "SkyMask: positions and normals must hold 3 values per sample each";
        return false;
    }
    return true;
}

double Dot (const double* a, const double* b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

} // namespace

bool TraceSkyMasks (const ITraversal& traversal, const std::vector<double>& positions,
                    const std::vector<double>& normals, const std::vector<rust::SkyCell>& cells, size_t patchCount,
                    const SkyMaskOptions& options, const std::function<bool ()>& cancelled, std::vector<uint8_t>& open,
                    std::string& error)
{
    open.clear ();
    if (!Shapes (positions, normals, error))
        return false;
    for (const rust::SkyCell& cell : cells)
        if (cell.patch >= patchCount) {
            error = "SkyMask: a cell names a patch beyond the dome";
            return false;
        }
    const size_t samples = positions.size () / 3;
    std::vector<uint8_t> out (samples * patchCount, 0);
    // Whole samples per batch, so a patch's weights never straddle batches.
    const size_t perBatch = std::max<size_t> (1, options.raysPerBatch / std::max<size_t> (1, cells.size ()));
    std::vector<OcclusionRay> rays;
    std::vector<std::pair<size_t, double>> slots; // (patch row index in the batch, weight)
    std::vector<uint8_t> hits;
    std::vector<double> weight, seen;
    for (size_t first = 0; first < samples; first += perBatch) {
        if (cancelled && cancelled ()) {
            error = "Cancelled";
            return false;
        }
        const size_t last = std::min (samples, first + perBatch);
        rays.clear ();
        slots.clear ();
        weight.assign ((last - first) * patchCount, 0.0);
        seen.assign (weight.size (), 0.0);
        for (size_t s = first; s < last; ++s) {
            const double* p = positions.data () + 3 * s;
            const double* n = normals.data () + 3 * s;
            for (const rust::SkyCell& cell : cells) {
                const double w = Dot (n, cell.moment.data ());
                if (w <= 0.0)
                    continue;
                const size_t row = (s - first) * patchCount + cell.patch;
                weight[row] += w;
                OcclusionRay ray;
                for (int i = 0; i < 3; ++i) {
                    ray.origin[i] = p[i] + n[i] * options.normalOffset;
                    ray.dir[i] = cell.direction[static_cast<size_t> (i)];
                }
                ray.tmin = 0.0;
                ray.tmax = options.maxDistance;
                rays.push_back (ray);
                slots.emplace_back (row, w);
            }
        }
        hits.assign (rays.size (), 0);
        traversal.OccludeRays (rays.data (), rays.size (), hits.data (), options.maxParallel);
        for (size_t r = 0; r < rays.size (); ++r)
            if (hits[r] == 0)
                seen[slots[r].first] += slots[r].second;
        for (size_t row = 0; row < weight.size (); ++row)
            if (weight[row] > 0.0)
                out[first * patchCount + row] =
                    static_cast<uint8_t> (std::lround (rust::kShareFull * std::min (1.0, seen[row] / weight[row])));
    }
    open = std::move (out);
    error.clear ();
    return true;
}

bool TraceBeam (const ITraversal& traversal, const std::vector<double>& positions, const std::vector<double>& normals,
                const std::array<double, 3>& sun, const SkyMaskOptions& options, std::vector<uint8_t>& paths,
                std::string& error)
{
    paths.clear ();
    if (!Shapes (positions, normals, error))
        return false;
    const size_t samples = positions.size () / 3;
    std::vector<double> origins (positions.size ());
    for (size_t s = 0; s < samples; ++s)
        for (size_t i = 0; i < 3; ++i)
            origins[3 * s + i] = positions[3 * s + i] + normals[3 * s + i] * options.normalOffset;
    std::vector<uint8_t> hits (samples, 0);
    traversal.OccludeDirectional (origins.data (), samples, sun.data (), 0.0, options.maxDistance, hits.data (),
                                  options.maxParallel);
    paths.assign (samples, kBlocked);
    for (size_t s = 0; s < samples; ++s)
        if (hits[s] == 0 && Dot (normals.data () + 3 * s, sun.data ()) > 0.0)
            paths[s] = kOpen;
    error.clear ();
    return true;
}

} // namespace evp::sunstudy
