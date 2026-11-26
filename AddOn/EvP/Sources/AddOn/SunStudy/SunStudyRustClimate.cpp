#include "SunStudy/SunStudyRustClimate.hpp"
#include "SunStudy/SunStudyRustDetail.hpp"

#include <algorithm>

namespace evp::sunstudy::rust {

using namespace detail;

static_assert (sizeof (TapiocaSkyPatch) == 56);
static_assert (sizeof (TapiocaSkyCell) == 64);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaClimateCreateRequest) == 168);
static_assert (sizeof (TapiocaClimateInterval) == 104);
static_assert (sizeof (TapiocaWeatherStamp) == 4);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaWeatherInstantRequest) == 64);
static_assert (sizeof (TapiocaWeatherInstant) == 64);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaSunCrossCheck) == 32);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaClimateReport) == 1040);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaClimateReadOutput) == 64);

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

bool WeatherInstants (const std::vector<WeatherStamp>& stamps, const WeatherPlacement& placement,
                      std::vector<WeatherInstant>& out, std::string& error)
{
    out.clear ();
    // 255 is out of every field's range: Rust refuses the stamp.
    const auto narrow = [] (int v) { return static_cast<uint8_t> (v < 0 || v > 255 ? 255 : v); };
    std::vector<TapiocaWeatherStamp> raw;
    raw.reserve (stamps.size ());
    for (const WeatherStamp& st : stamps)
        raw.push_back ({ narrow (st.month), narrow (st.day), narrow (st.hour), narrow (st.minute) });
    TapiocaWeatherInstantRequest request {};
    request.stamps = { raw.data (), raw.size () };
    request.year = placement.year;
    request.recordsPerHour = static_cast<uint32_t> (std::max (0, placement.recordsPerHour));
    request.stationOffsetHours = placement.stationOffsetHours;
    request.projectOffsetHours = placement.projectOffsetHours;
    request.latitude = placement.latitude;
    request.longitude = placement.longitude;
    request.rotation = placement.rotation;
    std::vector<TapiocaWeatherInstant> written (raw.size ());
    const auto call = [&] (char* m, size_t n) {
        return tapioca_sunstudy_weather_instants (&request, written.data (), written.size (), m, n);
    };
    if (!Invoke (call, error))
        return false;
    out.reserve (written.size ());
    for (const TapiocaWeatherInstant& w : written) {
        WeatherInstant instant;
        instant.valid = w.valid != 0;
        instant.julianDay = w.julianDay;
        instant.hours = w.hours;
        instant.projectYear = w.projectYear;
        instant.projectMonth = w.projectMonth;
        instant.projectDay = w.projectDay;
        instant.projectSeconds = w.projectSeconds;
        instant.dayOfYear = w.dayOfYear;
        std::copy (w.independentSun, w.independentSun + 3, instant.independentSun.begin ());
        out.push_back (instant);
    }
    return true;
}

bool CrossCheckSun (const std::vector<double>& host, const std::vector<double>& independent, SunCrossCheck& out,
                    std::string& error)
{
    TapiocaSunCrossCheck record {};
    const TapiocaDoubleView a { host.data (), host.size () };
    const TapiocaDoubleView b { independent.data (), independent.size () };
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_sun_cross_check (a, b, &record, m, n); }, error))
        return false;
    out.compared = record.compared;
    out.meanDegrees = record.meanDegrees;
    out.maxDegrees = record.maxDegrees;
    out.accepted = record.accepted != 0;
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
    request.luxSource = setup.luxEfficacyOnly ? 1 : 0;
    request.luxThreshold = setup.luxThreshold;
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
    record.ghi = interval.ghi;
    std::copy (interval.fileLux.begin (), interval.fileLux.end (), record.fileLux);
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
    const bool lux = report.luxKept != 0;
    std::vector<double> luxHours (lux ? samples * 12 : 0), hoursAbove (lux ? samples * 12 : 0);
    output.direct = direct.data ();
    output.diffuse = diffuse.data ();
    output.reflected = reflected.data ();
    output.skyView = skyView.data ();
    output.luxHours = lux ? luxHours.data () : nullptr;
    output.hoursAbove = lux ? hoursAbove.data () : nullptr;
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
    totals.luxHours = lux ? rows (luxHours) : std::vector<Monthly> {};
    totals.hoursAbove = lux ? rows (hoursAbove) : std::vector<Monthly> {};
    ClimateReport& r = totals.report;
    std::copy (report.validHours, report.validHours + 12, r.validHours.begin ());
    std::copy (report.missingHours, report.missingHours + 12, r.missingHours.begin ());
    std::copy (report.openHorizontal, report.openHorizontal + 12, r.openHorizontal.begin ());
    std::copy (report.comparedModel, report.comparedModel + 12, r.comparedModel.begin ());
    std::copy (report.comparedFile, report.comparedFile + 12, r.comparedFile.begin ());
    std::copy (report.comparedHours, report.comparedHours + 12, r.comparedHours.begin ());
    std::copy (report.closureMonthly, report.closureMonthly + 12, r.closureMonthly.begin ());
    r.closureAnnual = report.closureAnnual;
    r.closureTolerance = report.closureTolerance;
    r.closureHours = report.closureHours;
    r.closureWorstMonth = report.closureWorstMonth;
    r.closureWithin = report.closureWithin != 0;
    r.luxKept = lux;
    r.luxThreshold = report.luxThreshold;
    std::copy (report.luxFileHours, report.luxFileHours + 12, r.luxFileHours.begin ());
    std::copy (report.luxEfficacyHours, report.luxEfficacyHours + 12, r.luxEfficacyHours.begin ());
    std::copy (report.luxUnavailableHours, report.luxUnavailableHours + 12, r.luxUnavailableHours.begin ());
    r.beamBelowHorizon = report.beamBelowHorizon;
    r.maxClampedShare = report.maxClampedShare;
    r.isotropicIntervals = report.isotropicIntervals;
    r.heldIntervals = report.heldIntervals;
    r.sampleCount = report.sampleCount;
    r.patchCount = report.patchCount;
    return true;
}

bool RustClimate::DailyLightIntegral (const ParConversion& conversion, std::vector<Monthly>& dli,
                                      std::string& error) const
{
    dli.clear ();
    ClimateTotals totals;
    if (!Read (totals, error)) // the sample count
        return false;
    const size_t samples = totals.report.sampleCount;
    std::vector<double> flat (samples * 12);
    const auto call = [&] (char* m, size_t n) {
        return tapioca_sunstudy_climate_dli (handle_, conversion.parFraction, conversion.micromolPerJoule, flat.data (),
                                             samples, m, n);
    };
    if (samples != 0 && !Invoke (call, error))
        return false;
    dli.resize (samples);
    for (size_t s = 0; s < samples; ++s)
        std::copy (flat.begin () + static_cast<std::ptrdiff_t> (s * 12),
                   flat.begin () + static_cast<std::ptrdiff_t> (s * 12 + 12), dli[s].begin ());
    return true;
}

} // namespace evp::sunstudy::rust
