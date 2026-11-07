#include "APIEnvir.h"
#include "ACAPinc.h"

// NativeCommands/SunStudySunPath -- the sun's path over the project (#152), an
// optional preview: Archicad's own sun positions, from the project's location
// and north, for the solstices, the equinox and one chosen date (one point
// every ten minutes), and each whole hour across the year (the analemmas).
//
// Captured on the main thread -- CalcSunOnPlace is a host call -- by one
// worker, latest request wins; the HUD only reads the finished path.
// ⚠️ InvokeUnlessStopped, NOT Invoke: Shutdown runs on the main thread and
// joins this worker, which would otherwise wait for that same thread.

#include "NativeCommands/SunStudyHudRunner.hpp"
#include "Python/MainThreadGate.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <optional>
#include <thread>

namespace geomsrv::sunstudyhud {

namespace {

constexpr int kTimeoutMs = 30000;
constexpr double kPi = 3.14159265358979323846;

struct Wanted {
    int year = 0, month = 0, day = 0;
    bool operator== (const Wanted& other) const
    {
        return year == other.year && month == other.month && day == other.day;
    }
};

std::mutex gMutex;
std::shared_ptr<const SunPath> gPath;
std::optional<Wanted> gWant;
std::optional<Wanted> gAsked; // the last request taken or done
std::thread gWorker;
bool gRunning = false;
std::atomic<bool> gStop { false };
// The year's lattice, kept across date changes (the worker's alone).
int gGridYear = 0;
std::shared_ptr<const std::vector<SunPathGridPoint>> gGrid;

bool DaysValid (int year, int month, int day)
{
    static constexpr int kDays[] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return year > 0 && month >= 1 && month <= 12 && day >= 1 && day <= kDays[month - 1];
}

// One position: a unit direction toward the sun in model space (the day
// study's own conversion: sunAngXY is CCW from +X, already north-corrected).
bool SunAt (const API_PlaceInfo& place, int year, int month, int day, int minute, SunPathPoint& point)
{
    API_PlaceInfo moment = place;
    moment.year = (unsigned short) year;
    moment.month = (unsigned short) month;
    moment.day = (unsigned short) day;
    moment.hour = (unsigned short) (minute / 60);
    moment.minute = (unsigned short) (minute % 60);
    moment.second = 0;
    if (ACAPI_GeoLocation_CalcSunOnPlace (&moment) != NoError)
        return false;
    const double horizontal = std::cos (moment.sunAngZ);
    point.direction[0] = float (horizontal * std::cos (moment.sunAngXY));
    point.direction[1] = float (horizontal * std::sin (moment.sunAngXY));
    point.direction[2] = float (std::sin (moment.sunAngZ));
    point.minute = minute;
    return true;
}

// Above the horizon only: a run that dips below is split there.
void Keep (std::vector<SunPathArc>& arcs, SunPathArc& run, const SunPathPoint& point, bool valid)
{
    if (valid && point.direction[2] >= 0.0f) {
        run.points.push_back (point);
        return;
    }
    if (run.points.size () > 1)
        arcs.push_back (run);
    run.points.clear ();
}

void Capture (const Wanted& wanted, SunPath& path)
{
    API_PlaceInfo place {};
    if (ACAPI_GeoLocation_GetPlaceSets (&place) != NoError) {
        path.error = "Archicad did not give the project's location";
        return;
    }
    path.northRadians = place.north;
    path.latitude = place.latitude;
    path.longitude = place.longitude;
    // The day arcs: winter solstice, equinox, summer solstice, the chosen date.
    struct Date {
        int month, day, kind;
    };
    const Date dates[] = { { 12, 21, 0 }, { 3, 20, 1 }, { 6, 21, 2 }, { wanted.month, wanted.day, 3 } };
    for (const Date& date : dates) {
        SunPathArc run;
        run.kind = date.kind;
        run.month = date.month;
        run.day = date.day;
        for (int minute = 0; minute <= 24 * 60; minute += 10) {
            SunPathPoint point;
            const bool valid =
                SunAt (place, wanted.year, date.month, date.day, (std::min) (minute, 24 * 60 - 1), point);
            Keep (path.arcs, run, point, valid);
        }
        Keep (path.arcs, run, {}, false);
    }
    // The analemmas: each whole hour, every seventh day of the year.
    static constexpr int kDays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    for (int hour = 0; hour < 24; ++hour) {
        SunPathArc run;
        run.kind = 4;
        run.hour = hour;
        for (int ordinal = 1; ordinal <= 365; ordinal += 7) {
            int month = 1, day = ordinal;
            while (day > kDays[month - 1]) {
                day -= kDays[month - 1];
                ++month;
            }
            SunPathPoint point;
            const bool valid = SunAt (place, wanted.year, month, day, hour * 60, point);
            Keep (path.arcs, run, point, valid);
        }
        Keep (path.arcs, run, {}, false);
    }
    path.ready = true;
}

// One month of the lattice: chunked so no single main-thread call is long.
bool CaptureGridMonth (int year, int month, int firstOrdinal, std::vector<SunPathGridPoint>& grid)
{
    API_PlaceInfo place {};
    if (ACAPI_GeoLocation_GetPlaceSets (&place) != NoError)
        return false;
    static constexpr int kDays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    for (int day = 1; day <= kDays[month - 1]; ++day)
        for (int minute = 0; minute < 24 * 60; minute += kSunPathGridMinutes) {
            SunPathPoint point;
            if (!SunAt (place, year, month, day, minute, point) || point.direction[2] < 0.0f)
                continue;
            SunPathGridPoint out;
            std::copy (point.direction, point.direction + 3, out.direction);
            out.ordinal = uint16_t (firstOrdinal + day - 1);
            out.minute = uint16_t (minute);
            grid.push_back (out);
        }
    return true;
}

std::shared_ptr<const std::vector<SunPathGridPoint>> CaptureGrid (int year)
{
    static constexpr int kDays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    auto grid = std::make_shared<std::vector<SunPathGridPoint>> ();
    grid->reserve (365 * 80);
    int first = 1;
    for (int month = 1; month <= 12; ++month) {
        bool ok = false;
        GS::UniString error;
        if (!evp::MainThreadGate::Get ().InvokeUnlessStopped (
                [&] { ok = CaptureGridMonth (year, month, first, *grid); }, kTimeoutMs, gStop, error) ||
            !ok)
            return nullptr;
        first += kDays[month - 1];
    }
    return grid;
}

void Worker ()
{
    for (;;) {
        Wanted wanted;
        {
            std::lock_guard<std::mutex> lock (gMutex);
            if (!gWant || gStop) {
                gRunning = false;
                return;
            }
            wanted = *gWant;
            gWant.reset ();
        }
        auto path = std::make_shared<SunPath> ();
        path->year = wanted.year;
        path->month = wanted.month;
        path->day = wanted.day;
        GS::UniString error;
        if (!evp::MainThreadGate::Get ().InvokeUnlessStopped ([&] { Capture (wanted, *path); }, kTimeoutMs, gStop,
                                                              error) &&
            path->error.empty ())
            path->error = std::string (error.ToCStr (0, MaxUSize, CC_UTF8).Get ());
        if (path->error.empty () && gGridYear != wanted.year) {
            gGrid = CaptureGrid (wanted.year);
            gGridYear = gGrid != nullptr ? wanted.year : 0;
        }
        path->grid = gGridYear == wanted.year ? gGrid : nullptr;
        path->ready = true;
        std::lock_guard<std::mutex> lock (gMutex);
        gPath = std::move (path);
    }
}

} // namespace

void RequestSunPath (int year, int month, int day)
{
    if (!DaysValid (year, month, day))
        return;
    const Wanted wanted { year, month, day };
    std::lock_guard<std::mutex> lock (gMutex);
    if (gStop || (gAsked && *gAsked == wanted))
        return;
    gAsked = wanted;
    gWant = wanted;
    if (gRunning)
        return;
    if (gWorker.joinable ())
        gWorker.join (); // finished: its last act was to clear the flag
    gRunning = true;
    gWorker = std::thread (Worker);
}

std::shared_ptr<const SunPath> LatestSunPath ()
{
    std::lock_guard<std::mutex> lock (gMutex);
    return gPath;
}

namespace sunpath {

void Shutdown ()
{
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock (gMutex);
        gStop = true;
        gWant.reset ();
        worker = std::move (gWorker);
    }
    if (worker.joinable ())
        worker.join ();
    std::lock_guard<std::mutex> lock (gMutex);
    gPath.reset ();
}

} // namespace sunpath

} // namespace geomsrv::sunstudyhud
