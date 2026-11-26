// A planting layer's daily light integral drawn in the viewer (#278 Stage 5,
// #155): ShowPlantingDli, HidePlantingDli, CurrentPlantingDli and
// PlantingDliHeldHours, declared with the runner's other plain-value calls in
// SunStudyHudRunner.hpp.
//
// The map is presented through the layer's own study (SunStudyStore::
// Represent), on the hours ramp: DLI x kPlantingDliRampTop / top. What the
// study held before -- the traced day, or a period's mean daily hours, which
// are themselves a presentation -- is kept and put back on Hide.
//
// ⚠️ WHILE THE MAP IS SHOWN THE STUDY'S HOURS ARE DLI. Every planting reader
// that reads the study's hours (ReadPlantingSite, ReadPlantingAreas) asks
// PlantingDliHeldHours first and reads the kept hours instead; a new reader
// must do the same, or it classes light by DLI.
//
// ⚠️ PUT BACK ONLY WHAT IS STILL OURS. If anything else presented through the
// study since (a period view on the Sun tab), the study's values are no longer
// the map's, and Hide forgets the map without overwriting them.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/SunStudyDisplayCommands.hpp"
#include "NativeCommands/SunStudyDisplayPreparation.hpp"
#include "NativeCommands/SunStudyHudPeriod.hpp"
#include "NativeCommands/SunStudyHudRunner.hpp"
#include "SunStudy/SunStudyDliMap.hpp"
#include "SunStudy/SunStudyStore.hpp"

#if defined(TAPIOCA_GEOMETRY_RUST)
#include "SunStudy/SunStudyRustStudy.hpp"
#endif

#include <chrono>
#include <mutex>
#include <optional>
#include <thread>

