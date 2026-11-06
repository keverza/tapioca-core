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

#include <cstddef>
#include <cstdint>
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
    bool period = false;
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
