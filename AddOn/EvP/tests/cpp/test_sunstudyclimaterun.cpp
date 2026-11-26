// One sample set's climate run (#155 F3, #278 Stage 5) and the weather time
// base it is fed from (#155 P2), offline: a long overhang over a wall, an
// open sample in front of it, and one sample a planting layer cannot answer.

#include "SunStudy/CpuTraversal.hpp"
#include "SunStudy/SunStudyClimateRun.hpp"
#include "SunStudy/SunStudyWeather.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <memory>
#include <vector>

using namespace evp::sunstudy;
using evp::sunstudy::rust::ClimateInterval;
using evp::sunstudy::rust::ClimateSetup;
using evp::sunstudy::rust::ClimateTotals;
using evp::sunstudy::rust::CrossCheckSun;
using evp::sunstudy::rust::DiffuseSky;
using evp::sunstudy::rust::SkyPath;
using evp::sunstudy::rust::SunCrossCheck;
using evp::sunstudy::rust::WeatherInstant;
using evp::sunstudy::rust::WeatherInstants;
using evp::sunstudy::rust::WeatherPlacement;
using evp::sunstudy::rust::WeatherStamp;
using geomsrv::Mesh;
using geomsrv::QueryEngine;
using geomsrv::Snapshot;

namespace {

// A 400 m overhang 1 m above the origin, 2 m deep in front of a wall facing -y.
std::unique_ptr<CpuTraversal> OverhangScene ()
{
    auto snap = std::make_shared<Snapshot> ();
    snap->id = 21;
    Mesh mesh;
    mesh.guid = "overhang";
    mesh.vertices = { -200, -2, 1, 200, -2, 1, 200, 0, 1, -200, 0, 1 };
    mesh.triangles = { 0, 1, 2, 0, 2, 3 };
    snap->meshes.push_back (std::move (mesh));
    return std::make_unique<CpuTraversal> (std::make_shared<QueryEngine> (snap));
}

// Vilnius, June 21 to 23, hourly records on the station's own clock.
WeatherPlacement Vilnius ()
{
    WeatherPlacement p;
    p.year = 2026;
    p.stationOffsetHours = 2.0;
    p.projectOffsetHours = 2.0;
    p.latitude = 54.687;
    p.longitude = 25.28;
    return p;
}

std::vector<WeatherStamp> ThreeDays ()
{
    std::vector<WeatherStamp> stamps;
    for (int day = 21; day <= 23; ++day)
        for (int hour = 1; hour <= 24; ++hour)
            stamps.push_back ({ 6, day, hour, 60 });
    return stamps;
}

// Intervals with the independent sun (a preview, as the run is told) and a
// clear-sky shape: DNI and DHI follow the sun's height.
std::vector<ClimateInterval> Intervals (const std::vector<WeatherInstant>& instants)
{
    std::vector<ClimateInterval> out;
    for (const WeatherInstant& w : instants) {
        ClimateInterval i;
        i.sun = w.independentSun;
        i.hours = w.hours;
        const double up = std::max (0.0, w.independentSun[2]);
        i.dni = 800.0 * up;
        i.dhi = up > 0.0 ? 60.0 + 80.0 * up : 0.0;
        i.albedo = 0.2;
        i.dewPointC = 10.0;
        i.dayOfYear = w.dayOfYear;
        i.month = 5;
        out.push_back (i);
    }
    return out;
}

ClimateRunRequest Request (const std::vector<ClimateInterval>& intervals)
{
    ClimateRunRequest r;
    // 0: the wall under the overhang; 1: open ground 500 m out, where the
    // overhang is below every cell; 2: unavailable.
    r.positions = { 0, 0, 0, 0, -500, 0, 5, -10, 0 };
    r.normals = { 0, -1, 0, 0, 0, 1, 0, 0, 1 };
    r.available = { 1, 1, 0 };
    r.intervals = intervals;
    r.sunSource = SunSource::Independent;
    return r;
}

} // namespace