namespace geomsrv::sunstudyhud {

namespace {

namespace ss = evp::sunstudy;

struct Held {
    std::string studyId;
    uint64_t revision = 0;
    double keptMax = 0.0; // the layer's own display ceiling
    // What the study held before, to put back.
    bool wasPresented = false;
    std::vector<double> hours;
    double ceiling = 0.0;
    // What the map put in, to recognise it.
    std::vector<double> shown;
    PlantingDli view;
};

struct ShowWant {
    std::string studyId;
    double hoursMax = 0.0;
};

std::mutex gMutex; // guards everything below
std::optional<Held> gHeld;
std::optional<ShowWant> gShow;
std::string gDisplayError;
std::thread gWorker;
bool gWorkerRunning = false;
bool gStopping = false;

std::string Utf8 (const GS::UniString& text)
{
    return std::string (text.ToCStr (0, MaxUSize, CC_UTF8).Get ());
}

// Put `want` on screen and wait for the display to be prepared.
std::string Display (const ShowWant& want)
{
    GS::ObjectState show;
    show.Add ("studyId", GS::UniString (want.studyId.c_str (), CC_UTF8));
    show.Add ("show", true);
    show.Add ("follow", false);
    show.Add ("debug", (GS::Int32) 0);
    show.Add ("depth", (GS::Int32) 0);
    if (want.hoursMax > 0.0)
        show.Add ("hoursMax", want.hoursMax);
    // The display worker takes one preparation at a time.
    for (int attempt = 0;; ++attempt) {
        const auto shown = ShowSunStudyForHud (show, nullptr);
        if (shown.ok)
            break;
        const std::string why = Utf8 (shown.error);
        if (why.find ("already running") == std::string::npos || attempt >= 200)
            return why;
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    while (ManualSunStudyDisplayStatus ().preparing)
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    return ManualSunStudyDisplayStatus ().error;
}

void Worker ()
{
    for (;;) {
        ShowWant want;
        {
            std::lock_guard<std::mutex> lock (gMutex);
            if (!gShow || gStopping) {
                gWorkerRunning = false;
                return;
            }
            want = std::move (*gShow);
            gShow.reset ();
        }
        const std::string error = Display (want);
        std::lock_guard<std::mutex> lock (gMutex);
        gDisplayError = error;
    }
}

// Called with gMutex held.
void QueueDisplayLocked (const std::string& studyId, double hoursMax)
{
    if (gStopping)
        return;
    gShow = ShowWant { studyId, hoursMax };
    gDisplayError.clear ();
    if (gWorkerRunning)
        return;
    if (gWorker.joinable ())
        gWorker.join (); // finished: its last act was to clear the flag
    gWorkerRunning = true;
    gWorker = std::thread (Worker);
}

// Put back what the map replaced, when the study still shows the map. Called
// with gMutex held; forgets the map either way.
void RestoreLocked (bool reshow)
{
    if (!gHeld)
        return;
    Held held = std::move (*gHeld);
    gHeld.reset ();
#if defined(TAPIOCA_GEOMETRY_RUST)
    std::string error;
    bool ours = false;
    const auto restore = [&held, &ours] (const ss::StudyRecord& record, std::string& restoreError) {
        if (record.rustCore == nullptr)
            return false;
        std::vector<double> now;
        if (!record.rustCore->SunHours (now, restoreError))
            return false;
        ours = now == held.shown;
        if (!ours)
            return true; // someone else presented since: theirs stays
        return held.wasPresented ? record.rustCore->Present (held.hours, held.ceiling, restoreError)
                                 : record.rustCore->Withdraw (restoreError);
    };
    // A refusal means the study is gone or busy; there is nothing to put back.
    if (ss::SunStudyStore::Get ().Represent (held.studyId, held.revision, restore, error) && ours && reshow)
        QueueDisplayLocked (held.studyId, held.keptMax);
#else
    (void) reshow;
#endif
}

} // namespace

bool ShowPlantingDli (const std::string& studyId, int month, std::string& error)
{
#if defined(TAPIOCA_GEOMETRY_RUST)
    error.clear ();
    const PlantingClimate climate = CurrentPlantingClimate ();
    if (climate.state != PlantingClimate::State::Ready || climate.studyId != studyId || climate.sampleDli == nullptr) {
        error = "weigh this layer by the weather first";
        return false;
    }
    ss::DliMap map;
    if (!ss::BuildDliMap (*climate.sampleDli, month, map, error))
        return false;
    uint64_t revision = 0;
    double keptMax = 0.0;
    if (!plantinglayers::Find (studyId, revision, keptMax)) {
        error = "the layer is no longer kept";
        return false;
    }
    std::vector<double> shown (map.values.size ());
    const double scale = kPlantingDliRampTop / map.top;
    for (size_t i = 0; i < shown.size (); ++i)
        shown[i] = map.values[i] * scale;

    std::lock_guard<std::mutex> lock (gMutex);
    if (gHeld && gHeld->studyId != studyId)
        RestoreLocked (false); // another layer's map: its own hours go back, this layer is shown below
    // Keep what the study holds now, unless it already shows our map.
    std::optional<Held> prior = std::move (gHeld);
    gHeld.reset ();
    std::optional<Held> held = prior;
    const auto present = [&] (const ss::StudyRecord& record, std::string& presentError) {
        if (record.rustCore == nullptr) {
            presentError = "the layer's study has no Rust core";
            return false;
        }
        std::vector<double> now;
        if (!record.rustCore->SunHours (now, presentError))
            return false;
        if (now.size () != shown.size ()) {
            presentError = "the weather run's samples do not match the layer's";
            return false;
        }
        if (!held || now != held->shown) {
            TapiocaStudyInfo info {};
            if (!record.rustCore->Info (info, presentError))
                return false;
            held = Held ();
            held->wasPresented = info.presented != 0;
            held->hours = std::move (now);
            held->ceiling = info.daylightHours;
        }
        return record.rustCore->Present (shown, kPlantingDliRampTop, presentError);
    };
    if (!ss::SunStudyStore::Get ().Represent (studyId, revision, present, error)) {
        gHeld = std::move (prior); // nothing changed: a map already shown stays
        return false;
    }
    held->studyId = studyId;
    held->revision = revision;
    held->keptMax = keptMax;
    held->shown = std::move (shown);
    held->view.shown = true;
    held->view.studyId = studyId;
    held->view.month = month;
    held->view.top = map.top;
    held->view.largest = map.largest;
    held->view.missing = map.missing;
    held->view.samples = map.values.size ();
    gHeld = std::move (held);
    QueueDisplayLocked (studyId, kPlantingDliRampTop);
    return true;
#else
    (void) studyId;
    (void) month;
    error = "the DLI map needs the Rust study core";
    return false;
#endif
}

void HidePlantingDli (bool reshow)
{
    std::lock_guard<std::mutex> lock (gMutex);
    RestoreLocked (reshow);
}

PlantingDli CurrentPlantingDli ()
{
    std::lock_guard<std::mutex> lock (gMutex);
    PlantingDli view = gHeld ? gHeld->view : PlantingDli ();
    view.displayError = gDisplayError;
    return view;
}

bool PlantingDliHeldHours (const std::string& studyId, std::vector<double>& hours)
{
    std::lock_guard<std::mutex> lock (gMutex);
    if (!gHeld || gHeld->studyId != studyId)
        return false;
    hours = gHeld->hours;
    return true;
}

bool PlantingDliHeldHourAt (const std::string& studyId, size_t sample, double& hours)
{
    std::lock_guard<std::mutex> lock (gMutex);
    if (!gHeld || gHeld->studyId != studyId || sample >= gHeld->hours.size ())
        return false;
    hours = gHeld->hours[sample];
    return true;
}

namespace plantingdli {

void Shutdown ()
{
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock (gMutex);
        gStopping = true;
        gShow.reset ();
        RestoreLocked (false);
        worker = std::move (gWorker);
    }
    if (worker.joinable ())
        worker.join ();
    std::lock_guard<std::mutex> lock (gMutex);
    gStopping = false;
    gWorkerRunning = false;
    gDisplayError.clear ();
}

} // namespace plantingdli

} // namespace geomsrv::sunstudyhud
