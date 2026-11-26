#ifndef EVP_SUNSTUDY_SUNSTUDYRUSTCLIMATE_HPP
#define EVP_SUNSTUDY_SUNSTUDYRUSTCLIMATE_HPP

// SunStudy/SunStudyRustClimate -- the sky patch domes (#269) and the climate
// irradiation run (#155 F3, #278 Stage 5) over the Rust sun-study ABI.
//
// A run is built from each sample's normal and the share of every sky patch
// it sees (SunStudy/SunStudySkyMask traces them), then fed one weather interval at a
// time with the beam's path for that interval's sun. Read gives per sample
// and month the direct, diffuse and ground-reflected energy (Wh/m2), and the
// geometric sky view factor beside them -- never mixed into the totals.
//
// The host's sun is the authority: every interval's direction comes from the
// caller, on the project's standard-time clock (#155 P2).
//
// One thread at a time: the runner that owns it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

struct TapiocaSunStudyClimate;

namespace evp::sunstudy::rust {

enum class SkyPatchSet : uint8_t {
    Tregenza = 0, // 145 patches
    Reinhart = 1, // 577 patches
};

// One patch in the model frame.
struct SkyPatch {
    std::array<double, 3> direction {}; // unit centroid: where a mask ray goes
    std::array<double, 3> moment {};    // integral of d dOmega, sr
    double solidAngle = 0.0;            // sr
};

// The dome in the contract's order (rows from the horizon, clockwise from
// north, cap last), turned by `rotation` radians about z: sky north lands on
// (-sin, cos).
bool SkyPatches (SkyPatchSet set, double rotation, std::vector<SkyPatch>& patches, std::string& error);

// One cell of a patch: a mask ray goes along `direction`, weighted on a plane
// with normal n by max(0, n . moment).
struct SkyCell {
    std::array<double, 3> direction {};
    std::array<double, 3> moment {};
    double solidAngle = 0.0;
    uint32_t patch = 0; // in the dome's order
};

// Every patch split into perSide^2 cells (1-16), patch by patch.
bool SkyCells (SkyPatchSet set, double rotation, uint32_t perSide, std::vector<SkyCell>& cells, std::string& error);

// A full patch in a sky-mask share byte.
inline constexpr uint8_t kShareFull = 255;

// The beam's path for one interval.
enum class SkyPath : uint8_t {
    Blocked = 0,
    Canopy = 1, // passes the month's canopy transmittance
    Open = 2,
};

enum class DiffuseSky : uint8_t {
    Perez1993 = 0, // all-weather; a sun below the horizon falls back to isotropic
    Isotropic = 1,
};

struct ClimateSetup {
    SkyPatchSet set = SkyPatchSet::Tregenza;
    double rotation = 0.0; // as SkyPatches
    DiffuseSky sky = DiffuseSky::Perez1993;
    std::vector<double> normals; // 3 per sample, unit, model frame
    std::vector<uint8_t> open;   // per sample, then per patch: share seen open, 0-255
    std::vector<uint8_t> canopy; // the same layout, through canopy only; may be empty
    std::array<double, 12> canopyTransmittance {};
};

// One weather interval on the project's clock.
struct ClimateInterval {
    std::array<double, 3> sun {}; // unit, to the sun at the interval midpoint
    double hours = 1.0;
    double dni = 0.0;                                       // mean W/m2; NaN missing
    double dhi = 0.0;                                       // mean W/m2; NaN missing
    double albedo = 0.0;                                    // outside (0, 1]: 0.2
    double dewPointC = 0.0;                                 // NaN missing
    double ghi = std::numeric_limits<double>::quiet_NaN (); // the file's, W/m2: checked only
    uint32_t dayOfYear = 1;
    int month = 0; // 0 = January

