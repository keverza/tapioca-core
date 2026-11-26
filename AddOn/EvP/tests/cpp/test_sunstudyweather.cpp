// EPW weather files through the C++ wrapper (#155 P1): identity, interval
// energy, refusals with their line, growing season and station distance.

#include "SunStudy/SunStudyWeather.hpp"
#include "SunStudy/ZipArchive.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

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

void Put16 (std::vector<uint8_t>& b, uint32_t v)
{
    b.push_back (uint8_t (v));
    b.push_back (uint8_t (v >> 8));
}

void Put32 (std::vector<uint8_t>& b, uint32_t v)
{
    Put16 (b, v & 0xffffu);
    Put16 (b, v >> 16);
}

// A zip of "stored" members, as a minimal writer would make it.
std::vector<uint8_t> StoredZip (const std::vector<std::pair<std::string, std::string>>& files)
{
    std::vector<uint8_t> zip, directory;
    for (const auto& [name, text] : files) {
        const uint32_t offset = uint32_t (zip.size ());
        const uint32_t crc = Crc32 (reinterpret_cast<const uint8_t*> (text.data ()), text.size ());
        Put32 (zip, 0x04034b50u);
        Put16 (zip, 20);
        Put16 (zip, 0);
        Put16 (zip, 0); // stored
        Put32 (zip, 0);
        Put32 (zip, crc);
        Put32 (zip, uint32_t (text.size ()));
        Put32 (zip, uint32_t (text.size ()));
        Put16 (zip, uint32_t (name.size ()));
        Put16 (zip, 0);
        zip.insert (zip.end (), name.begin (), name.end ());
        zip.insert (zip.end (), text.begin (), text.end ());
        Put32 (directory, 0x02014b50u);
        Put16 (directory, 20);
        Put16 (directory, 20);
        Put16 (directory, 0);
        Put16 (directory, 0);
        Put32 (directory, 0);
        Put32 (directory, crc);
        Put32 (directory, uint32_t (text.size ()));
        Put32 (directory, uint32_t (text.size ()));
        Put16 (directory, uint32_t (name.size ()));
        Put16 (directory, 0);
        Put16 (directory, 0);
        Put16 (directory, 0);
        Put16 (directory, 0);
        Put32 (directory, 0);
        Put32 (directory, offset);
        directory.insert (directory.end (), name.begin (), name.end ());
    }
    const uint32_t at = uint32_t (zip.size ());
    zip.insert (zip.end (), directory.begin (), directory.end ());
    Put32 (zip, 0x06054b50u);
    Put16 (zip, 0);
    Put16 (zip, 0);
    Put16 (zip, uint32_t (files.size ()));
    Put16 (zip, uint32_t (files.size ()));
    Put32 (zip, uint32_t (directory.size ()));
    Put32 (zip, at);
    Put16 (zip, 0);
    return zip;
}

} // namespace

// Raw deflate streams made by zlib (wbits -15): one fixed-Huffman block, and
// one dynamic block with long back references.
TEST (ZipArchive, InflatesFixedAndDynamicBlocks)
{
    EXPECT_EQ (Crc32 (reinterpret_cast<const uint8_t*> ("123456789"), 9), 0xCBF43926u);
    const uint8_t fixed[] = { 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0xd7, 0x51, 0xc8, 0x40, 0xa2,
                              0x14, 0x0a, 0x72, 0x12, 0xf3, 0x4a, 0x32, 0xf3, 0xd2, 0x01 };
    std::vector<uint8_t> out;
    std::string error;
    ASSERT_TRUE (Inflate (fixed, sizeof (fixed), 64, out, error)) << error;
    EXPECT_EQ (std::string (out.begin (), out.end ()), "hello, hello, hello planting");

    const uint8_t dynamic[] = { 0xed, 0xd0, 0x2b, 0x0e, 0x80, 0x30, 0x10, 0x84, 0x61, 0xcf, 0x29, 0xe6, 0x00, 0x08,
                                0xde, 0xf7, 0xa9, 0xd8, 0x86, 0x26, 0x5b, 0x9a, 0xd0, 0x4d, 0x80, 0xdb, 0xf3, 0x50,
                                0xd8, 0xf5, 0xa3, 0x46, 0x4c, 0x7e, 0xf3, 0x69, 0xda, 0x04, 0x1d, 0x4a, 0x84, 0xad,
                                0x82, 0x43, 0xc2, 0x33, 0x3b, 0x62, 0x52, 0x69, 0x91, 0x4b, 0x35, 0xbd, 0xbe, 0xa3,
                                0x86, 0x2c, 0x30, 0x39, 0xad, 0xd1, 0x37, 0xe8, 0xbd, 0xc1, 0xe0, 0x0d, 0x46, 0x6f,
                                0x30, 0x79, 0x83, 0xd9, 0x1b, 0x2c, 0xde, 0x80, 0xac, 0x64, 0x25, 0x2b, 0x59, 0xc9,
                                0x4a, 0x56, 0xb2, 0x92, 0x95, 0xac, 0x64, 0xfd, 0x07, 0x37 };
    std::string expected;
    for (int i = 0; i < 60; ++i)
        expected += "line " + std::to_string (i % 7) + " of the weather file, mostly the same text\n";
    ASSERT_TRUE (Inflate (dynamic, sizeof (dynamic), expected.size (), out, error)) << error;
    EXPECT_EQ (std::string (out.begin (), out.end ()), expected);

    // Damage and a size cap are refused, never read past.
    EXPECT_FALSE (Inflate (dynamic, 40, expected.size (), out, error));
    EXPECT_FALSE (Inflate (dynamic, sizeof (dynamic), 100, out, error)) << "beyond the stated size";
    EXPECT_NE (error.find ("beyond"), std::string::npos);
}

