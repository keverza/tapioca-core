#include "SunStudy/SunStudyClimateRun.hpp"
#include "SunStudy/ITraversal.hpp"

#include <cmath>
#include <limits>

namespace evp::sunstudy {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN ();

// Spread the traced samples' rows back over the request's samples.
std::vector<rust::Monthly> Spread (const std::vector<rust::Monthly>& traced, const std::vector<size_t>& index,
                                   size_t count)
{
    rust::Monthly missing;
    missing.fill (kNaN);
    std::vector<rust::Monthly> out (count, missing);
    for (size_t k = 0; k < index.size () && k < traced.size (); ++k)
        out[index[k]] = traced[k];
    return out;
}

} // namespace

bool RunClimate (const ITraversal& traversal, const ClimateRunRequest& request, const std::function<bool ()>& cancelled,
                 const ClimateProgress& progress, ClimateRunResult& result, std::string& error)
{
    const size_t count = request.positions.size () / 3;
    if (request.positions.size () % 3 != 0 || request.normals.size () != request.positions.size () ||
        (!request.available.empty () && request.available.size () != count)) {
        error = "ClimateRun: positions, normals and availability must describe the same samples";
        return false;
    }
    // The traced subset, in request order.
    std::vector<size_t> index;
    std::vector<double> positions, normals;
    for (size_t s = 0; s < count; ++s) {
        if (!request.available.empty () && request.available[s] == 0)
            continue;
        index.push_back (s);
        positions.insert (positions.end (), request.positions.begin () + static_cast<std::ptrdiff_t> (3 * s),
                          request.positions.begin () + static_cast<std::ptrdiff_t> (3 * s + 3));
        normals.insert (normals.end (), request.normals.begin () + static_cast<std::ptrdiff_t> (3 * s),
                        request.normals.begin () + static_cast<std::ptrdiff_t> (3 * s + 3));
    }

    std::vector<rust::SkyPatch> patches;
    std::vector<rust::SkyCell> cells;
    if (!rust::SkyPatches (request.set, request.rotation, patches, error) ||
        !rust::SkyCells (request.set, request.rotation, request.cellsPerSide, cells, error))
        return false;
    rust::ClimateSetup setup;
    setup.set = request.set;
    setup.rotation = request.rotation;
    setup.sky = request.sky;
    setup.canopyTransmittance = request.canopyTransmittance;
    setup.normals = normals;
    if (!TraceSkyMasks (traversal, positions, normals, cells, patches.size (), request.mask, cancelled, setup.open,
                        error))
        return false;
    auto run = rust::RustClimate::Create (setup, error);
    if (!run)
        return false;
    const size_t total = request.intervals.size ();
    if (progress)
        progress (0, total);

    size_t beamPasses = 0;
    std::vector<uint8_t> beam;
    for (size_t k = 0; k < total; ++k) {
        if (cancelled && cancelled ()) {
            error = "Cancelled";
            return false;
        }
        const rust::ClimateInterval& interval = request.intervals[k];
        beam.clear ();
        if (interval.NeedsBeam () && !positions.empty ()) {
            if (!TraceBeam (traversal, positions, normals, interval.sun, request.mask, beam, error))
                return false;
            ++beamPasses;
        }
        if (!run->Add (interval, beam, error)) {
            error = "interval " + std::to_string (k) + ": " + error;
            return false;
        }
        if (progress)
            progress (k + 1, total);
    }

    rust::ClimateTotals totals;
    std::vector<rust::Monthly> dli;
    if (!run->Read (totals, error) || !run->DailyLightIntegral (request.par, dli, error))
        return false;
    ClimateRunResult out;
    out.direct = Spread (totals.direct, index, count);
    out.diffuse = Spread (totals.diffuse, index, count);
    out.reflected = Spread (totals.reflected, index, count);
    out.dli = Spread (dli, index, count);
    out.skyView.assign (count, kNaN);
    for (size_t k = 0; k < index.size () && k < totals.skyView.size (); ++k)
        out.skyView[index[k]] = totals.skyView[k];
    out.report = totals.report;
    out.sunSource = request.sunSource;
    out.set = request.set;
    out.cellsPerSide = request.cellsPerSide;
    out.traced = index.size ();
    out.excluded = count - index.size ();
    out.beamPasses = beamPasses;
    result = std::move (out);
    error.clear ();
    return true;
}

double AreaWeightedMean (const std::vector<rust::Monthly>& values, const std::vector<double>& areas, int month)
{
    if (month < 0 || month > 11)
        return kNaN;
    double sum = 0.0, area = 0.0;
    for (size_t s = 0; s < values.size () && s < areas.size (); ++s) {
        const double v = values[s][static_cast<size_t> (month)];
        if (std::isnan (v) || !(areas[s] > 0.0))
            continue;
        sum += v * areas[s];
        area += areas[s];
    }
    return area > 0.0 ? sum / area : kNaN;
}

} // namespace evp::sunstudy