TEST (SunStudyClimateRun, WeatherRecordsLandOnTheProjectClock)
{
    std::vector<WeatherInstant> instants;
    std::string error;
    ASSERT_TRUE (WeatherInstants (ThreeDays (), Vilnius (), instants, error)) << error;
    ASSERT_EQ (instants.size (), 72u);
    // Hour 13 of June 21: 12:30 on the project's clock, day 172, sun up.
    const WeatherInstant& noon = instants[12];
    EXPECT_TRUE (noon.valid);
    EXPECT_EQ (noon.projectDay, 21);
    EXPECT_NEAR (noon.projectSeconds, 45000.0, 1e-3);
    EXPECT_EQ (noon.dayOfYear, 172u);
    EXPECT_GT (noon.independentSun[2], 0.8);
    std::vector<WeatherInstant> bad;
    ASSERT_TRUE (WeatherInstants ({ { 13, 1, 1, 0 } }, Vilnius (), bad, error)) << error;
    EXPECT_FALSE (bad[0].valid);
}

TEST (SunStudyClimateRun, TheCrossCheckRefusesASunAnHourOff)
{
    std::vector<WeatherInstant> instants;
    std::string error;
    ASSERT_TRUE (WeatherInstants (ThreeDays (), Vilnius (), instants, error)) << error;
    std::vector<double> same, shifted;
    for (size_t k = 0; k + 1 < instants.size (); ++k) {
        same.insert (same.end (), instants[k].independentSun.begin (), instants[k].independentSun.end ());
        shifted.insert (shifted.end (), instants[k + 1].independentSun.begin (), instants[k + 1].independentSun.end ());
    }
    SunCrossCheck check;
    ASSERT_TRUE (CrossCheckSun (same, same, check, error)) << error;
    EXPECT_TRUE (check.accepted);
    EXPECT_GT (check.compared, 40u);
    ASSERT_TRUE (CrossCheckSun (shifted, same, check, error)) << error;
    EXPECT_FALSE (check.accepted) << "a daylight-saving hour must not pass";
}

TEST (SunStudyClimateRun, AnOpenSampleGetsTheHorizontalTotalAndAnUnavailableOneGetsNothing)
{
    std::vector<WeatherInstant> instants;
    std::string error;
    ASSERT_TRUE (WeatherInstants (ThreeDays (), Vilnius (), instants, error)) << error;
    const auto scene = OverhangScene ();
    size_t calls = 0, last = 0;
    ClimateRunResult result;
    ASSERT_TRUE (RunClimate (
        *scene, Request (Intervals (instants)), {},
        [&] (size_t done, size_t) {
            ++calls;
            last = done;
        },
        result, error))
        << error;
    EXPECT_EQ (calls, 73u);
    EXPECT_EQ (last, 72u);
    EXPECT_EQ (result.sunSource, SunSource::Independent);
    EXPECT_EQ (result.traced, 2u);
    EXPECT_EQ (result.excluded, 1u);
    EXPECT_GT (result.beamPasses, 40u);

    // Open ground: everything the model's horizontal total holds.
    const double open = result.report.openHorizontal[5];
    const double ground = result.direct[1][5] + result.diffuse[1][5] + result.reflected[1][5];
    EXPECT_NEAR (ground, open, 1e-9 * open);
    EXPECT_NEAR (result.skyView[1], 1.0, 1e-12);
    // The wall under the overhang: no high summer beam, a fraction of the sky.
    EXPECT_LT (result.direct[0][5], 0.2 * result.direct[1][5]);
    EXPECT_NEAR (result.skyView[0], 0.2254, 0.02 * 0.2254);
    EXPECT_GT (result.reflected[0][5], 0.0);
    // Unavailable is not dark: NaN everywhere, never zero.
    EXPECT_TRUE (std::isnan (result.direct[2][5]) && std::isnan (result.dli[2][5]) && std::isnan (result.skyView[2]));
    // DLI: the open sample's June energy over the three days' hours.
    const double expected = open * 3600.0 * 0.45 * 4.57e-6 * 24.0 / result.report.validHours[5];
    EXPECT_NEAR (result.dli[1][5], expected, 1e-9 * expected);
    EXPECT_TRUE (std::isnan (result.dli[1][0])) << "no January weather: no January DLI";
}

TEST (SunStudyClimateRun, ACancelledRunLeavesThePreviousResult)
{
    std::vector<WeatherInstant> instants;
    std::string error;
    ASSERT_TRUE (WeatherInstants (ThreeDays (), Vilnius (), instants, error)) << error;
    const auto scene = OverhangScene ();
    ClimateRunResult result;
    result.traced = 99;
    size_t polls = 0;
    EXPECT_FALSE (RunClimate (*scene, Request (Intervals (instants)), [&] { return ++polls > 10; }, {}, result, error));
    EXPECT_EQ (error, "Cancelled");
    EXPECT_EQ (result.traced, 99u);
    ClimateRunRequest bad = Request ({});
    bad.available = { 1 };
    EXPECT_FALSE (RunClimate (*scene, bad, {}, {}, result, error));
}

