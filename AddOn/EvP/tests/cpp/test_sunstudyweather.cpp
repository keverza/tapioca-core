// EPW weather files through the C++ wrapper (#155 P1): identity, interval
// energy, refusals with their line, growing season and station distance.

#include "SunStudy/SunStudyWeather.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <string>

using namespace evp::sunstudy;

namespace {

// `days` consecutive days from 1 May, `perHour` records an hour; 400 W/m²
// global from 09:00 to 15:00 and a dry bulb of 10 °C plus the day index.
std::string Epw (int days, int perHour, int badLine = -1)
{
    std::string text = "LOCATION,Vilnius test,VL,LTU,Authored,267300,54.6364,25.2908,2.0,196.9\n"
                       "DESIGN CONDITIONS,0\nTYPICAL/EXTREME PERIODS,0\nGROUND TEMPERATURES,0\n"
                       "HOLIDAYS/DAYLIGHT SAVINGS,No,0,0,0\nCOMMENTS 1,\"Authored, for tests\"\nCOMMENTS 2,\n"
                       "DATA PERIODS,1," +
                       std::to_string (perHour) + ",Data,Sunday, 5/ 1, 5/" + std::to_string (days) + "\n";
    int line = 8;
    for (int d = 1; d <= days; ++d)
        for (int h = 1; h <= 24; ++h)
            for (int s = 1; s <= perHour; ++s) {
                ++line;
                const bool lit = h >= 10 && h <= 15;
                const std::string ghi = lit ? "400" : "0";
                const std::string dry = line == badLine ? "warm" : std::to_string (10 + d - 1) + ".0";
                text += "2001,5," + std::to_string (d) + "," + std::to_string (h) + "," +
                        std::to_string (perHour == 1 ? 0 : s * 60 / perHour) + ",?9," + dry + ",5.0,70,99000," +
                        (lit ? "1000" : "0") + ",1300,300," + ghi +
                        ",200,9999,0,0,0,0,180,3.0,5,4,20.0,77777,9,"
                        "999999999,10,0.0850,0,88,0.200,0.5,1.0\r\n";
            }
    return text;
}

std::vector<uint8_t> Bytes (const std::string& s)
{
    return { s.begin (), s.end () };
}

} // namespace

TEST (SunStudyWeather, AParsedFileCarriesItsIdentity)
{
    WeatherData data;
    std::string error;
    ASSERT_TRUE (ParseWeather (Bytes (Epw (3, 1)), data, error)) << error;
    const auto& id = data.identity;
    EXPECT_EQ (id.city, "Vilnius test");
    EXPECT_EQ (id.wmo, "267300");
    EXPECT_EQ (id.comments[0], "Authored, for tests");
    EXPECT_DOUBLE_EQ (id.timeZoneHours, 2.0);
    EXPECT_EQ (id.sha256.size (), 64u);
    EXPECT_EQ (id.startMonth, 5);
    EXPECT_EQ (id.endDay, 3);
    ASSERT_EQ (data.records.size (), 72u);
    EXPECT_EQ (data.records[12].hour, 13);
    EXPECT_DOUBLE_EQ (data.records[12].Get (WeatherField::GlobalHorizontal), 400.0);
    EXPECT_TRUE (std::isnan (data.records[12].Get (WeatherField::DiffuseHorizontal))) << "9999 is missing";
    EXPECT_EQ (id.missing[size_t (WeatherField::DiffuseHorizontal)], 72u);
    ASSERT_EQ (data.days.size (), 3u);
    EXPECT_DOUBLE_EQ (data.days[2].meanDryBulbC, 12.0);
    EXPECT_EQ (data.months[4].records, 72u);
    EXPECT_EQ (data.months[0].records, 0u);
}

TEST (SunStudyWeather, AQuarterHourFileKeepsEnergyPerHour)
{
    WeatherData data;
    std::string error;
    ASSERT_TRUE (ParseWeather (Bytes (Epw (1, 4)), data, error)) << error;
    EXPECT_EQ (data.records.size (), 96u);
    EXPECT_DOUBLE_EQ (data.IntervalHours (), 0.25);
    // Six lit hours at a mean 400 W/m²: 2.4 kWh/m², not four times that.
    EXPECT_NEAR (data.days[0].globalHorizontalWhM2, 2400.0, 1e-9);
    EXPECT_NEAR (data.months[4].globalHorizontalKwhM2, 2.4, 1e-12);
}

TEST (SunStudyWeather, ARefusalNamesItsLine)
{
    WeatherData data;
    std::string error;
    EXPECT_FALSE (ParseWeather (Bytes (Epw (2, 1, 20)), data, error));
    EXPECT_NE (error.find ("line 20, column 6"), std::string::npos) << error;
    EXPECT_TRUE (data.records.empty ());
    std::string cut = Epw (2, 1);
    cut.resize (cut.size () / 2);
    cut.resize (cut.rfind ('\n') + 1);
    EXPECT_FALSE (ParseWeather (Bytes (cut), data, error));
    EXPECT_NE (error.find ("the data period needs 48"), std::string::npos) << error;
    EXPECT_FALSE (ReadWeatherFile ("Z:/no/such/file.epw", data, error));
    EXPECT_NE (error.find ("cannot open"), std::string::npos);
}

TEST (SunStudyWeather, SeasonAndDistance)
{
    std::vector<WeatherDay> days (365);
    for (size_t d = 0; d < days.size (); ++d)
        days[d].meanDryBulbC = d >= 110 && d <= 290 ? 12.0 : (d == 200 ? NAN : -1.0);
    GrowingSeason season;
    std::string error;
    ASSERT_TRUE (FindGrowingSeason (days, 7, 5.0, true, season, error)) << error;
    EXPECT_TRUE (season.found);
    EXPECT_EQ (season.first, 110u);
    EXPECT_EQ (season.last, 290u);
    EXPECT_FALSE (FindGrowingSeason (days, 4, 5.0, true, season, error)) << "an even window has no centre";
    double metres = 0;
    ASSERT_TRUE (StationDistance (54.6364, 25.2908, 54.6872, 25.2797, metres, error)) << error;
    EXPECT_NEAR (metres, 5700.0, 300.0) << "Vilnius airport to the old town";
}

// Opt-in: TAPIOCA_EPW_FILE names one real file (never committed).
TEST (SunStudyWeather, AReferenceFileReadsWhenProvided)
{
    const char* path = std::getenv ("TAPIOCA_EPW_FILE");
    if (!path)
        GTEST_SKIP () << "set TAPIOCA_EPW_FILE to an .epw file";
    WeatherData data;
    std::string error;
    ASSERT_TRUE (ReadWeatherFile (path, data, error)) << error;
    EXPECT_GE (data.records.size (), 8760u);
}
