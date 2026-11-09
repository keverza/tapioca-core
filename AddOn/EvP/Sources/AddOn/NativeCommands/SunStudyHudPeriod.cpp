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
#include "SunStudy/SunPathStudyData.hpp"
#include "SunStudy/SunPathPeriodPublication.hpp"

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
    std::shared_ptr<const SunPathPeriodGeometry> geometry;
    std::shared_ptr<const std::vector<KeptDay>> kept; // playback, when the run kept its days
    std::mutex annualMutex;
};

std::mutex gPeriodMutex; // guards everything below
std::shared_ptr<RetainedPeriod> gRetained;
PeriodInfo gPeriodInfo;
SunPathPeriodPublication s_sunPathPeriod;
std::optional<PeriodView> gWantView;
std::optional<std::vector<size_t>> gWantProfile;
std::shared_ptr<PeriodProfile> gProfile = std::make_shared<PeriodProfile> ();
std::thread gPeriodWorker;
bool gPeriodWorkerRunning = false;

void ReleaseLocked ()
{
    gRetained.reset ();
    s_sunPathPeriod.Reset ();
    gPeriodInfo = PeriodInfo ();
    gWantView.reset ();
    gWantProfile.reset ();
    gProfile = std::make_shared<PeriodProfile> ();
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
    auto geometry = std::make_shared<SunPathPeriodGeometry> ();
    geometry->studyId = carrierId;
    geometry->revision = carrierRevision;
    geometry->year = retained->carrier->year;
    geometry->estimate = estimate;
    geometry->days = std::move (days);
    retained->geometry = std::move (geometry);
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
    std::vector<rs::ArchivedDay> archived;
    size_t bytes = 0;
    if (retained->annual->ArchivedDays (archived, bytes, ignored)) {
        auto kept = std::make_shared<std::vector<KeptDay>> ();
        for (rs::ArchivedDay& day : archived)
            kept->push_back ({ day.month, day.day, day.hoursPerStep, std::move (day.minutes) });
        retained->kept = std::move (kept);
        period.daysKept = true;
        period.keptBytes = bytes;
    }
    std::lock_guard<std::mutex> lock (gPeriodMutex);
    gRetained = std::move (retained);
    gPeriodInfo = period;
    s_sunPathPeriod.Retain (gRetained->geometry);
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

// The colour ramp's top for a view: the scale for a share or an instant, the
// day's daylight for one kept day, else the mean daylight a day of the
// chosen months.
double RampTopOf (const RetainedPeriod& retained, const PeriodInfo& info, const PeriodView& view)
{
    if (view.quantity == 3 || view.quantity == 4 || view.frame == 1)
        return view.scale;
    if (view.frame == 2) {
        if (retained.kept == nullptr || view.day < 0 || size_t (view.day) >= retained.kept->size ())
            return 0.0;
        const KeptDay& day = (*retained.kept)[size_t (view.day)];
        return double (day.minutes.size ()) * day.hoursPerStep;
    }
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
        if (record.rustCore == nullptr)
            return false;
        if (view.frame != 0)
            return retained.annual->PresentDay (*record.rustCore, size_t ((std::max) (view.day, 0)),
                                                view.frame == 1 ? (std::max) (view.step, 0) : -1,
                                                QuantityOf (view.quantity), view.scale, presentError);
        return retained.annual->Present (*record.rustCore, QuantityOf (view.quantity), view.month, view.quantity != 3,
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
    const double top = RampTopOf (retained, info, view);
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

void ComputePeriodProfile (RetainedPeriod& retained, const std::vector<size_t>& samples)
{
    PeriodProfile result;
    result.studyId = retained.carrierId;
    result.revision = retained.carrierRevision;
    result.samples = samples;
    const auto started = std::chrono::steady_clock::now ();
    const auto& carrier = *retained.carrier;
    // ⚠️ CPU ONLY: the GPU traversal's device is not shared with a second
    // caller thread; the CPU tracer is const and thread-safe.
    if (carrier.backend != "cpu")
        result.error = "the point profile re-traces on the CPU; run the period on CPU to use it";
    else if (carrier.traversal == nullptr || samples.empty ())
        result.error = "the study point is not in the period";
    else {
        // The mean of the point's samples: sun up is the site's, the same for
        // every sample; in front and lit are averaged. The per-step series is
        // only for this point, never an archive of every cell on the map.
        for (const auto& day : retained.geometry->days) {
            result.dayWeights.push_back (day.weightDays);
            result.dayHoursPerStep.push_back (day.hoursPerStep);
            std::vector<uint16_t> minutes;
            minutes.reserve (day.steps.size ());
            for (const auto& step : day.steps)
                minutes.push_back (uint16_t (std::clamp (step.time.hour * 60 + step.time.minute, 0, 1439)));
            result.dayMinutes.push_back (std::move (minutes));
        }
        size_t traced = 0;
        for (const size_t sample : samples) {
            if (sample >= carrier.positions.size () / 3)
                continue;
            const double* normal =
                carrier.normals.size () == carrier.positions.size () ? &carrier.normals[3 * sample] : nullptr;
            evp::sunstudy::PointProfile profile;
            evp::sunstudy::ComputePointProfile (&carrier.positions[3 * sample], normal, retained.geometry->days,
                                                *carrier.traversal, 0.001, profile);
            for (int month = 0; month < 12; ++month)
                for (int hour = 0; hour < 24; ++hour) {
                    result.up[month][hour] = profile.up[month][hour];
                    result.facing[month][hour] += profile.facing[month][hour];
                    result.lit[month][hour] += profile.lit[month][hour];
                }
            if (traced == 0) {
                result.dayMonth = std::move (profile.dayMonth);
                result.dayOfMonth = std::move (profile.dayOfMonth);
                result.dayUp = std::move (profile.dayUp);
                result.dayFacing = std::move (profile.dayFacing);
                evp::sunstudy::AccumulateDayCellRange (profile.dayLit, true, result.dayMinLit, result.dayMaxLit);
                result.dayLit = std::move (profile.dayLit);
                result.dayStepFacing.resize (profile.dayStepFacing.size ());
                result.dayStepLit.resize (profile.dayStepLit.size ());
                for (size_t d = 0; d < profile.dayStepLit.size (); ++d) {
                    for (const uint8_t value : profile.dayStepFacing[d])
                        result.dayStepFacing[d].push_back (double (value));
                    for (const uint8_t value : profile.dayStepLit[d])
                        result.dayStepLit[d].push_back (double (value));
                }
            }
            else {
                evp::sunstudy::AccumulateDayCellRange (profile.dayLit, false, result.dayMinLit, result.dayMaxLit);
                for (size_t d = 0; d < result.dayLit.size () && d < profile.dayLit.size (); ++d) {
                    result.dayFacing[d] += profile.dayFacing[d];
                    result.dayLit[d] += profile.dayLit[d];
                    for (size_t s = 0; s < result.dayStepFacing[d].size () && s < profile.dayStepFacing[d].size ();
                         ++s) {
                        result.dayStepFacing[d][s] += profile.dayStepFacing[d][s];
                        result.dayStepLit[d][s] += profile.dayStepLit[d][s];
                    }
                }
            }
            result.rays += profile.rays;
            ++traced;
        }
        if (traced == 0)
            result.error = "the study point is not in the period";
        else {
            const double n = double (traced);
            for (int month = 0; month < 12; ++month)
                for (int hour = 0; hour < 24; ++hour) {
                    result.facing[month][hour] /= n;
                    result.lit[month][hour] /= n;
                }
            for (size_t d = 0; d < result.dayLit.size (); ++d) {
                result.dayFacing[d] /= n;
                result.dayLit[d] /= n;
                for (size_t s = 0; s < result.dayStepLit[d].size (); ++s) {
                    result.dayStepFacing[d][s] /= n;
                    result.dayStepLit[d][s] /= n;
                }
            }
        }
    }
    result.milliseconds =
        std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count ();
    result.ready = true;
    std::lock_guard<std::mutex> lock (gPeriodMutex);
    if (gRetained.get () == &retained && gProfile->samples == samples)
        gProfile = std::make_shared<PeriodProfile> (std::move (result));
}

// Latest wins for both: a view the user has already moved past is skipped.
void PeriodWorker ()
{
    for (;;) {
        std::shared_ptr<RetainedPeriod> retained;
        std::optional<PeriodView> view;
        std::optional<std::vector<size_t>> profile;
        PeriodInfo info;
        {
            std::lock_guard<std::mutex> lock (gPeriodMutex);
            retained = gRetained;
            view = gWantView;
            gWantView.reset ();
            profile = std::move (gWantProfile);
            gWantProfile.reset ();
            info = gPeriodInfo;
            if (retained == nullptr || (!view && !profile)) {
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
                if (error.empty ()) {
                    gPeriodInfo.applied = *view;
                    SunPathAppliedFrame applied { view->frame, view->month, -1, -1 };
                    // Kept-day indices need not equal geometry indices (sunless
                    // days can have no archive frame). Match calendar and clock.
                    if (view->frame != 0 && retained->kept != nullptr && view->day >= 0 &&
                        size_t (view->day) < retained->kept->size ()) {
                        const auto& kept = (*retained->kept)[size_t (view->day)];
                        const int minute = view->step >= 0 && size_t (view->step) < kept.minutes.size ()
                                               ? kept.minutes[size_t (view->step)]
                                               : -1;
                        applied = SunPathAppliedFrameFor (*retained->geometry, view->frame, view->month, kept.month,
                                                          kept.day, minute);
                    }
                    s_sunPathPeriod.Apply (retained->geometry, applied);
                }
            }
        }
        if (profile)
            ComputePeriodProfile (*retained, *profile);
    }
}

bool ReleaseRetained (bool onlyIfIdle)
{
    std::shared_ptr<RetainedPeriod> retired;
    std::string carrierId;
    uint64_t carrierRevision = 0;
    {
        std::lock_guard<std::mutex> lock (gPeriodMutex);
        if (onlyIfIdle && gPeriodWorkerRunning)
            return false;
        if (gRetained != nullptr) {
            carrierId = gRetained->carrierId;
            carrierRevision = gRetained->carrierRevision;
        }
        retired = std::move (gRetained);
        ReleaseLocked ();
    }
    // PeriodRecords::Keep removed this carrier from run-scoped cleanup. Release
    // its remaining store owner too, but never clear unrelated studies or hide
    // the renderer's last published frame. The shared handle keeps the record
    // alive for any frame-local reader until that reader finishes.
    if (!carrierId.empty ())
        evp::sunstudy::SunStudyStore::Get ().Erase (carrierId, carrierRevision);
    return true;
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
    (void) ReleaseRetained (false);
}

bool ReleaseIfIdle ()
{
    // Busy refusal and retirement are one period-lock transaction: a profile or
    // view request cannot start between checking the worker and clearing it.
    return ReleaseRetained (true);
}

void InvalidateSunPath ()
{
    std::lock_guard<std::mutex> lock (gPeriodMutex);
    s_sunPathPeriod.Reset ();
}

void Shutdown ()
{
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock (gPeriodMutex);
        gWantView.reset ();
        gWantProfile.reset ();
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

std::shared_ptr<const SunPathPeriodData> LatestSunPathPeriod ()
{
    std::lock_guard<std::mutex> lock (gPeriodMutex);
    return s_sunPathPeriod.Latest ();
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

std::shared_ptr<const std::vector<KeptDay>> KeptPeriodDays ()
{
    std::lock_guard<std::mutex> lock (gPeriodMutex);
    return gRetained != nullptr ? gRetained->kept : nullptr;
}

bool ReadPeriodPoints (const std::vector<size_t>& samples, PeriodPoint& mean)
{
    mean = PeriodPoint ();
#if defined(TAPIOCA_GEOMETRY_RUST)
    std::shared_ptr<RetainedPeriod> retained;
    {
        std::lock_guard<std::mutex> lock (gPeriodMutex);
        retained = gRetained;
    }
    if (retained == nullptr)
        return false;
    size_t read = 0;
    std::lock_guard<std::mutex> lock (retained->annualMutex);
    for (const size_t sample : samples) {
        rs::AnnualSampleRow row;
        std::string error;
        if (!retained->annual->Sample (sample, row, error))
            continue;
        for (int m = 0; m < 12; ++m) {
            mean.unoccluded[m] += row.unoccluded[size_t (m)];
            mean.facing[m] += row.facing[size_t (m)];
        }
        for (int b = 0; b < 14; ++b)
            mean.daily[b] += row.daily[size_t (b)];
        ++read;
    }
    if (read == 0)
        return false;
    for (int m = 0; m < 12; ++m) {
        mean.unoccluded[m] /= double (read);
        mean.facing[m] /= double (read);
    }
    for (int b = 0; b < 14; ++b)
        mean.daily[b] /= float (read);
    return true;
#else
    (void) samples;
    return false;
#endif
}

void RequestPeriodProfile (const std::vector<size_t>& samples)
{
    std::lock_guard<std::mutex> lock (gPeriodMutex);
    if (gRetained == nullptr || gProfile->samples == samples)
        return; // ready, or already asked for
    gProfile = std::make_shared<PeriodProfile> ();
    gProfile->samples = samples;
    gProfile->studyId = gRetained->carrierId;
    gProfile->revision = gRetained->carrierRevision;
    gWantProfile = samples;
    WakePeriodWorkerLocked ();
}

std::shared_ptr<const PeriodProfile> LatestPeriodProfile ()
{
    std::lock_guard<std::mutex> lock (gPeriodMutex);
    return gProfile; // no per-frame deep copy of every traced day and step
}

} // namespace geomsrv::sunstudyhud