TEST (SunStudyClimateRun, AreaWeightedMeansSkipExcludedSamples)
{
    rust::Monthly a {}, b {}, gone {};
    a[3] = 10.0;
    b[3] = 20.0;
    gone[3] = std::nan ("");
    EXPECT_DOUBLE_EQ (AreaWeightedMean ({ a, b, gone }, { 1.0, 3.0, 5.0 }, 3), 17.5);
    EXPECT_TRUE (std::isnan (AreaWeightedMean ({ gone }, { 1.0 }, 3)));
    EXPECT_TRUE (std::isnan (AreaWeightedMean ({ a }, { 1.0 }, 12)));
}

// The file check (#155 P3): an open horizontal sample's model total is
// Σ(DNI·sin α + DHI); a file whose GHI reads apart from it is reported, never
// absorbed. A consistent file closes; one with GHI 10 % high is flagged.
TEST (SunStudyClimateRun, TheHorizontalClosureReportsTheFilesOwnGhi)
{
    std::vector<WeatherInstant> instants;
    std::string error;
    ASSERT_TRUE (WeatherInstants (ThreeDays (), Vilnius (), instants, error)) << error;
    for (const double factor : { 1.0, 1.1 }) {
        std::vector<ClimateInterval> intervals = Intervals (instants);
        for (ClimateInterval& i : intervals)
            i.ghi = factor * (i.dni * std::max (0.0, i.sun[2]) + i.dhi);
        auto request = Request (intervals);
        ClimateRunResult result;
        auto scene = OverhangScene ();
        ASSERT_TRUE (RunClimate (*scene, request, {}, {}, result, error)) << error;
        const auto& r = result.report;
        EXPECT_NEAR (r.closureAnnual, 1.0 / factor - 1.0, 1e-9) << factor;
        EXPECT_EQ (r.closureWithin, factor == 1.0);
        EXPECT_EQ (r.closureWorstMonth, 5);
        EXPECT_DOUBLE_EQ (r.closureTolerance, 0.05);
        // The file's GHI never moves a total: the open sample is the model's.
        EXPECT_NEAR (result.direct[1][5] + result.diffuse[1][5], r.openHorizontal[5], 1e-6 * r.openHorizontal[5]);
    }
}

// Opt-in: TAPIOCA_EPW_DIR names a folder of the reference weather files
// (tapioca-ref\EPW, zipped or not; never committed). Each one's DNI and DHI,
// with the independent sun at the interval midpoint, rebuild its own GHI
// within the file-consistency tolerance over the year. The monthly shares are
// printed: the evidence for that tolerance.
TEST (SunStudyClimateRun, ReferenceWeatherFilesCloseOnTheirOwnGhi)
{
    const char* dir = std::getenv ("TAPIOCA_EPW_DIR");
    if (dir == nullptr || *dir == 0)
        GTEST_SKIP () << "set TAPIOCA_EPW_DIR to a folder of .epw files";
    int checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator (std::filesystem::u8path (dir))) {
        if (entry.path ().extension () != ".epw" && !IsZipPath (entry.path ()))
            continue;
        WeatherData w;
        std::string error;
        ASSERT_TRUE (ReadWeatherFile (entry.path (), w, error)) << error;
        WeatherPlacement place;
        place.year = 2026;
        place.recordsPerHour = int (w.identity.recordsPerHour);
        place.stationOffsetHours = w.identity.timeZoneHours;
        place.projectOffsetHours = w.identity.timeZoneHours;
        place.latitude = w.identity.latitude;
        place.longitude = w.identity.longitude;
        std::vector<WeatherStamp> stamps;
        std::vector<const WeatherRecord*> kept;
        for (const WeatherRecord& r : w.records) {
            if (r.month == 2 && r.day == 29)
                continue;
            stamps.push_back ({ r.month, r.day, r.hour, r.minute });
            kept.push_back (&r);
        }
        std::vector<WeatherInstant> instants;
        ASSERT_TRUE (WeatherInstants (stamps, place, instants, error)) << error;
        ClimateSetup setup;
        setup.sky = DiffuseSky::Isotropic;
        setup.normals = { 0, 0, 1 };
        setup.open.assign (145, 255);
        auto climate = rust::RustClimate::Create (setup, error);
        ASSERT_NE (climate, nullptr) << error;
        for (size_t k = 0; k < kept.size (); ++k) {
            if (!instants[k].valid)
                continue;
            ClimateInterval i;
            i.sun = instants[k].independentSun;
            i.hours = instants[k].hours;
            i.dni = kept[k]->Get (WeatherField::DirectNormal);
            i.dhi = kept[k]->Get (WeatherField::DiffuseHorizontal);
            i.ghi = kept[k]->Get (WeatherField::GlobalHorizontal);
            i.albedo = 0.2;
            i.dewPointC = std::nan ("");
            i.dayOfYear = instants[k].dayOfYear;
            i.month = int (kept[k]->month) - 1;
            const std::vector<uint8_t> beam =
                i.NeedsBeam () ? std::vector<uint8_t> { uint8_t (SkyPath::Open) } : std::vector<uint8_t> {};
            ASSERT_TRUE (climate->Add (i, beam, error)) << error;
        }
        ClimateTotals totals;
        ASSERT_TRUE (climate->Read (totals, error)) << error;
        const auto& r = totals.report;
        std::printf ("closure %s: year %+.2f %%, worst month %d,", entry.path ().filename ().string ().c_str (),
                     100.0 * r.closureAnnual, r.closureWorstMonth + 1);
        for (double m : r.closureMonthly)
            std::printf (" %+.1f", 100.0 * m);
        std::printf ("\n");
        EXPECT_TRUE (r.closureWithin) << entry.path ().filename ().string () << ": " << r.closureAnnual;
        ++checked;
    }
    if (checked == 0)
        GTEST_SKIP () << "no weather file in TAPIOCA_EPW_DIR";
}

