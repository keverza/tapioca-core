#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/SunStudyHudRunner.hpp"
#include "NativeCommands/SunStudyCommands.hpp"
#include "NativeCommands/SunStudyDisplayCommands.hpp"
#include "NativeCommands/SunStudyDisplayPreparation.hpp"
#include "SunStudy/CpuTraversal.hpp"

#include "AddOnCommands.hpp" // ExecuteNativeCommand, CommandNeedsMainThread
#include "ArchViz/ArchVizLog.hpp"
#include "Geometry/MeshStore.hpp" // MeshStore::FullPassInFlight
#include "Python/MainThreadGate.hpp"
#include "SunStudy/SunStudyStore.hpp"

#if defined(TAPIOCA_GEOMETRY_RUST)
#include "SunStudy/SunStudyRustAnnual.hpp"
#include "SunStudy/SunStudyRustStudy.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <functional>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>

namespace geomsrv::sunstudyhud {

namespace {

constexpr int kSliceSteps = 1;
// Host capture may exceed the gate's default budget. Sampling/BVH preparation
// runs on this worker, never inside the host gate.
constexpr int kMainThreadTimeoutMs = 120000;

std::mutex gMutex;
Status gStatus;
std::thread gThread;
std::shared_ptr<tapioca::SunStudyRunTicket> s_ticket;
std::chrono::steady_clock::time_point s_started;

void SetStage (const std::string& stage)
{
    std::lock_guard<std::mutex> lock (gMutex);
    if (!gStatus.cancelling)
        gStatus.stage = stage;
}

std::string Utf8 (const GS::UniString& text)
{
    return std::string (text.ToCStr (0, MaxUSize, CC_UTF8).Get ());
}

// One command, on the thread it declares. ⚠️ THE GATE JOB OWNS ITS STATE: a
// timed-out Invoke may still run the job later, so nothing it touches lives on
// this frame.
NativeCommandResult Call (const char* name, const GS::ObjectState& params,
                          std::shared_ptr<tapioca::SunStudyRunTicket> ticket)
{
    if (ticket->cancelled.load ())
        return NativeCommandResult::Failure ("sun study cancelled");
    bool needsMainThread = true;
    CommandNeedsMainThread (GS::String (name), needsMainThread);
    if (!needsMainThread)
        return ExecuteNativeCommand (GS::String (name), params);
    struct Job {
        GS::String name;
        GS::ObjectState params;
        NativeCommandResult result;
        std::shared_ptr<tapioca::SunStudyRunTicket> ticket;
    };
    const auto job = std::make_shared<Job> ();
    job->name = GS::String (name);
    job->params = params;
    job->ticket = std::move (ticket);
    job->result = NativeCommandResult::Failure (GS::UniString ("Tapioca.") + name + " did not run on the main thread");
    GS::UniString error;
    if (!evp::MainThreadGate::Get ().Invoke (
            [job] () {
                if (!job->ticket->cancelled.load ())
                    job->result = ExecuteNativeCommand (job->name, job->params);
            },
            kMainThreadTimeoutMs, error))
        return NativeCommandResult::Failure (GS::UniString ("Tapioca.") + name + ": " + error);
    return job->result;
}

// Omitted when empty: an empty list and an absent one mean different role
// tables (SunStudy/SunStudyRoles.hpp).
void AddGuids (GS::ObjectState& params, const char* name, const std::vector<std::string>& guids)
{
    if (guids.empty ())
        return;
    GS::Array<GS::UniString> values;
    for (const auto& guid : guids)
        values.Push (GS::UniString (guid.c_str (), CC_UTF8));
    params.Add (name, values);
}

// Everything StartSunStudy reads, for one date: roles, the cylinder and the
// time slice included, so a period's days differ in their date alone.
GS::ObjectState StartParams (const Request& request, int year, int month, int day)
{
    GS::ObjectState params;
    params.Add ("year", (GS::Int32) year);
    params.Add ("month", (GS::Int32) month);
    params.Add ("day", (GS::Int32) day);
    params.Add ("timestep", (GS::Int32) request.timestep);
    params.Add ("hourFrom", (GS::Int32) request.hourFrom);
    params.Add ("hourTo", (GS::Int32) request.hourTo);
    params.Add ("grid", request.grid);
    params.Add ("samples", GS::UniString ("surfaces"));
    params.Add ("domain", GS::UniString ("patch"));
    params.Add ("backend", GS::UniString (request.backend.c_str (), CC_UTF8));
    params.Add ("preset", GS::UniString (request.preset.c_str (), CC_UTF8));
    params.Add ("glassThreshold", request.glassThreshold);
    if (request.bounded) {
        params.Add ("clipX", request.centreX);
        params.Add ("clipY", request.centreY);
        params.Add ("clipRadius", request.radius);
    }
    AddGuids (params, "contextElements", request.contextElements);
    AddGuids (params, "ignoredElements", request.ignoredElements);
    return params;
}

// One host capture through the gate. ⚠️ THE JOB OWNS ITS STATE: `capture` is
// moved in and must own whatever it reads, because a timed-out Invoke may
// still run it later.
using CaptureFn = std::function<NativeCommandResult (std::shared_ptr<const CapturedSunStudyInputs>&)>;

NativeCommandResult CaptureOnMain (CaptureFn capture, const std::shared_ptr<tapioca::SunStudyRunTicket>& ticket,
                                   std::shared_ptr<const CapturedSunStudyInputs>& captured)
{
    struct CaptureJob {
        std::shared_ptr<const CapturedSunStudyInputs> captured;
        NativeCommandResult result = NativeCommandResult::Failure ("sun capture did not run");
    };
    const auto job = std::make_shared<CaptureJob> ();
    GS::UniString gateError;
    if (!evp::MainThreadGate::Get ().Invoke (
            [job, capture = std::move (capture), ticket] {
                if (!ticket->cancelled.load ())
                    job->result = capture (job->captured);
            },
            kMainThreadTimeoutMs, gateError))
        return NativeCommandResult::Failure (gateError);
    captured = job->captured;
    return job->result;
}

// One prepared day to convergence, its progress published as it goes. Empty
// on success and on cancellation (the caller reads the token).
std::string AdvanceToConvergence (const std::string& studyId, uint64_t revision, const std::atomic<bool>& cancel)
{
    auto& store = evp::sunstudy::SunStudyStore::Get ();
    for (;;) {
        if (cancel)
            return "";
        size_t advanced = 0;
        std::string error;
        if (!store.Advance (studyId, kSliceSteps, evp::sunstudy::BackgroundTraversalThreads (), 0.001, 0.0, advanced,
                            error, &cancel, revision))
            return error;
        if (cancel)
            return "";
        evp::sunstudy::StudyProgress progress;
        evp::sunstudy::SunStudyBackendReport backend;
        if (!store.Progress (studyId, progress, error))
            return error;
        store.Backend (studyId, backend, error);
        {
            std::lock_guard<std::mutex> lock (gMutex);
            if (!gStatus.cancelling) {
                gStatus.resolvedSteps = progress.resolvedSteps;
                gStatus.totalSteps = progress.totalSteps;
                gStatus.resolvedBackend = backend.resolved;
                gStatus.backendFallbackReason = backend.fallbackReason;
            }
        }
        if (progress.converged)
            return "";
        if (advanced == 0)
            return "the study stopped before converging";
    }
}

std::string DateText (int year, int month, int day)
{
    char text[16];
    std::snprintf (text, sizeof (text), "%04d-%02d-%02d", year, month, day);
    return text;
}

// Day records a period run made and has not handed on: erased on every exit
// but the one that keeps the carrier, so a cancelled or failed period leaves
// nothing in the store and the last shown study stays shown.
class PeriodRecords {
  public:
    PeriodRecords () = default;
    PeriodRecords (const PeriodRecords&) = delete;
    PeriodRecords& operator= (const PeriodRecords&) = delete;
    ~PeriodRecords ()
    {
        for (const auto& [id, revision] : owned_)
            evp::sunstudy::SunStudyStore::Get ().Erase (id, revision);
    }
    void Own (const std::string& id, uint64_t revision)
    {
        owned_.emplace_back (id, revision);
    }
    void Drop (const std::string& id)
    {
        const auto found =
            std::find_if (owned_.begin (), owned_.end (), [&id] (const auto& owned) { return owned.first == id; });
        if (found == owned_.end ())
            return;
        evp::sunstudy::SunStudyStore::Get ().Erase (found->first, found->second);
        owned_.erase (found);
    }
    void Keep (const std::string& id)
    {
        owned_.erase (
            std::remove_if (owned_.begin (), owned_.end (), [&id] (const auto& owned) { return owned.first == id; }),
            owned_.end ());
    }

