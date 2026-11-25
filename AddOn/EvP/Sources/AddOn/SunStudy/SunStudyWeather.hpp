#ifndef EVP_SUNSTUDY_SUNSTUDYWEATHER_HPP
#define EVP_SUNSTUDY_SUNSTUDYWEATHER_HPP

// SunStudy/SunStudyWeather -- EnergyPlus weather (EPW) files (#155 P1).
//
// Parsing and validation are the Rust `weather` module; this file reads the
// bytes from disk and copies the result into C++ types.
//
// ⚠️ A RECORD COVERS THE INTERVAL BEFORE ITS TIMESTAMP, in the station's local
// STANDARD time (no daylight saving). Hour 1 is 00:00-01:00.
//
// ⚠️ RADIATION IS A MEAN RATE (Wh/m² per hour = W/m²). Energy over a record
// is value × IntervalHours (); summing raw values of a 15-minute file gives
// four times the energy.
//
// ⚠️ NaN IS MISSING. A missing marker or an out-of-range value is NaN and is
// counted in WeatherIdentity::missing / outOfRange; nothing is zero-filled.
//
// ⚠️ NOT YET A SKY OR IRRADIANCE MODEL. Until #155 P2 aligns these instants
// with the host's sun positions, nothing here may feed a light quantity.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace evp::sunstudy {

// Same order as the Rust `weather::Field`.
enum class WeatherField : uint8_t {
    DryBulb,
    DewPoint,
    RelativeHumidity,
    Pressure,
    ExtraterrestrialHorizontal,
    ExtraterrestrialDirectNormal,
    HorizontalInfrared,
    GlobalHorizontal,
    DirectNormal,
    DiffuseHorizontal,
    GlobalIlluminance,
    DirectIlluminance,
    DiffuseIlluminance,
    ZenithLuminance,
    WindDirection,
    WindSpeed,
    TotalSkyCover,
    OpaqueSkyCover,
    Visibility,
    CeilingHeight,
    PrecipitableWater,
    AerosolOpticalDepth,
    SnowDepth,
    DaysSinceSnow,
    Albedo,
    PrecipitationDepth,
    PrecipitationHours,
};
constexpr size_t kWeatherFieldCount = 27;

struct WeatherRecord {
    int32_t year = 0;
    uint8_t month = 0, day = 0;
    uint8_t hour = 0; // 1..24, the hour the interval ends in
    uint8_t minute = 0;
    std::array<double, kWeatherFieldCount> values {};
    double Get (WeatherField f) const
    {
        return values[size_t (f)];
    }
};

struct WeatherDay {
    uint8_t month = 0, day = 0;
    double meanDryBulbC = 0;         // NaN when no valid value
    double globalHorizontalWhM2 = 0; // valid intervals only
    size_t globalMissing = 0;
};

struct WeatherMonth {
    size_t records = 0; // 0 when outside the data period
    size_t globalMissing = 0;
    double meanDryBulbC = 0, minDryBulbC = 0, maxDryBulbC = 0; // NaN when none
    double globalHorizontalKwhM2 = 0;
    double diffuseHorizontalKwhM2 = 0;
    double precipitationMm = 0;
};

// Who measured it, where, and how complete it is.
struct WeatherIdentity {
    std::string city, region, country, source, wmo;
    std::array<std::string, 2> comments;
    double latitude = 0, longitude = 0; // degrees, north/east positive
    double timeZoneHours = 0;           // the file's standard time
    double elevationM = 0;
    bool leapYear = false;
    uint32_t recordsPerHour = 1;
    uint8_t startMonth = 1, startDay = 1, endMonth = 12, endDay = 31;
    std::string sha256; // lower-case hex of the file's bytes
    size_t radiationInDark = 0;
    std::array<size_t, kWeatherFieldCount> missing {};
    std::array<size_t, kWeatherFieldCount> outOfRange {};
};

struct WeatherData {
    WeatherIdentity identity;
    std::vector<WeatherRecord> records;
    std::vector<WeatherDay> days;
    std::array<WeatherMonth, 12> months {};
    double IntervalHours () const
    {
        return 1.0 / double (identity.recordsPerHour ? identity.recordsPerHour : 1);
    }
};

// Parse bytes. False with `error` for a refused file ("line 4021, column 14:
// not a number"); `out` is then empty.
bool ParseWeather (const std::vector<uint8_t>& bytes, WeatherData& out, std::string& error);

// Read and parse a file; adds the path to the error.
bool ReadWeatherFile (const std::filesystem::path& path, WeatherData& out, std::string& error);

// The longest run of days whose centred `windowDays` running mean of the daily
// mean dry bulb stays at or above `thresholdC`. `last < first` when it wraps
// over New Year (only when `wraps`). Indices into WeatherData::days.
struct GrowingSeason {
    bool found = false;
    size_t first = 0, last = 0;
};
bool FindGrowingSeason (const std::vector<WeatherDay>& days, int windowDays, double thresholdC, bool wraps,
                        GrowingSeason& out, std::string& error);

// Great-circle distance between two points, metres.
bool StationDistance (double latA, double lonA, double latB, double lonB, double& metres, std::string& error);

} // namespace evp::sunstudy

#endif