// Opt-in, #155 P3 "a shaded vertical surface against an independent
// reference": TAPIOCA_RADIANCE_OVERHANG_DIR names a folder of pairs
// `<name>.wea` (Radiance's epw2wea) and `<name>.wall`, the hourly SKY
// irradiance on this file's wall sample under OverhangScene from Radiance:
//
//   rfluxmtx -I+ -ab 1 -ad 65536 -lw 1e-6 -c 1 < sensor.pts - sky_m1.rad overhang.rad > wall_m1.dc
//   (sensor 0 -0.02 0 facing 0 -1 0; the ground bin zeroed: sky only)
//   gendaymtx -m 1 -s -O1 <name>.wea | dctimestep wall_m1_sky.dc /dev/stdin > <name>.wall
//
// Ours: the same sample through the sky masks (4 x 4 cells a patch) and the
// Perez sky, with the independent sun. Radiance computes its own sun and its
// own Perez sky, so the sky models' difference (within 2.2 % of the year's
// DHI, sky::radiance_tests) is part of what is measured. Bound: 10 % over the
// year (P3); the months are printed. Measured 2026-11-26: Perugia IGDG -2.48 %,
// Kaunas IWEC -2.15 %, Vilnius TMYx -2.18 %, every month within -3.6 %; the
// same offset as gendaymtx's sky alone (its patches integrate to about 1.022 x
// DHI), so the shading adds almost nothing. Radiance's view factor for this
// sensor is 0.2256, the analytic 0.2254.
namespace {

struct Wea {
    double latitude = std::nan (""), longitudeEast = std::nan (""), offsetHours = std::nan ("");
    struct Step {
        int month = 0, day = 0;
        double hour = 0.0, dni = 0.0, dhi = 0.0;
    };
    std::vector<Step> steps;
};

bool ReadWea (const std::filesystem::path& path, Wea& wea)
{
    std::ifstream in (path);
    std::string line;
    while (std::getline (in, line)) {
        std::istringstream fields (line);
        std::string key;
        fields >> key;
        if (key == "latitude")
            fields >> wea.latitude;
        else if (key == "longitude") {
            fields >> wea.longitudeEast; // the file writes west positive
            wea.longitudeEast = -wea.longitudeEast;
        }
        else if (key == "time_zone") {
            fields >> wea.offsetHours; // the meridian, west positive
            wea.offsetHours = -wea.offsetHours / 15.0;
        }
        else if (!key.empty () && std::isdigit (static_cast<unsigned char> (key[0]))) {
            Wea::Step s;
            s.month = std::stoi (key);
            fields >> s.day >> s.hour >> s.dni >> s.dhi;
            wea.steps.push_back (s);
        }
    }
    return std::isfinite (wea.latitude) && std::isfinite (wea.longitudeEast) && std::isfinite (wea.offsetHours);
}

// dctimestep's one row: after the header's blank line, an RGB triple a step.
std::vector<double> ReadWall (const std::filesystem::path& path)
{
    std::ifstream in (path);
    std::string line;
    while (std::getline (in, line) && !line.empty () && line != "\r") {
    }
    std::vector<double> rgb, out;
    double v = 0.0;
    while (in >> v)
        rgb.push_back (v);
    for (size_t i = 0; i + 2 < rgb.size (); i += 3)
        out.push_back ((rgb[i] + rgb[i + 1] + rgb[i + 2]) / 3.0);
    return out;
}

} // namespace

