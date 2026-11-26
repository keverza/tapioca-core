// Weather-weighted light on a planting layer (#278 Stage 5):
// RequestPlantingClimate and CurrentPlantingClimate, declared with the
// runner's other plain-value calls in SunStudyHudRunner.hpp. One worker
// thread: Archicad's sun for the weather year through the main-thread gate
// (SunStudyWeatherSun, refused unless its cross-check passes), then the
// climate run (SunStudyClimateRun) on the completed layer's own samples and
// traversal, then the layer's area-weighted monthly figures.
//
// ⚠️ THE GATE JOB CAPTURES BY VALUE. It holds a shared_ptr to its own request
// and answer, never a reference to this thread's locals: a timed-out job may
// still run later.

#include "NativeCommands/SunStudyHudPeriod.hpp"
#include "NativeCommands/SunStudyHudRunner.hpp"
#include "NativeCommands/SunStudyWeatherSun.hpp"
#include "Python/MainThreadGate.hpp"
#include "SunStudy/SunStudyClimateRun.hpp"
#include "SunStudy/SunStudyStore.hpp"
#include "SunStudy/SunStudyThreadPolicy.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <cstdio>
#include <functional>
#include <optional>
#include <thread>

namespace geomsrv::sunstudyhud {

namespace {

namespace ss = evp::sunstudy;

constexpr int kSunCaptureTimeoutMs = 120000; // 8 784 host calls take ~0.4 s

struct Want {
    std::string studyId;
    double layerHeight = 0.0;
    std::shared_ptr<const ss::WeatherData> weather;
    int year = 2026;
};

std::mutex gMutex; // guards everything below
PlantingClimate gClimate;
std::optional<Want> gWant;
std::thread gWorker;
bool gWorkerRunning = false;
std::atomic<bool> gCancel { false };
std::atomic<size_t> gDone { 0 };

bool LeapYear (int year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

// The gate job's own state: shared, so a late run touches nothing freed.
struct SunJob {
    WeatherSunRequest request;
    WeatherSunCapture capture;
    std::string error;
    bool ok = false;
};

void Publish (const Want& want, const std::function<void (PlantingClimate&)>& edit)
{
    std::lock_guard<std::mutex> lock (gMutex);
    if (gClimate.studyId == want.studyId && gClimate.year == want.year && want.weather &&
        gClimate.weatherSha256 == want.weather->identity.sha256)
        edit (gClimate);
}

void Run (const Want& want)
{
    const auto started = std::chrono::steady_clock::now ();
    auto fail = [&] (const std::string& error) {
        Publish (want, [&] (PlantingClimate& c) {
            c.state = PlantingClimate::State::Failed;
            c.error = error;
        });
    };
    const ss::WeatherData& weather = *want.weather;
    const auto record = ss::SunStudyStore::Get ().CompletedRecord (want.studyId);
    if (record == nullptr || record->traversal == nullptr) {
        fail ("the layer's study is not complete or has no geometry");
        return;
    }
    PlantingSite site;
    std::string error;
    if (!ReadPlantingSite (want.studyId, want.layerHeight, site, error)) {
        fail (error);
        return;
    }
    if (site.areas.size () * 3 != record->positions.size () || record->normals.size () != record->positions.size ()) {
        fail ("the layer's samples and areas do not match");
        return;
    }

    // The records to place: 29 February only in a leap year.
    const bool leap = LeapYear (want.year);
    std::vector<size_t> kept;
    auto job = std::make_shared<SunJob> ();
    job->request.year = want.year;
    job->request.recordsPerHour = static_cast<int> (weather.identity.recordsPerHour);
    job->request.stationOffsetHours = weather.identity.timeZoneHours;
    for (size_t k = 0; k < weather.records.size (); ++k) {
        const ss::WeatherRecord& r = weather.records[k];
        if (!leap && r.month == 2 && r.day == 29)
            continue;
        kept.push_back (k);
        job->request.stamps.push_back ({ r.month, r.day, r.hour, r.minute });
    }

    // Archicad's sun, on the main thread.
    GS::UniString gateError;
    const bool ran = evp::MainThreadGate::Get ().InvokeUnlessStopped (
        [job] { job->ok = CaptureWeatherSun (job->request, job->capture, job->error); }, kSunCaptureTimeoutMs, gCancel,
        gateError);
    if (gCancel.load ())
        return;
    if (!ran) {
        fail ("Archicad's sun could not be read: " + std::string (gateError.ToCStr (0, MaxUSize, CC_UTF8).Get ()));
        return;
    }
    if (!job->ok) {
        fail (job->error);
        return;
    }
    const WeatherSunCapture& sun = job->capture;
    if (sun.failedCalls != 0 || !sun.check.accepted) {
        char text[200];
        std::snprintf (text, sizeof text,
                       "Archicad's sun does not match the weather file's clock (mean %.2f deg, max %.2f deg, %zu "
                       "failed): nothing was computed.",
                       sun.check.meanDegrees, sun.check.maxDegrees, sun.failedCalls);
        fail (text);
        return;
    }

    // The intervals, with the host's sun.
    ss::ClimateRunRequest request;
    request.positions = record->positions;
    request.normals = record->normals;
    request.sunSource = ss::SunSource::Host;
    request.rotation = sun.rotation;
    request.intervals.reserve (kept.size ());
    for (size_t i = 0; i < kept.size (); ++i) {
        const ss::WeatherRecord& r = weather.records[kept[i]];
        const auto& instant = sun.instants[i];
        ss::rust::ClimateInterval interval;
        interval.sun = sun.host[i];
        interval.hours = instant.hours;
        interval.dni = r.Get (ss::WeatherField::DirectNormal);
        interval.dhi = r.Get (ss::WeatherField::DiffuseHorizontal);
        interval.albedo = r.Get (ss::WeatherField::Albedo);
        interval.dewPointC = r.Get (ss::WeatherField::DewPoint);
        interval.dayOfYear = instant.dayOfYear;
        interval.month = int (r.month) - 1;
        request.intervals.push_back (interval);
    }
    Publish (want, [&] (PlantingClimate& c) {
        c.state = PlantingClimate::State::Tracing;
        c.total = request.intervals.size ();
        c.sunMeanDegrees = sun.check.meanDegrees;
        c.sunMaxDegrees = sun.check.maxDegrees;
    });

    ss::ClimateRunResult result;
    if (!ss::RunClimate (
            *record->traversal, request, [] { return gCancel.load (); }, [] (size_t done, size_t) { gDone = done; },
            result, error)) {
        if (!gCancel.load ())
            fail (error);
        return;
    }

    // The layer's figures.
    PlantingClimate figures;
    std::vector<ss::rust::Monthly> totals (result.direct.size ());
    for (size_t s = 0; s < totals.size (); ++s)
        for (size_t m = 0; m < 12; ++m)
            totals[s][m] = (result.direct[s][m] + result.diffuse[s][m] + result.reflected[s][m]) / 1000.0;
    double viewSum = 0.0, viewArea = 0.0;
    for (size_t s = 0; s < result.skyView.size (); ++s)
        if (!std::isnan (result.skyView[s]) && site.areas[s] > 0.0) {
            viewSum += result.skyView[s] * site.areas[s];
            viewArea += site.areas[s];
        }
    const double nan = std::numeric_limits<double>::quiet_NaN ();
    for (int m = 0; m < 12; ++m) {
        figures.dli[size_t (m)] = ss::AreaWeightedMean (result.dli, site.areas, m);
        figures.energyKwh[size_t (m)] = ss::AreaWeightedMean (totals, site.areas, m);
        const double valid = result.report.validHours[size_t (m)];
        const double all = valid + result.report.missingHours[size_t (m)];
        figures.coverage[size_t (m)] = all > 0.0 ? valid / all : nan;
    }
    const double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now () - started).count ();
    Publish (want, [&] (PlantingClimate& c) {
        if (gCancel.load ())
            return;
        c.state = PlantingClimate::State::Ready;
        c.station = weather.identity.city + ", " + weather.identity.country;
        c.samples = result.traced;
        c.dli = figures.dli;
        c.energyKwh = figures.energyKwh;
        c.coverage = figures.coverage;
        c.skyView = viewArea > 0.0 ? viewSum / viewArea : nan;
        c.seconds = seconds;
    });
}

void Worker ()
{
    (void) ss::UseInteractiveStudyPriority ();
    for (;;) {
        Want want;
        {
            std::lock_guard<std::mutex> lock (gMutex);
            if (!gWant) {
                gWorkerRunning = false;
                return;
            }
            want = std::move (*gWant);
            gWant.reset ();
            gCancel = false;
            gDone = 0;
        }
        try {
            Run (want);
        }
        catch (const std::exception& e) {
            Publish (want, [&] (PlantingClimate& c) {
                c.state = PlantingClimate::State::Failed;
                c.error = e.what ();
            });
        }
    }
}

} // namespace

void RequestPlantingClimate (const std::string& studyId, double layerHeight,
                             std::shared_ptr<const evp::sunstudy::WeatherData> weather, int year)
{
    if (studyId.empty () || weather == nullptr)
        return;
    std::lock_guard<std::mutex> lock (gMutex);
    if (gClimate.studyId == studyId && gClimate.year == year && gClimate.weatherSha256 == weather->identity.sha256 &&
        gClimate.state != PlantingClimate::State::Idle)
        return; // running, done, or failed for good: asked once per study, file and year
    gClimate = PlantingClimate ();
    gClimate.studyId = studyId;
    gClimate.weatherSha256 = weather->identity.sha256;
    gClimate.year = year;
    gClimate.state = PlantingClimate::State::Capturing;
    gWant = Want { studyId, layerHeight, std::move (weather), year };
    gCancel = true; // a run for something else stops; the worker then takes this one
    if (gWorkerRunning)
        return;
    if (gWorker.joinable ())
        gWorker.join (); // finished: its last act was to clear the flag
    gWorkerRunning = true;
    gWorker = std::thread (Worker);
}

PlantingClimate CurrentPlantingClimate ()
{
    std::lock_guard<std::mutex> lock (gMutex);
    PlantingClimate climate = gClimate;
    if (climate.state == PlantingClimate::State::Tracing)
        climate.done = gDone.load ();
    return climate;
}

namespace plantingclimate {

void Shutdown ()
{
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock (gMutex);
        gWant.reset ();
        gCancel = true;
        worker = std::move (gWorker);
    }
    if (worker.joinable ())
        worker.join ();
    std::lock_guard<std::mutex> lock (gMutex);
    gClimate = PlantingClimate ();
}

} // namespace plantingclimate

} // namespace geomsrv::sunstudyhud
