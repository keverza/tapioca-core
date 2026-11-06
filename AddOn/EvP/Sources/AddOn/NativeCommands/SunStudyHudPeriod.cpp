#include "APIEnvir.h"
#include "ACAPinc.h"

// NativeCommands/SunStudyHudPeriod -- the finished period the HUD looks through
// (#152), split from SunStudyHudRunner.cpp for the size cap: the retained
// totals, carrier and suns; the one worker that presents another view or
// traces the hovered point; and the plain-value calls the HUD makes.

#include "NativeCommands/SunStudyHudPeriod.hpp"
#include "NativeCommands/SunStudyHudRunner.hpp"
#include "NativeCommands/SunStudyDisplayCommands.hpp"
#include "NativeCommands/SunStudyDisplayPreparation.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>

namespace geomsrv::sunstudyhud {

namespace {

#if defined(TAPIOCA_GEOMETRY_RUST)
namespace rs = evp::sunstudy::rust;
#endif

std::string Utf8 (const GS::UniString& text)
{
    return std::string (text.ToCStr (0, MaxUSize, CC_UTF8).Get ());
}

// ---- the finished period, retained (#152) ----------------------------------
//
// ⚠️ ONE CALLER AT A TIME INTO THE ACCUMULATOR: the Rust handle is
// one-thread-at-a-time, so every call holds `annualMutex`. The carrier record
// is held here too, so it outlives a store Erase while the map draws through it.
struct RetainedPeriod {
#if defined(TAPIOCA_GEOMETRY_RUST)
    std::unique_ptr<rs::RustAnnualSun> annual;
#endif
    std::shared_ptr<const evp::sunstudy::StudyRecord> carrier;
    std::string carrierId;
    uint64_t carrierRevision = 0;
    std::vector<evp::sunstudy::ProfileDay> days;
    std::mutex annualMutex;
};

std::mutex gPeriodMutex; // guards everything below
std::shared_ptr<RetainedPeriod> gRetained;
PeriodInfo gPeriodInfo;
std::optional<PeriodView> gWantView;
size_t gWantProfile = SIZE_MAX;
PeriodProfile gProfile;
std::thread gPeriodWorker;
bool gPeriodWorkerRunning = false;

void ReleaseLocked ()
{
    gRetained.reset ();
    gPeriodInfo = PeriodInfo ();
    gWantView.reset ();
    gWantProfile = SIZE_MAX;
    gProfile = PeriodProfile ();
}

#if defined(TAPIOCA_GEOMETRY_RUST)
void RetainImpl (std::unique_ptr<rs::RustAnnualSun> annual, std::shared_ptr<const evp::sunstudy::StudyRecord> carrier,
                 const std::string& carrierId, uint64_t carrierRevision, std::vector<evp::sunstudy::ProfileDay> days,
                 const rs::AnnualInfo& info, const std::string& label, bool estimate)
{
    auto retained = std::make_shared<RetainedPeriod> ();
    retained->annual = std::move (annual);
    retained->carrier = std::move (carrier);
    retained->carrierId = carrierId;
    retained->carrierRevision = carrierRevision;
    retained->days = std::move (days);
    PeriodInfo period;
    period.available = true;
    period.studyId = carrierId;
    period.label = label;
    period.estimate = estimate;
    period.representedDays = info.representedDays;
    std::copy (info.monthDays.begin (), info.monthDays.end (), period.monthDays);
    std::copy (info.siteDaylight.begin (), info.siteDaylight.end (), period.siteDaylight);
    rs::AnnualSampleRow row;
    std::string ignored;
    if (retained->annual->Sample (0, row, ignored))
        std::copy (row.edges.begin (), row.edges.end (), period.edges);
    std::lock_guard<std::mutex> lock (gPeriodMutex);
    gRetained = std::move (retained);
    gPeriodInfo = period;
}

rs::AnnualQuantity QuantityOf (int quantity)
{
    switch (quantity) {
        case 1:
            return rs::AnnualQuantity::SurfaceFacing;
        case 2:
            return rs::AnnualQuantity::ObstructionLoss;
        case 3:
            return rs::AnnualQuantity::AccessRatio;
        case 4:
            return rs::AnnualQuantity::DaysAtLeast;
        default:
            return rs::AnnualQuantity::Unoccluded;
    }
}
#endif

// The colour ramp's top for a view: the scale for a share, else the mean
// daylight a day of the chosen months.
double RampTopOf (const PeriodInfo& info, const PeriodView& view)
{
    if (view.quantity == 3 || view.quantity == 4)
        return view.scale;
    double hours = 0.0, days = 0.0;
    for (int month = 1; month <= 12; ++month)
        if (view.month == 0 || view.month == month) {
            hours += info.siteDaylight[month - 1];
            days += info.monthDays[month - 1];
        }
    return days > 0.0 ? hours / days : 0.0;
}

// Present a view through the carrier and show it, waiting for the display.
std::string ApplyPeriodView (RetainedPeriod& retained, const PeriodInfo& info, const PeriodView& view)
{
#if defined(TAPIOCA_GEOMETRY_RUST)
    std::string error;
    const auto present = [&retained, &view] (const evp::sunstudy::StudyRecord& record, std::string& presentError) {
        std::lock_guard<std::mutex> lock (retained.annualMutex);
        return record.rustCore != nullptr &&
               retained.annual->Present (*record.rustCore, QuantityOf (view.quantity), view.month, view.quantity != 3,
                                         presentError, view.threshold, view.scale);
    };
    if (!evp::sunstudy::SunStudyStore::Get ().Represent (retained.carrierId, retained.carrierRevision, present, error))
        return error;
    GS::ObjectState show;
    show.Add ("studyId", GS::UniString (retained.carrierId.c_str (), CC_UTF8));
    show.Add ("show", true);
    show.Add ("follow", false);
    show.Add ("debug", (GS::Int32) 0);
    show.Add ("depth", (GS::Int32) 0);
    const double top = RampTopOf (info, view);
    if (top > 0.0)
        show.Add ("hoursMax", top);
    // The display worker takes one preparation at a time: a frame of the month
    // player may find the previous one still uploading.
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
#else
    (void) retained;
    (void) info;
    (void) view;
    return "period views need the Rust study core";
#endif
}

void ComputePeriodProfile (RetainedPeriod& retained, size_t sample)
{
    PeriodProfile result;
    result.sample = sample;
    const auto started = std::chrono::steady_clock::now ();
    const auto& carrier = *retained.carrier;
    // ⚠️ CPU ONLY: the GPU traversal's device is not shared with a second
    // caller thread; the CPU tracer is const and thread-safe.
    if (carrier.backend != "cpu")
        result.error = "the point profile re-traces on the CPU; run the period on CPU to use it";
    else if (carrier.traversal == nullptr || sample >= carrier.positions.size () / 3)
        result.error = "the hovered sample is not in the period";
    else {
        const double* normal =
            carrier.normals.size () == carrier.positions.size () ? &carrier.normals[3 * sample] : nullptr;
        evp::sunstudy::PointProfile profile;
        evp::sunstudy::ComputePointProfile (&carrier.positions[3 * sample], normal, retained.days, *carrier.traversal,
                                            0.001, profile);
        for (int month = 0; month < 12; ++month)
            for (int hour = 0; hour < 24; ++hour) {
                result.up[month][hour] = profile.up[month][hour];
                result.facing[month][hour] = profile.facing[month][hour];
                result.lit[month][hour] = profile.lit[month][hour];
            }
        result.dayMonth = std::move (profile.dayMonth);
        result.dayOfMonth = std::move (profile.dayOfMonth);
        result.dayUp = std::move (profile.dayUp);
        result.dayFacing = std::move (profile.dayFacing);
        result.dayLit = std::move (profile.dayLit);
        result.rays = profile.rays;
    }
    result.milliseconds =
        std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count ();
    result.ready = true;
    std::lock_guard<std::mutex> lock (gPeriodMutex);
    if (gProfile.sample == sample)
        gProfile = std::move (result);
}

// Latest wins for both: a view the user has already moved past is skipped.
void PeriodWorker ()
{
    for (;;) {
        std::shared_ptr<RetainedPeriod> retained;
        std::optional<PeriodView> view;
        size_t profile = SIZE_MAX;
        PeriodInfo info;
        {
            std::lock_guard<std::mutex> lock (gPeriodMutex);
            retained = gRetained;
            view = gWantView;
            gWantView.reset ();
            profile = gWantProfile;
            gWantProfile = SIZE_MAX;
            info = gPeriodInfo;
            if (retained == nullptr || (!view && profile == SIZE_MAX)) {
                gPeriodWorkerRunning = false;
                gPeriodInfo.busy = false;
                return;
            }
        }
        if (view) {
            const std::string error = ApplyPeriodView (*retained, info, *view);
            std::lock_guard<std::mutex> lock (gPeriodMutex);
            if (gRetained == retained) {
                gPeriodInfo.error = error;
                if (error.empty ())
                    gPeriodInfo.applied = *view;
            }
        }
        if (profile != SIZE_MAX)
            ComputePeriodProfile (*retained, profile);
    }
}

// Caller holds gPeriodMutex.
void WakePeriodWorkerLocked ()
{
    gPeriodInfo.busy = true;
    if (gPeriodWorkerRunning)
        return;
    if (gPeriodWorker.joinable ())
        gPeriodWorker.join (); // finished: its last act was to clear the flag
    gPeriodWorkerRunning = true;
    gPeriodWorker = std::thread (PeriodWorker);
}

} // namespace

namespace period {

#if defined(TAPIOCA_GEOMETRY_RUST)
void Retain (std::unique_ptr<rs::RustAnnualSun> annual, std::shared_ptr<const evp::sunstudy::StudyRecord> carrier,
             const std::string& carrierId, uint64_t carrierRevision, std::vector<evp::sunstudy::ProfileDay> days,
             const rs::AnnualInfo& info, const std::string& label, bool estimate)
{
    RetainImpl (std::move (annual), std::move (carrier), carrierId, carrierRevision, std::move (days), info, label,
                estimate);
}
#endif

bool WorkerBusy ()
{
    std::lock_guard<std::mutex> lock (gPeriodMutex);
    return gPeriodWorkerRunning;
}

void Release ()
{
    std::lock_guard<std::mutex> lock (gPeriodMutex);
    ReleaseLocked ();
}

void Shutdown ()
{
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock (gPeriodMutex);
        gWantView.reset ();
        gWantProfile = SIZE_MAX;
        worker = std::move (gPeriodWorker);
    }
    if (worker.joinable ())
        worker.join ();
    Release ();
}

} // namespace period