TEST (SunStudyClimateRun, AShadedWallMatchesRadianceWhenFixturesAreProvided)
{
    const char* dir = std::getenv ("TAPIOCA_RADIANCE_OVERHANG_DIR");
    if (dir == nullptr || *dir == 0)
        GTEST_SKIP () << "set TAPIOCA_RADIANCE_OVERHANG_DIR to a folder of .wea and .wall pairs";
    int checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator (std::filesystem::u8path (dir))) {
        if (entry.path ().extension () != ".wea")
            continue;
        auto wallPath = entry.path ();
        wallPath.replace_extension (".wall");
        Wea wea;
        ASSERT_TRUE (ReadWea (entry.path (), wea)) << entry.path ();
        const std::vector<double> wall = ReadWall (wallPath);
        ASSERT_EQ (wall.size (), wea.steps.size ()) << wallPath;

        WeatherPlacement place;
        place.year = 2026;
        place.stationOffsetHours = wea.offsetHours;
        place.projectOffsetHours = wea.offsetHours;
        place.latitude = wea.latitude;
        place.longitude = wea.longitudeEast;
        std::vector<WeatherStamp> stamps;
        for (const auto& s : wea.steps) // the .wea hour is the interval's midpoint
            stamps.push_back ({ s.month, s.day, int (std::floor (s.hour)) + 1, 0 });
        std::vector<WeatherInstant> instants;
        std::string error;
        ASSERT_TRUE (WeatherInstants (stamps, place, instants, error)) << error;

        ClimateRunRequest request;
        request.positions = { 0, 0, 0 };
        request.normals = { 0, -1, 0 };
        request.sunSource = SunSource::Independent;
        std::vector<size_t> kept; // the steps run, by index into the .wall
        for (size_t k = 0; k < wea.steps.size (); ++k) {
            if (!instants[k].valid)
                continue;
            ClimateInterval i;
            i.sun = instants[k].independentSun;
            i.hours = instants[k].hours;
            i.dni = wea.steps[k].dni;
            i.dhi = wea.steps[k].dhi;
            i.albedo = 0.2;
            i.dewPointC = std::nan ("");
            i.dayOfYear = instants[k].dayOfYear;
            i.month = wea.steps[k].month - 1;
            request.intervals.push_back (i);
            kept.push_back (k);
        }
        const auto scene = OverhangScene ();
        ClimateRunResult result;
        ASSERT_TRUE (RunClimate (*scene, request, {}, {}, result, error)) << error;

        std::array<double, 12> theirs {};
        for (size_t k : kept)
            theirs[size_t (wea.steps[k].month - 1)] += wall[k]; // W/m2 over one hour: Wh/m2
        double oursYear = 0.0, theirsYear = 0.0;
        std::string months;
        for (size_t m = 0; m < 12; ++m) {
            const double ours = result.diffuse[0][m];
            oursYear += ours;
            theirsYear += theirs[m];
            char text[32];
            std::snprintf (text, sizeof text, " %+.1f", theirs[m] > 0.0 ? 100.0 * (ours / theirs[m] - 1.0) : 0.0);
            months += text;
        }
        const double share = oursYear / theirsYear - 1.0;
        std::printf ("shaded wall %s: sky %.1f kWh/m2 (Radiance %.1f), %+.2f %% over the year; months%s\n",
                     entry.path ().filename ().string ().c_str (), oursYear / 1000.0, theirsYear / 1000.0,
                     100.0 * share, months.c_str ());
        EXPECT_LT (std::fabs (share), 0.10) << entry.path ().filename ().string ();
        ++checked;
    }
    if (checked == 0)
        GTEST_SKIP () << "no .wea in TAPIOCA_RADIANCE_OVERHANG_DIR";
}