  private:
    std::vector<std::pair<std::string, uint64_t>> owned_;
};

#if defined(TAPIOCA_GEOMETRY_RUST)
namespace rs = evp::sunstudy::rust;

// Folds one traced day into the period, creating the totals on the first one
// (the sample count is known only then) with any sunless days seen before it.
std::string AddPeriodDay (std::unique_ptr<rs::RustAnnualSun>& annual, std::vector<std::pair<int, double>>& sunless,
                          const evp::sunstudy::StudyRecord& record, const rs::AnnualDay& day)
{
    std::string error;
    if (record.rustCore == nullptr)
        return "a period day finished without a study core";
    if (annual == nullptr) {
        TapiocaStudyInfo info {};
        if (!record.rustCore->Info (info, error))
            return error;
        annual = rs::RustAnnualSun::Create (info.sampleCount, error);
        if (annual == nullptr)
            return "the period's totals could not be allocated (" + error + ")";
        for (const auto& [month, weight] : sunless)
            if (!annual->AddSunless (month, weight, error))
                return error;
        sunless.clear ();
    }
    if (!annual->Add (*record.rustCore, day.month, day.weightDays, error))
        return error == "AnnualReceptorsChanged" ? "the samples changed between days of the period; nothing was mixed"
                                                 : error;
    return "";
}
#endif

// A period (#152). The schedule comes from the Rust core; roles, place and THE
// snapshot are captured once; each day then recaptures only its sun on that
// same snapshot, is traced as an ordinary day study, folded into the totals
// and erased. The newest traced day is kept: it is the next day's reuse source
// and, at the end, the carrier the period's mean hours a day are presented
// through. Returns the error; empty on success or cancellation, and on success
// `studyId`/`revision` name the carrier.
std::string RunPeriod (const Request& request, const std::shared_ptr<tapioca::SunStudyRunTicket>& ticket,
                       std::string& studyId, uint64_t& revision)
{
#if defined(TAPIOCA_GEOMETRY_RUST)
    const auto& cancel = ticket->cancelled;
    const bool year = request.period == "year";
    const int fromMonth = year ? 1 : request.month, fromDay = year ? 1 : request.day;
    const int toMonth = year ? 12 : request.toMonth, toDay = year ? 31 : request.toDay;
    std::vector<rs::AnnualDay> days;
    uint32_t represented = 0;
    std::string error;
    const auto sampling = request.dailyResolved ? rs::AnnualSampling::DailyResolved : rs::AnnualSampling::Preview;
    if (!rs::ScheduleAnnualDays (request.year, fromMonth, fromDay, toMonth, toDay, sampling, days, represented, error))
        return error == "AnnualInvalidRange" ? "the period must end on or after its first day, in the same year"
                                             : "the period is not a calendar date range (" + error + ")";
    const std::string label =
        DateText (request.year, fromMonth, fromDay) + " to " + DateText (request.year, toMonth, toDay);
    {
        std::lock_guard<std::mutex> lock (gMutex);
        gStatus.period = true;
        gStatus.estimate = !request.dailyResolved;
        gStatus.periodDays = days.size ();
        gStatus.representedDays = represented;
        gStatus.periodLabel = label;
    }
    archviz::ArchVizLog ("pipeline: stage=sun-period-start period=" + label +
                         " sampling=" + (request.dailyResolved ? "daily" : "preview") +
                         " days=" + std::to_string (days.size ()) + " represented=" + std::to_string (represented) +
                         " hours=" + std::to_string (request.hourFrom) + "-" + std::to_string (request.hourTo) +
                         " timestep=" + std::to_string (request.timestep));

    std::shared_ptr<const CapturedSunStudyInputs> base;
    const GS::ObjectState baseParams =
        StartParams (request, days.front ().year, days.front ().month, days.front ().day);
    if (const auto captured = CaptureOnMain (
            [baseParams] (std::shared_ptr<const CapturedSunStudyInputs>& out) {
                return CaptureSunStudyInputs (baseParams, out);
            },
            ticket, base);
        !captured.ok)
        return cancel ? "" : Utf8 (captured.error);

    PeriodRecords records;
    std::unique_ptr<rs::RustAnnualSun> annual;
    std::vector<std::pair<int, double>> sunless; // seen before the first traced day
    std::shared_ptr<const evp::sunstudy::StudyRecord> carrier;
    std::string carrierId;
    uint64_t carrierRevision = 0;
    for (size_t index = 0; index < days.size (); ++index) {
        if (cancel)
            return "";
        const rs::AnnualDay& day = days[index];
        {
            std::lock_guard<std::mutex> lock (gMutex);
            gStatus.currentDate = DateText (day.year, day.month, day.day);
            gStatus.resolvedSteps = gStatus.totalSteps = 0;
        }
        SetStage ("snapshot");
        std::shared_ptr<const CapturedSunStudyInputs> captured;
        const GS::ObjectState params = StartParams (request, day.year, day.month, day.day);
        if (const auto recaptured = CaptureOnMain (
                [base, params] (std::shared_ptr<const CapturedSunStudyInputs>& out) {
                    return RecaptureSunStudyDay (*base, params, out);
                },
                ticket, captured);
            !recaptured.ok)
            return cancel ? "" : Utf8 (recaptured.error);
        if (cancel)
            return "";
        if (captured->series.Empty ()) {
            // No sun above the floor in the slice: the days count, at zero.
            if (annual == nullptr)
                sunless.emplace_back (day.month, day.weightDays);
            else if (!annual->AddSunless (day.month, day.weightDays, error))
                return error;
        }
        else {
            SetStage ("starting");
            const NativeCommandResult start = PrepareCapturedSunStudy (captured, cancel, carrier);
            if (!start.ok)
                return cancel ? "" : Utf8 (start.error);
            GS::UniString idText;
            start.data.Get ("studyId", idText);
            const std::string id = Utf8 (idText);
            const uint64_t idRevision = evp::sunstudy::SunStudyStore::Get ().Revision (id);
            records.Own (id, idRevision);
            bool empty = false;
            start.data.Get ("empty", empty);
            if (empty)
                return "nothing to study: no samples in the analysis";
            SetStage ("calculating");
            if (const std::string failed = AdvanceToConvergence (id, idRevision, cancel); !failed.empty ())
                return failed;
            if (cancel)
                return "";
            const auto record = evp::sunstudy::SunStudyStore::Get ().CompletedRecord (id);
            if (record == nullptr)
                return "a period day was not complete after converging";
            if (const std::string failed = AddPeriodDay (annual, sunless, *record, day); !failed.empty ())
                return failed;
            if (!carrierId.empty ())
                records.Drop (carrierId);
            carrier = record;
            carrierId = id;
            carrierRevision = idRevision;
            std::lock_guard<std::mutex> lock (gMutex);
            gStatus.carrierDate = DateText (day.year, day.month, day.day);
        }
        std::lock_guard<std::mutex> lock (gMutex);
        gStatus.periodDaysDone = index + 1;
    }
    if (annual == nullptr || carrier == nullptr)
        return "no sun above the horizon in this time slice on any day of the period";
    rs::AnnualInfo info;
    std::vector<double> daily;
    double dailyCeiling = 0.0;
    if (!annual->Present (*carrier->rustCore, rs::AnnualQuantity::Unoccluded, 0, true, error) ||
        !annual->Info (info, error) ||
        !annual->Values (rs::AnnualQuantity::Unoccluded, 0, true, daily, dailyCeiling, error))
        return error;
    const double representedDays = info.representedDays > 0.0 ? info.representedDays : 1.0;
    {
        std::lock_guard<std::mutex> lock (gMutex);
        gStatus.meanTotalHours = info.meanHours;
        gStatus.daylightTotalHours = info.ceilingHours;
        gStatus.meanDailyHours = info.meanHours / representedDays;
        gStatus.maxDailyHours = info.maxHours / representedDays;
        gStatus.daylightDailyHours = dailyCeiling;
    }
    char line[480];
    std::snprintf (line, sizeof (line),
                   "pipeline: stage=sun-period-result study=%s carrier=%04d-%02d-%02d sampledDays=%zu "
                   "representedDays=%.0f samples=%zu meanHours=%.6f minHours=%.6f maxHours=%.6f "
                   "siteDaylightHours=%.6f meanDailyHours=%.6f estimate=%d",
                   carrierId.c_str (), int (carrier->year), int (carrier->month), int (carrier->day), info.sampledDays,
                   info.representedDays, info.sampleCount, info.meanHours, info.minHours, info.maxHours,
                   info.ceilingHours, info.meanHours / representedDays, request.dailyResolved ? 0 : 1);
    archviz::ArchVizLog (line);
    records.Keep (carrierId);
    studyId = carrierId;
    revision = carrierRevision;
    return "";
#else
    (void) request;
    (void) ticket;
    (void) studyId;
    (void) revision;
    return "period studies need the Rust study core";
#endif
}

void Run (Request request, std::shared_ptr<tapioca::SunStudyRunTicket> ticket)
{
    const auto& gCancel = ticket->cancelled;
    const auto started = std::chrono::steady_clock::now ();
    std::string studyId;
    uint64_t revision = 0;
    const auto finish = [&] (const std::string& stage, const std::string& error) {
        const bool cancelled = gCancel.load ();
        if (stage == "failed")
            ticket->Cancel (); // invalidate any timed-out gate job before releasing ownership
        if ((cancelled || stage == "failed") && !studyId.empty ())
            evp::sunstudy::SunStudyStore::Get ().Erase (studyId, revision);
        std::string line;
        {
            std::lock_guard<std::mutex> lock (gMutex);
            sunfollow::ReleaseManualRun ();
            gStatus.running = false;
            gStatus.stage = cancelled ? "cancelled" : stage;
            gStatus.error = cancelled ? "" : error;
            gStatus.cancelling = false;
            if (cancelled)
                gStatus.resolvedSteps = gStatus.totalSteps = 0;
            gStatus.studyId = studyId;
            gStatus.wallMs =
                std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count ();
            if (cancelled)
                archviz::ArchVizLog ("sun-cancel source=manual generation=" + std::to_string (gStatus.runs) +
                                     " study=" + studyId +
                                     " acknowledgedMs=" + std::to_string (gStatus.wallMs - gStatus.cancelRequestedMs) +
                                     " publicationRevoked=true");
            // Requested beside resolved: a GPU request that fell back is
            // visible here and in the HUD, never rewritten to `cpu`.
            if (!studyId.empty ())
                line = "pipeline: stage=sun-hud-backend study=" + studyId + " requested=" + gStatus.requestedBackend +
                       " resolved=" + (gStatus.resolvedBackend.empty () ? "untried" : gStatus.resolvedBackend) +
                       " outcome=" + (cancelled ? "cancelled" : stage) +
                       (gStatus.backendFallbackReason.empty () ? "" : " reason=" + gStatus.backendFallbackReason);
        }
        if (!line.empty ())
            archviz::ArchVizLog (line);
    };
    const auto failed = [&] (const NativeCommandResult& result) { finish ("failed", Utf8 (result.error)); };

    // The previous follower would otherwise see this snapshot and rerun its
    // own study underneath this one.
    SetStage ("snapshot");
    // ⚠️ WAIT FOR THE VIEWER'S FULL PASS HERE, OFF THE MAIN THREAD. Until it
    // publishes, BuildSnapshot finds no complete shared snapshot and walks the
    // whole model itself in ONE main-thread call: 37 s of a blocked Archicad
    // on an 806k-triangle load, with that load stalled behind it (#97). The
    // pass is bounded by its own maxSeconds; Cancel still works here.
    if (MeshStore::Get ().FullPassInFlight ()) {
        SetStage ("waiting");
        const auto waitStarted = std::chrono::steady_clock::now ();
        while (MeshStore::Get ().FullPassInFlight () && !gCancel)
            std::this_thread::sleep_for (std::chrono::milliseconds (100));
        if (gCancel)
            return finish ("cancelled", "");
        archviz::ArchVizLog (
            "pipeline: stage=sun-wait-viewer-pass waitedMs=" +
            std::to_string (int64_t (
                std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - waitStarted).count ())));
        SetStage ("snapshot");
    }
    GS::ObjectState snapshotParams;
    snapshotParams.Add ("scope", GS::UniString ("all"));
    snapshotParams.Add ("reuseShared", true);
    if (const auto snapshot = Call ("BuildSnapshot", snapshotParams, ticket); !snapshot.ok)
        return failed (snapshot);
    if (gCancel)
        return finish ("cancelled", "");