// A zipped .epw reads as the .epw itself; the other members are passed over.
TEST (ZipArchive, AZippedWeatherFileReads)
{
    const std::string epw = Epw (3, 1);
    const auto zip = StoredZip ({ { "README.txt", "not weather" }, { "LTU_Vilnius/Vilnius.EPW", epw } });
    std::vector<ZipMember> members;
    std::string error;
    ASSERT_TRUE (ListZipMembers (zip, members, error)) << error;
    ASSERT_EQ (members.size (), 2u);
    EXPECT_EQ (members[1].name, "LTU_Vilnius/Vilnius.EPW");
    std::vector<uint8_t> inner;
    ASSERT_TRUE (ExtractZipMember (zip, members[1], inner, error)) << error;
    EXPECT_EQ (std::string (inner.begin (), inner.end ()), epw);

    const auto path = std::filesystem::temp_directory_path () / "tapioca-weather-test.zip";
    std::ofstream (path, std::ios::binary)
        .write (reinterpret_cast<const char*> (zip.data ()), std::streamsize (zip.size ()));
    WeatherData data;
    ASSERT_TRUE (ReadWeatherFile (path, data, error)) << error;
    EXPECT_EQ (data.records.size (), 72u);

    auto damaged = zip;
    damaged[damaged.size () / 2] ^= 0xff; // inside the .epw's bytes: the checksum catches it
    std::ofstream (path, std::ios::binary)
        .write (reinterpret_cast<const char*> (damaged.data ()), std::streamsize (damaged.size ()));
    EXPECT_FALSE (ReadWeatherFile (path, data, error));
    EXPECT_NE (error.find ("tapioca-weather-test.zip"), std::string::npos) << error;

    const auto none = StoredZip ({ { "README.txt", "not weather" } });
    std::ofstream (path, std::ios::binary)
        .write (reinterpret_cast<const char*> (none.data ()), std::streamsize (none.size ()));
    EXPECT_FALSE (ReadWeatherFile (path, data, error));
    EXPECT_NE (error.find ("no .epw"), std::string::npos) << error;
    std::filesystem::remove (path);
}

// Opt-in: TAPIOCA_EPW_DIR names a folder of zipped reference files (as
// downloaded); each one's .epw reads in full.
TEST (ZipArchive, ReferenceZipsReadWhenProvided)
{
    const char* dir = std::getenv ("TAPIOCA_EPW_DIR");
    if (dir == nullptr || *dir == 0)
        GTEST_SKIP () << "set TAPIOCA_EPW_DIR to a folder of zipped .epw files";
    int read = 0;
    for (const auto& entry : std::filesystem::directory_iterator (std::filesystem::u8path (dir))) {
        if (!IsZipPath (entry.path ()))
            continue;
        WeatherData data;
        std::string error;
        EXPECT_TRUE (ReadWeatherFile (entry.path (), data, error)) << error;
        EXPECT_GE (data.records.size (), 8760u) << entry.path ().filename ().string ();
        ++read;
    }
    if (read == 0)
        GTEST_SKIP () << "no .zip in TAPIOCA_EPW_DIR";
}

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
