#include "SunStudy/SunStudyRustClimate.hpp"
#include "SunStudy/SunStudyRustDetail.hpp"

#include <algorithm>

namespace evp::sunstudy::rust {

using namespace detail;

static_assert (sizeof (TapiocaSkyPatch) == 56);
static_assert (sizeof (TapiocaSkyCell) == 64);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaClimateCreateRequest) == 160);
static_assert (sizeof (TapiocaClimateInterval) == 72);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaClimateReport) == 328);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaClimateReadOutput) == 48);

bool SkyPatches (SkyPatchSet set, double rotation, std::vector<SkyPatch>& patches, std::string& error)
{
    patches.clear ();
    std::vector<TapiocaSkyPatch> raw;
    size_t count = 0;
    const auto read = [&] (char* m, size_t n) {
        return tapioca_sunstudy_sky_patches (static_cast<uint8_t> (set), rotation, raw.data (), raw.size (), &count, m,
                                             n);
    };
    if (!Invoke (read, error)) // size
        return false;
    raw.assign (count, TapiocaSkyPatch {});
    if (!Invoke (read, error)) // fill
        return false;
    patches.reserve (raw.size ());
    for (const TapiocaSkyPatch& p : raw) {
        SkyPatch patch;
        std::copy (p.direction, p.direction + 3, patch.direction.begin ());
        std::copy (p.moment, p.moment + 3, patch.moment.begin ());
        patch.solidAngle = p.solidAngle;
        patches.push_back (patch);
    }
    return true;
}

bool SkyCells (SkyPatchSet set, double rotation, uint32_t perSide, std::vector<SkyCell>& cells, std::string& error)
{
    cells.clear ();
    std::vector<TapiocaSkyCell> raw;
    size_t count = 0;
    const auto read = [&] (char* m, size_t n) {
        return tapioca_sunstudy_sky_cells (static_cast<uint8_t> (set), rotation, perSide, raw.data (), raw.size (),
                                           &count, m, n);
    };
    if (!Invoke (read, error)) // size
        return false;
    raw.assign (count, TapiocaSkyCell {});
    if (!Invoke (read, error)) // fill
        return false;
    cells.reserve (raw.size ());
    for (const TapiocaSkyCell& c : raw) {
        SkyCell cell;
        std::copy (c.direction, c.direction + 3, cell.direction.begin ());
        std::copy (c.moment, c.moment + 3, cell.moment.begin ());
        cell.solidAngle = c.solidAngle;
        cell.patch = c.patch;
        cells.push_back (cell);
    }
    return true;
}

double ClimateTotals::Total (size_t sample, int month) const
{
    if (sample >= direct.size () || month < 0 || month >= 12)
        return 0.0;
    const auto m = static_cast<size_t> (month);
    return direct[sample][m] + diffuse[sample][m] + reflected[sample][m];
}

RustClimate::RustClimate (TapiocaSunStudyClimate* handle) : handle_ (handle)
{
}

RustClimate::~RustClimate ()
{
    std::string ignored;
    Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_climate_destroy (handle_, m, n); }, ignored);
}

std::unique_ptr<RustClimate> RustClimate::Create (const ClimateSetup& setup, std::string& error)
{
    TapiocaClimateCreateRequest request {};
    request.normals = { setup.normals.data (), setup.normals.size () };
    request.open = { setup.open.data (), setup.open.size () };
    request.canopy = { setup.canopy.data (), setup.canopy.size () };
    std::copy (setup.canopyTransmittance.begin (), setup.canopyTransmittance.end (), request.canopyTransmittance);
    request.rotation = setup.rotation;
    request.patchSet = static_cast<uint8_t> (setup.set);
    request.sky = static_cast<uint8_t> (setup.sky);
    TapiocaSunStudyClimate* handle = nullptr;
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_climate_create (&request, &handle, m, n); }, error))
        return nullptr;
    return std::unique_ptr<RustClimate> (new RustClimate (handle));
}

bool RustClimate::Add (const ClimateInterval& interval, const std::vector<uint8_t>& beam, std::string& error)
{
    if (interval.month < 0 || interval.month > 11) {
        error = "ClimateInterval";
        return false;
    }
    TapiocaClimateInterval record {};
    std::copy (interval.sun.begin (), interval.sun.end (), record.sun);
    record.hours = interval.hours;
    record.dni = interval.dni;
    record.dhi = interval.dhi;
    record.albedo = interval.albedo;
    record.dewPointC = interval.dewPointC;
    record.dayOfYear = interval.dayOfYear;
    record.month = static_cast<uint8_t> (interval.month);
    const TapiocaByteView view { beam.data (), beam.size () };
    return Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_climate_add (handle_, &record, view, m, n); },
                   error);
}

bool RustClimate::Read (ClimateTotals& totals, std::string& error) const
{
    TapiocaClimateReport report {};
    TapiocaClimateReadOutput output {};
    output.report = &report;
    const auto read = [&] (char* m, size_t n) { return tapioca_sunstudy_climate_read (handle_, &output, m, n); };
    if (!Invoke (read, error)) // the report: the sample count
        return false;
    const size_t samples = report.sampleCount;
    std::vector<double> direct (samples * 12), diffuse (samples * 12), reflected (samples * 12), skyView (samples);
    output.direct = direct.data ();
    output.diffuse = diffuse.data ();
    output.reflected = reflected.data ();
    output.skyView = skyView.data ();
    output.capacity = samples;
    if (samples != 0 && !Invoke (read, error))
        return false;
    const auto rows = [samples] (const std::vector<double>& flat) {
        std::vector<Monthly> out (samples);
        for (size_t s = 0; s < samples; ++s)
            std::copy (flat.begin () + static_cast<std::ptrdiff_t> (s * 12),
                       flat.begin () + static_cast<std::ptrdiff_t> (s * 12 + 12), out[s].begin ());
        return out;
    };
    totals.direct = rows (direct);
    totals.diffuse = rows (diffuse);
    totals.reflected = rows (reflected);
    totals.skyView = std::move (skyView);
    ClimateReport& r = totals.report;
    std::copy (report.validHours, report.validHours + 12, r.validHours.begin ());
    std::copy (report.missingHours, report.missingHours + 12, r.missingHours.begin ());
    std::copy (report.openHorizontal, report.openHorizontal + 12, r.openHorizontal.begin ());
    r.beamBelowHorizon = report.beamBelowHorizon;
    r.maxClampedShare = report.maxClampedShare;
    r.isotropicIntervals = report.isotropicIntervals;
    r.heldIntervals = report.heldIntervals;
    r.sampleCount = report.sampleCount;
    r.patchCount = report.patchCount;
    return true;
}

} // namespace evp::sunstudy::rust