    GS::UniString id;
    double hoursMax = 0.0; // 0: the day's own daylight
    if (request.period != "day") {
        SetStage ("starting");
        const std::string periodError = RunPeriod (request, ticket, studyId, revision);
        if (gCancel)
            return finish ("cancelled", "");
        if (!periodError.empty ())
            return finish ("failed", periodError);
        id = GS::UniString (studyId.c_str (), CC_UTF8);
        {
            std::lock_guard<std::mutex> lock (gMutex);
            gStatus.studyId = studyId;
            hoursMax = gStatus.daylightDailyHours;
        }
        // ⚠️ A PERIOD IS NEVER FOLLOWED: an edit must not rerun its carrier
        // day over it. Before the show, which this would otherwise cancel.
        sunfollow::Disable ();
    }
    else {
        SetStage ("starting");
        // Capture SDK inputs on the host; prepare on this worker with the same
        // token used by ray batches.
        std::shared_ptr<const CapturedSunStudyInputs> captured;
        const GS::ObjectState params = StartParams (request, request.year, request.month, request.day);
        if (const auto capture = CaptureOnMain (
                [params] (std::shared_ptr<const CapturedSunStudyInputs>& out) {
                    return CaptureSunStudyInputs (params, out);
                },
                ticket, captured);
            !capture.ok)
            return failed (capture);
        const NativeCommandResult start = PrepareCapturedSunStudy (captured, gCancel);
        if (!start.ok)
            return failed (start);
        start.data.Get ("studyId", id);
        studyId = Utf8 (id);
        revision = evp::sunstudy::SunStudyStore::Get ().Revision (studyId);
        evp::sunstudy::StudyProgress initialProgress;
        std::string progressError;
        evp::sunstudy::SunStudyStore::Get ().Progress (studyId, initialProgress, progressError);
        {
            std::lock_guard<std::mutex> lock (gMutex);
            gStatus.studyId = studyId;
            gStatus.totalSteps = initialProgress.totalSteps;
            gStatus.resolvedSteps = initialProgress.resolvedSteps;
        }
        bool empty = false;
        start.data.Get ("empty", empty);
        if (empty)
            return finish ("failed", "nothing to study: no sun above the horizon in this time slice, or no samples");

        SetStage ("calculating");
        if (const std::string advanceError = AdvanceToConvergence (studyId, revision, gCancel); !advanceError.empty ())
            return finish ("failed", advanceError);
        if (gCancel)
            return finish ("cancelled", "");
    }