    // The beam needs tracing: sun up and DNI above zero.
    bool NeedsBeam () const
    {
        return sun[2] > 0.0 && dni > 0.0;
    }
};

struct ClimateReport {
    std::array<double, 12> validHours {};
    std::array<double, 12> missingHours {};
    std::array<double, 12> openHorizontal {}; // sum (DNI sin a + DHI) dt, Wh/m2
    // Over intervals with DNI, DHI and GHI present, per month.
    std::array<double, 12> comparedModel {}, comparedFile {}, comparedHours {};
    // How well the file's DNI and DHI rebuild its GHI (#155 P3): (model -
    // file) / file, NaN where a month does not compare. Shown, never absorbed.
    std::array<double, 12> closureMonthly {};
    double closureAnnual = 0.0, closureTolerance = 0.0, closureHours = 0.0;
    int closureWorstMonth = -1; // 0 = January
    bool closureWithin = false;
    double beamBelowHorizon = 0.0; // Wh/m2 lost with the sun down
    double maxClampedShare = 0.0;
    uint32_t isotropicIntervals = 0;
    uint32_t heldIntervals = 0;
    size_t sampleCount = 0;
    size_t patchCount = 0;
};

using Monthly = std::array<double, 12>;

struct ClimateTotals {
    std::vector<Monthly> direct, diffuse, reflected; // Wh/m2, January first
    std::vector<double> skyView;                     // 0-1, open paths only
    ClimateReport report;

    double Total (size_t sample, int month) const;
};

// Daily light integral conversion: an estimate whose PAR fraction and photon
// efficacy are stated assumptions; 0 takes the defaults (0.45, 4.57 umol/J).
struct ParConversion {
    double parFraction = 0.0;
    double micromolPerJoule = 0.0;
};

// An EPW record's stamp, as written.
struct WeatherStamp {
    int month = 0, day = 0, hour = 0, minute = 0;
};

struct WeatherPlacement {
    int year = 2026;
    int recordsPerHour = 1;
    double stationOffsetHours = 0.0; // EPW header time zone
    double projectOffsetHours = 0.0; // project standard time, no DST
    double latitude = 0.0, longitude = 0.0;
    double rotation = 0.0; // as SkyPatches
};

// One record on the project's clock. `independentSun` is the NOAA sun in the
// model frame: a cross-check for the host's, never an analysis input.
struct WeatherInstant {
    bool valid = false;
    double julianDay = 0.0; // UTC, interval midpoint
    double hours = 0.0;
    int projectYear = 0, projectMonth = 0, projectDay = 0;
    double projectSeconds = 0.0;
    uint32_t dayOfYear = 0;
    std::array<double, 3> independentSun {};
};

bool WeatherInstants (const std::vector<WeatherStamp>& stamps, const WeatherPlacement& placement,
                      std::vector<WeatherInstant>& out, std::string& error);

struct SunCrossCheck {
    size_t compared = 0;
    double meanDegrees = 0.0, maxDegrees = 0.0;
    bool accepted = false; // mean <= 0.5 deg, max <= 2 deg (#155 P2)
};

// `host` and `independent`: 3 doubles per instant, same frame.
bool CrossCheckSun (const std::vector<double>& host, const std::vector<double>& independent, SunCrossCheck& out,
                    std::string& error);

class RustClimate final {
  public:
    static std::unique_ptr<RustClimate> Create (const ClimateSetup& setup, std::string& error);
    ~RustClimate ();
    RustClimate (const RustClimate&) = delete;
    RustClimate& operator= (const RustClimate&) = delete;

    // `beam`: one SkyPath per sample; empty when the interval needs no beam.
    // A refused interval changes nothing.
    bool Add (const ClimateInterval& interval, const std::vector<uint8_t>& beam, std::string& error);
    bool Read (ClimateTotals& totals, std::string& error) const;
    // Mean daily light integral, mol/m2/day, per sample and month (NaN for a
    // month without valid weather hours).
    bool DailyLightIntegral (const ParConversion& conversion, std::vector<Monthly>& dli, std::string& error) const;

  private:
    explicit RustClimate (TapiocaSunStudyClimate* handle);
    TapiocaSunStudyClimate* handle_ = nullptr;
};

} // namespace evp::sunstudy::rust

#endif