PeriodInfo CurrentPeriod ()
{
    std::lock_guard<std::mutex> lock (gPeriodMutex);
    return gPeriodInfo;
}

bool ShowPeriodView (const PeriodView& view, std::string& error)
{
    if (Current ().running) {
        error = "a sun study is running";
        return false;
    }
    std::lock_guard<std::mutex> lock (gPeriodMutex);
    if (gRetained == nullptr) {
        error = "no finished period to show - run a period or year study first";
        return false;
    }
    gWantView = view;
    WakePeriodWorkerLocked ();
    return true;
}

bool ReadPeriodPoint (size_t sample, PeriodPoint& point)
{
#if defined(TAPIOCA_GEOMETRY_RUST)
    std::shared_ptr<RetainedPeriod> retained;
    {
        std::lock_guard<std::mutex> lock (gPeriodMutex);
        retained = gRetained;
    }
    if (retained == nullptr)
        return false;
    rs::AnnualSampleRow row;
    std::string error;
    {
        std::lock_guard<std::mutex> lock (retained->annualMutex);
        if (!retained->annual->Sample (sample, row, error))
            return false;
    }
    std::copy (row.unoccluded.begin (), row.unoccluded.end (), point.unoccluded);
    std::copy (row.facing.begin (), row.facing.end (), point.facing);
    std::copy (row.daily.begin (), row.daily.end (), point.daily);
    return true;
#else
    (void) sample;
    (void) point;
    return false;
#endif
}

void RequestPeriodProfile (size_t sample)
{
    std::lock_guard<std::mutex> lock (gPeriodMutex);
    if (gRetained == nullptr || gProfile.sample == sample)
        return; // ready, or already asked for
    gProfile = PeriodProfile ();
    gProfile.sample = sample;
    gWantProfile = sample;
    WakePeriodWorkerLocked ();
}

PeriodProfile LatestPeriodProfile ()
{
    std::lock_guard<std::mutex> lock (gPeriodMutex);
    return gProfile;
}

} // namespace geomsrv::sunstudyhud