    // Shown, the follower adopts a day and keeps it current through edits.
    SetStage ("showing");
    GS::ObjectState show;
    show.Add ("studyId", id);
    show.Add ("show", true);
    show.Add ("debug", (GS::Int32) 0);
    show.Add ("depth", (GS::Int32) 0);
    if (request.period != "day") {
        show.Add ("follow", false);
        if (hoursMax > 0.0)
            show.Add ("hoursMax", hoursMax);
    }
    if (gCancel)
        return finish ("cancelled", "");
    if (const auto shown = ShowSunStudyForHud (show, ticket); !shown.ok)
        return failed (shown);
    while (ManualSunStudyDisplayStatus ().preparing) {
        if (gCancel) {
            CancelManualSunStudyDisplays ();
            while (ManualSunStudyDisplayBusy ())
                std::this_thread::sleep_for (std::chrono::milliseconds (10));
            return finish ("cancelled", "");
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    if (const auto display = ManualSunStudyDisplayStatus (); !display.error.empty ())
        return finish ("failed", display.error);
    // The result's fingerprint, so two runs of one model -- the other backend,
    // another build -- can be compared from the log (#97).
    evp::sunstudy::SunStudyResultSummary summary;
    std::string summaryError;
    if (evp::sunstudy::SunStudyStore::Get ().Summary (studyId, summary, summaryError)) {
        char line[320];
        std::snprintf (line, sizeof (line),
                       "pipeline: stage=sun-result study=%s samples=%zu meanHours=%.9f minHours=%.6f maxHours=%.6f "
                       "fullyLit=%zu fullyShaded=%zu daylightHours=%.6f",
                       studyId.c_str (), summary.count, summary.meanHours, summary.minHours, summary.maxHours,
                       summary.fullyLit, summary.fullyShaded, summary.daylightHours);
        archviz::ArchVizLog (line);
    }
    finish ("done", "");
}

} // namespace

bool Submit (const Request& request, std::string& error)
{
    if (request.period != "day" && request.period != "period" && request.period != "year") {
        error = "period must be day, period or year";
        return false;
    }
    if (request.hourFrom < 0 || request.hourTo > 24 || request.hourFrom >= request.hourTo) {
        error = "the time slice must run forward within 0-24 h";
        return false;
    }
    std::lock_guard<std::mutex> lock (gMutex);
    if (gStatus.running) {
        error = "a sun study is already running";
        return false;
    }
    if (gThread.joinable ())
        gThread.join (); // finished: its last act was to clear `running`
    if (!sunfollow::ReserveManualRun (error))
        return false;
    s_ticket = std::make_shared<tapioca::SunStudyRunTicket> ();
    s_started = std::chrono::steady_clock::now ();
    const uint64_t runs = gStatus.runs + 1;
    gStatus = Status {};
    gStatus.running = true;
    gStatus.stage = "queued";
    gStatus.runs = runs;
    gStatus.requestedBackend = request.backend;
    gThread = std::thread (Run, request, s_ticket);
    return true;
}

void Cancel ()
{
    std::lock_guard<std::mutex> lock (gMutex);
    if (!gStatus.running || s_ticket == nullptr || !s_ticket->Cancel ())
        return;
    gStatus.cancelRequestedMs =
        std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - s_started).count ();
    archviz::ArchVizLog (
        "sun-cancel source=manual stage=" + gStatus.stage + " generation=" + std::to_string (gStatus.runs) +
        " study=" + gStatus.studyId + " resolved=" + std::to_string (gStatus.resolvedSteps) + "/" +
        std::to_string (gStatus.totalSteps) + " elapsedMs=" + std::to_string (gStatus.cancelRequestedMs));
    gStatus.cancelling = true;
    gStatus.stage = "cancelling";
}

Status Current ()
{
    std::lock_guard<std::mutex> lock (gMutex);
    return gStatus;
}

Status CurrentJob ()
{
    return JobStatusOf (Current (), sunfollow::State ());
}

void CancelActive ()
{
    if (Current ().running)
        Cancel ();
    else
        sunfollow::CancelActiveRun ();
}

bool ResumeFollowing (std::string& error)
{
    error.clear ();
    return sunfollow::Resume (error);
}

void Shutdown ()
{
    Cancel ();
    std::thread thread;
    {
        std::lock_guard<std::mutex> lock (gMutex);
        thread = std::move (gThread);
    }
    if (thread.joinable ())
        thread.join ();
}

} // namespace geomsrv::sunstudyhud
