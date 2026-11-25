#include "SunStudy/SunStudyWeather.hpp"
#include "SunStudy/SunStudyRustDetail.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>

namespace evp::sunstudy {

using namespace rust::detail;

namespace {

static_assert (kWeatherFieldCount == TAPIOCA_WEATHER_FIELDS);

std::string Text (const char* field, size_t capacity)
{
    return std::string (field, strnlen (field, capacity));
}

std::string Hex (const uint8_t (&bytes)[32])
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve (64);
    for (uint8_t b : bytes) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

std::string Refusal (const TapiocaWeatherSummary& s)
{
    const std::string at = "line " + std::to_string (s.line);
    switch (s.refusal) {
        case 1:
            return "the file is empty or shorter than its 8 header lines";
        case 2:
            return at + ": expected header line is missing (LOCATION ... DATA PERIODS)";
        case 3:
            return "LOCATION has no readable latitude, longitude, time zone or elevation";
        case 4:
            return "DATA PERIODS is unreadable or names an invalid date";
        case 5:
            return "more than one data period is not supported";
        case 6:
            return "records per hour must be 1-60 and divide 60";
        case 7:
            return at + ": " + std::to_string (s.found) + " columns, at least " + std::to_string (s.expected) +
                   " expected";
        case 8:
            return at + ", column " + std::to_string (s.column) + ": not a number";
        case 9:
            return std::to_string (s.found) + " records, the data period needs " + std::to_string (s.expected) +
                   " (truncated or extra records)";
        case 10:
            return at + ": date or hour out of sequence";
        default:
            return "refused (" + std::to_string (s.refusal) + ")";
    }
}

std::string Utf8 (const std::filesystem::path& path)
{
    const auto text = path.u8string ();
    return std::string (text.begin (), text.end ());
}

} // namespace

bool ParseWeather (const std::vector<uint8_t>& bytes, WeatherData& out, std::string& error)
{
    out = WeatherData ();
    // A record is at least one line, so the line count bounds the records.
    size_t lines = 1;
    for (uint8_t b : bytes)
        lines += b == '\n';
    std::vector<TapiocaWeatherRecord> records (lines);
    std::vector<TapiocaWeatherDay> days (366);
    TapiocaWeatherMonth months[12] {};
    auto summary = std::make_unique<TapiocaWeatherSummary> ();
    const TapiocaWeatherReadRequest request { { bytes.data (), bytes.size () } };
    const TapiocaWeatherReadOutput output { records.data (), records.size (), days.data (),
                                            days.size (),    months,          summary.get () };
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_weather_read (&request, &output, m, n); }, error))
        return false;
    const TapiocaWeatherSummary& s = *summary;
    if (s.refusal != 0) {
        error = "EPW refused: " + Refusal (s);
        return false;
    }
    WeatherData result;
    auto& id = result.identity;
    id.city = Text (s.city, sizeof (s.city));
    id.region = Text (s.region, sizeof (s.region));
    id.country = Text (s.country, sizeof (s.country));
    id.source = Text (s.source, sizeof (s.source));
    id.wmo = Text (s.wmo, sizeof (s.wmo));
    id.comments = { Text (s.comments1, sizeof (s.comments1)), Text (s.comments2, sizeof (s.comments2)) };
    id.latitude = s.latitude;
    id.longitude = s.longitude;
    id.timeZoneHours = s.timeZoneHours;
    id.elevationM = s.elevationM;
    id.leapYear = s.leapYear != 0;
    id.recordsPerHour = s.recordsPerHour;
    id.startMonth = s.periodStart[0];
    id.startDay = s.periodStart[1];
    id.endMonth = s.periodEnd[0];
    id.endDay = s.periodEnd[1];
    id.sha256 = Hex (s.sha256);
    id.radiationInDark = s.radiationInDark;
    std::copy (std::begin (s.missing), std::end (s.missing), id.missing.begin ());
    std::copy (std::begin (s.outOfRange), std::end (s.outOfRange), id.outOfRange.begin ());
    result.records.resize (s.records);
    for (size_t i = 0; i < s.records; ++i) {
        const auto& r = records[i];
        auto& w = result.records[i];
        w.year = r.year;
        w.month = r.month;
        w.day = r.day;
        w.hour = r.hour;
        w.minute = r.minute;
        std::copy (std::begin (r.values), std::end (r.values), w.values.begin ());
    }
    result.days.resize (s.days);
    for (size_t i = 0; i < s.days; ++i)
        result.days[i] = { days[i].month, days[i].day, days[i].meanDryBulbC, days[i].globalHorizontalWhM2,
                           days[i].globalMissing };
    for (size_t m = 0; m < 12; ++m)
        result.months[m] = { months[m].records,
                             months[m].globalMissing,
                             months[m].meanDryBulbC,
                             months[m].minDryBulbC,
                             months[m].maxDryBulbC,
                             months[m].globalHorizontalKwhM2,
                             months[m].diffuseHorizontalKwhM2,
                             months[m].precipitationMm };
    out = std::move (result);
    return true;
}

bool ReadWeatherFile (const std::filesystem::path& path, WeatherData& out, std::string& error)
{
    out = WeatherData ();
    std::ifstream file (path, std::ios::binary);
    if (!file) {
        error = "cannot open " + Utf8 (path);
        return false;
    }
    const std::vector<uint8_t> bytes ((std::istreambuf_iterator<char> (file)), std::istreambuf_iterator<char> ());
    if (!ParseWeather (bytes, out, error)) {
        error = Utf8 (path.filename ()) + ": " + error;
        return false;
    }
    return true;
}

bool FindGrowingSeason (const std::vector<WeatherDay>& days, int windowDays, double thresholdC, bool wraps,
                        GrowingSeason& out, std::string& error)
{
    out = GrowingSeason ();
    std::vector<double> means;
    means.reserve (days.size ());
    for (const auto& d : days)
        means.push_back (d.meanDryBulbC);
    const TapiocaWeatherSeasonRequest request { { means.data (), means.size () },
                                                size_t (windowDays < 0 ? 0 : windowDays),
                                                thresholdC,
                                                uint8_t (wraps ? 1 : 0),
                                                {} };
    TapiocaWeatherSeasonResult result {};
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_weather_season (&request, &result, m, n); }, error))
        return false;
    out = { result.found != 0, result.first, result.last };
    return true;
}

bool StationDistance (double latA, double lonA, double latB, double lonB, double& metres, std::string& error)
{
    const TapiocaStationDistanceRequest request { { latA, lonA }, { latB, lonB } };
    return Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_station_distance (&request, &metres, m, n); },
                   error);
}

} // namespace evp::sunstudy
