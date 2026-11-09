#ifndef EVP_NATIVECOMMANDS_SUNSTUDYHUDRUNNER_HPP
#define EVP_NATIVECOMMANDS_SUNSTUDYHUDRUNNER_HPP

// The viewer HUD's "Run sun study": the same command sequence the palette's
// Native sun study runs (pause the follower, capture, prepare, advance in
// cancellable slices, show), through the same services without Python.
//
// ⚠️ PLAIN TYPES ONLY IN THIS HEADER. The HUD (render thread, ArchViz) includes
// it; the DevKit and the command registry stay in the .cpp.
//
// Submit/Cancel/Current are safe from any thread. The run itself is on its own
// thread; commands that need the host main thread go through MainThreadGate.

#include "SunStudy/SunStudyPointProfile.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv::sunfollow {
struct FollowerStats;
}

namespace geomsrv::sunstudyhud {

struct Request {
    int year = 2026, month = 3, day = 21;
    // What is traced (#152): "day" the one date above; "period" from that
    // date to toMonth/toDay of the same year; "year" January 1 to December 31.
    // A period is traced day by day on ONE pinned scene and shown as the mean
    // direct-sun hours a day; it is never followed through edits.
    std::string period = "day";
    int toMonth = 6, toDay = 21;
    // Period sampling: false = Preview (one middle day per covered month,
    // weighted by its days: an estimate), true = every day.
    bool dailyResolved = false;
    // Keep every traced day's whole-map lit bits IN RAM for playback
    // (continuous, fixed hour, day by day). No file is written. Off by default:
    // about 11 bytes a sample and day; point playback re-traces on demand.
    bool keepDays = false;
    // The time slice, every mode: whole hours, `hourTo` exclusive.
    int hourFrom = 0, hourTo = 24;
    int timestep = 15; // minutes: 15, 30 or 60
    double grid = 1.0; // cell size, metres
    std::string preset = "early";
    double glassThreshold = 0.4;
    // The palette command's roles: Context casts shadow and is not measured,
    // Ignored is absent; everything else is analysed.
    std::vector<std::string> contextElements;
    std::vector<std::string> ignoredElements;
    // The size safeguard (SunStudy/SunStudyBounds.hpp): only elements reaching
    // into this plan circle are measured; the rest still cast shadow.
    bool bounded = false;
    double centreX = 0.0, centreY = 0.0, radius = 0.0;
    // "cpu" or "gpu", sent as asked and kept by the follower's replacements
    // (SunStudy/SunStudyBackend.hpp). CPU is the baseline.
    std::string backend = "cpu";
};

struct Status {
    bool running = false;
    bool cancelling = false;
    bool following = false;
    bool paused = false;
    bool stale = false;
    std::string stage; // "waiting" (for the viewer's load), "snapshot", "starting", "calculating", "showing", "done",
                       // "cancelled", "failed"
    size_t resolvedSteps = 0;
    size_t totalSteps = 0;
    std::string studyId;
    std::string error;
    double wallMs = 0.0;
    double cancelRequestedMs = 0.0;
    uint64_t runs = 0;
    // As asked, and as it ran: `resolvedBackend` is empty until the first
    // traced step; `backendFallbackReason` is the GPU's `sun-gpu-fallback`.
    std::string requestedBackend;
    std::string resolvedBackend;
    std::string backendFallbackReason;
    // A period run (#152): the schedule and where it is, then its result.
    // `resolvedSteps/totalSteps` stay the current day's.
    // As asked, for the result's explanation: the single day's date (empty for
    // a period) and the time slice, "00:00-24:00".
    std::string dayDate;
    std::string sliceLabel;
    bool period = false;
    bool wholeYear = false;    // January 1 to December 31
    bool estimate = false;     // Preview sampling
    size_t periodDays = 0;     // days traced in all
    size_t periodDaysDone = 0; // days traced so far, sunless ones included
    uint32_t representedDays = 0;
    std::string periodLabel; // "2026-01-01 to 2026-12-31"
    std::string currentDate; // the day being traced
    std::string carrierDate; // the traced day whose atlas shows the period and whose steps the shadow views scrub
    // The shown result, over the whole period: hours a day (mean of represented
    // days) and in total, unoccluded, with the site daylight as the ceiling.
    double meanDailyHours = 0.0, maxDailyHours = 0.0, daylightDailyHours = 0.0;
    double meanTotalHours = 0.0, daylightTotalHours = 0.0;
};

// A period's overall progress, 0..1: finished days plus the current day's
// resolved fraction, over all days. ⚠️ ONE NUMBER FOR THE WHOLE RUN: the
// per-day steps restart at 0 every day, and a bar that fills hundreds of times
// reads as a glitch.
inline double PeriodProgress (const Status& status)
{
    if (status.periodDays == 0)
        return 0.0;
    const double day = status.totalSteps > 0 ? double (status.resolvedSteps) / double (status.totalSteps) : 0.0;
    const double done = double (status.periodDaysDone) + (status.periodDaysDone < status.periodDays ? day : 0.0);
    const double fraction = done / double (status.periodDays);
    return fraction < 0.0 ? 0.0 : (fraction > 1.0 ? 1.0 : fraction);
}

// ---- the finished period (#152) -----------------------------------------
//
// The last successful period run is retained: its totals, its carrier day and
// the sun of every traced day. The map can then show another quantity, month
// or threshold, and the hovered point can be read or re-traced, without a new
// run. A new run (day or period) releases it.

// What the map shows. All views are a represented day's mean, except the two
// shares, which `scale` stretches over the colour ramp (100 % at its top).
struct PeriodView {
    int quantity = 0;  // 0 direct sun, 1 possible (sun in front), 2 lost to obstruction, 3 sun access share,
                       // 4 share of days with at least edges[threshold] hours
    int month = 0;     // 0 the whole period, 1-12 one month (quantity 4: whole period only)
    int threshold = 3; // index into PeriodInfo::edges, quantity 4
    double scale = 1.0;
    // Playback of a run that kept its days (PeriodInfo::daysKept): 0 the
    // totals of `month`; 1 one step of one kept day, lit or not (`scale` at
    // lit); 2 one kept day's `quantity` (0-3) against its daylight.
    int frame = 0;
    int day = 0, step = 0; // index into KeptPeriodDays, and into its minutes
    bool operator== (const PeriodView& other) const
    {
        return quantity == other.quantity && month == other.month && threshold == other.threshold &&
               scale == other.scale && frame == other.frame && day == other.day && step == other.step;
    }
    bool operator!= (const PeriodView& other) const
    {
        return !(*this == other);
    }
};

struct PeriodInfo {
    bool available = false;
    std::string studyId; // the carrier the map is drawn through
    std::string label;   // "2026-01-01 to 2026-12-31"
    bool estimate = false;
    double representedDays = 0.0;
    double monthDays[12] = {};    // represented days per month (0: not in the period)
    double siteDaylight[12] = {}; // hours per month
    double edges[13] = {};        // daily-hours thresholds
    PeriodView applied;           // what is on screen
    bool busy = false;            // a view or a profile is being prepared
    std::string error;            // the last view's refusal
    bool daysKept = false;        // the run kept its days: playback frames 1 and 2
    size_t keptBytes = 0;
};

// A day the run kept for playback: its date and each step's clock minute.
struct KeptDay {
    int month = 0, day = 0;
    double hoursPerStep = 0.0; // 0: no sun above the floor that day
    std::vector<uint16_t> minutes;
};

// One sample's whole period, from the stored totals (no tracing).
struct PeriodPoint {
    double unoccluded[12] = {}, facing[12] = {}; // hours per month
    float daily[14] = {};                        // represented days per daily-hours bin (PeriodInfo::edges)
};

// A study point's samples re-traced against every traced day's sun, averaged:
// when it is lit.
struct PeriodProfile {
    std::string studyId;
    uint64_t revision = 0;
    std::vector<size_t> samples;
    bool ready = false;
    std::string error;
    double up[12][24] = {}, facing[12][24] = {}, lit[12][24] = {}; // weighted hours, month x clock hour
    std::vector<int> dayMonth, dayOfMonth;
    std::vector<double> dayWeights;               // represented days per traced day, aligned with the arrays below
    std::vector<double> dayUp, dayFacing, dayLit; // mean hours, one per traced day
    std::vector<double> dayMinLit, dayMaxLit;     // exact per-cell range on that day
    // Point-only playback: each day's clock minutes and averaged cell shares,
    // aligned by day and step. This is the no-whole-map-archive alternative.
    std::vector<double> dayHoursPerStep;
    std::vector<std::vector<uint16_t>> dayMinutes;
    std::vector<std::vector<double>> dayStepFacing, dayStepLit;
    size_t rays = 0;
    double milliseconds = 0.0;
};

// The selected traced day as the shared clock's mean steps + exact cell range.
inline evp::sunstudy::PointDay PeriodPointDay (const PeriodProfile& profile, size_t selected)
{
    evp::sunstudy::PointDay day;
    day.samples = profile.samples.size ();
    if (selected < profile.dayHoursPerStep.size ())
        day.hoursPerStep = profile.dayHoursPerStep[selected];
    if (selected < profile.dayMinutes.size ())
        day.minutes = profile.dayMinutes[selected];
    if (selected < profile.dayStepLit.size ())
        day.lit = profile.dayStepLit[selected];
    if (selected < profile.dayStepFacing.size ())
        day.facing = profile.dayStepFacing[selected];
    if (selected < profile.dayMinLit.size () && selected < profile.dayMaxLit.size ()) {
        day.minDirectHours = profile.dayMinLit[selected];
        day.maxDirectHours = profile.dayMaxLit[selected];
        day.hasCellRange = true;
    }
    return day;
}
PeriodInfo CurrentPeriod ();
// Latest wins: a request replaces any not yet applied.
bool ShowPeriodView (const PeriodView& view, std::string& error);
// The kept days, or null when the run kept none.
std::shared_ptr<const std::vector<KeptDay>> KeptPeriodDays ();
// A study point's period: the mean of its samples' stored totals.
bool ReadPeriodPoints (const std::vector<size_t>& samples, PeriodPoint& mean);
// Asks for the samples' profile; LatestPeriodProfile shares its immutable publication.
void RequestPeriodProfile (const std::vector<size_t>& samples);
std::shared_ptr<const PeriodProfile> LatestPeriodProfile ();
// Release the retained totals, carrier, profile and optional playback archive.
// The renderer keeps its last frame; period browsing needs another run.
// Refuses while a manual run or the period worker is active; repeated calls are safe.
bool ClearPeriodCache (std::string& error);

// ---- the sun path (#152, an optional preview) ---------------------------
//
// Archicad's sun positions for the project's location and north: day arcs
// (kind 0 winter solstice, 1 equinox, 2 summer solstice, 3 the chosen date;
// a point every ten minutes) and hour analemmas (kind 4, every seventh day),
// above the horizon only. Captured on the main thread by one worker; a
// request for the same date is not repeated.
struct SunPathPoint {
    float direction[3] = { 0.0f, 0.0f, 1.0f }; // unit, toward the sun, model space
    int minute = 0;                            // clock minute (day arcs)
};
struct SunPathArc {
    int kind = 0;
    int month = 0, day = 0, hour = 0;
    std::vector<SunPathPoint> points;
};
// The draggable sun's lattice: every day of a 365-day year at every
// kSunPathGridMinutes, above the horizon only, from the same host call. The
// HUD picks the point nearest the cursor, so a drag lands on a position
// Archicad gave -- no solar formula on this side.
constexpr int kSunPathGridMinutes = 10;
struct SunPathGridPoint {
    float direction[3] = { 0.0f, 0.0f, 1.0f };
    uint16_t ordinal = 1; // day of a 365-day year, 1 = January 1
    uint16_t minute = 0;
};
struct SunPath {
    bool ready = false;
    std::string error;
    int year = 0, month = 0, day = 0;
    double northRadians = 0.0; // model angle of north, CCW from +X
    double latitude = 0.0, longitude = 0.0;
    std::vector<SunPathArc> arcs;
    // Shared between paths of the same year: a date change re-captures the
    // arcs only. Null until captured (or when it failed).
    std::shared_ptr<const std::vector<SunPathGridPoint>> grid;
};
void RequestSunPath (int year, int month, int day);
std::shared_ptr<const SunPath> LatestSunPath ();

bool Submit (const Request& request, std::string& error);
void Cancel ();
Status Current ();
// Plain-data boundary for the HUD: exactly one owner of the primary control.
Status CurrentJob ();
Status JobStatusOf (Status manual, const sunfollow::FollowerStats& follower);
void CancelActive ();
bool ResumeFollowing (std::string& error);
// Quit/unload: cancel and join.
void Shutdown ();

} // namespace geomsrv::sunstudyhud

#endif
