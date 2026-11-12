#ifndef EVP_ARCHVIZ_SUNSTUDYOBSTRUCTIONCONTROLLER_HPP
#define EVP_ARCHVIZ_SUNSTUDYOBSTRUCTIONCONTROLLER_HPP

// Single-day exact obstruction requests. Latest selection wins; stale work is
// cancelled and never published. CPU raycasts and drawable construction run on
// the study task worker, while the returned immutable frame is render-ready.

#include "ArchViz/GhPreviewGeometry.hpp"
#include "ArchViz/SunStudyObstructionGeometry.hpp"
#include "SunStudy/SunStudySolarCutCells.hpp"
#include "SunStudy/SunStudySolarExposure.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv {
struct Snapshot;
}

namespace geomsrv::archviz {

enum class SunObstructionMode {
    Off,
    TimeStep,
    WholeDay,
    SolarCut, // what of the blockers must go for sun through the slice
};

struct SunObstructionRequest {
    std::string studyId;
    std::vector<size_t> samples; // ordered study-point cells
    SunObstructionMode mode = SunObstructionMode::Off;
    int step = -1;                       // TimeStep only; WholeDay traces every measured step in the slice
    int fromMinute = 0, toMinute = 1440; // WholeDay: the diagram's slice; SolarCut: the pink cut slice
    // SolarCut: a lifted point is cut for in the air; otherwise the snapped cell.
    bool lifted = false;
    double liftedPoint[3] = {};
    double radius = 0.0; // SolarCut: the study point's radius; the cut clears the whole disc
};

// The solar cut arrives in stages, each a complete frame: the cutting surface
// at once, then what goes, then the shadows re-traced without it.
enum class SunSolarCutStage : uint8_t {
    Surface, // the envelope and the hours now; nothing clipped yet
    Cut,     // removed parts, volumes; the re-trace still running
    Done,
};

// Where a running solar cut is, read live by the panel while the frame of the
// last finished stage is shown. One per request, shared by its stage frames;
// the worker writes it, the UI only reads. A stage that runs long says so,
// and one that stops advancing says THAT, instead of looking finished (#168).
enum class SunSolarCutPhase : int {
    Surface,    // the cutting surface
    Cut,        // clipping the blockers
    Cells,      // re-tracing the study cells' rays past what remains
    LitSurface, // projecting the won-lit surface, surface by surface
    CellHours,  // drawing the re-counted cells
    Done,
};
const char* SunSolarCutPhaseName (SunSolarCutPhase phase);

struct SunSolarCutProgress {
    static int64_t NowMs ()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds> (
                   std::chrono::steady_clock::now ().time_since_epoch ())
            .count ();
    }
    std::atomic<int> phase { int (SunSolarCutPhase::Surface) };
    std::atomic<uint64_t> done { 0 }, total { 0 }; // of the current phase; total 0: unknown
    std::atomic<uint64_t> ticks { 0 };             // inner work units, to tell slow from stuck
    std::atomic<int64_t> startedMs { NowMs () }, phaseMs { NowMs () }, advancedMs { NowMs () };
    std::atomic<double> phaseSeconds[int (SunSolarCutPhase::Done)] = {};

    void Enter (SunSolarCutPhase next, uint64_t of = 0)
    {
        const int64_t now = NowMs ();
        const int previous = phase.load ();
        if (previous >= 0 && previous < int (SunSolarCutPhase::Done))
            phaseSeconds[previous].store (double (now - phaseMs.load ()) / 1000.0);
        phase.store (int (next));
        done.store (0);
        total.store (of);
        phaseMs.store (now);
        advancedMs.store (now);
    }
    void Advance (uint64_t count)
    {
        done.store (count);
        advancedMs.store (NowMs ());
    }
    void Tick ()
    {
        if ((ticks.fetch_add (1) & 1023u) == 0)
            advancedMs.store (NowMs ());
    }
};

// One element's share of the solar cut, cubic metres.
struct SunSolarCutObject {
    std::string guid;
    int32_t elemType = 0;
    double removed = 0.0, total = 0.0; // cubic metres
    bool closed = true;                // false: an open mesh, no cap or volume
};

struct SunObstructionFrame {
    uint64_t generation = 0;
    std::string studyId;
    uint64_t snapshotId = 0;
    uint64_t captureStamp = 0;
    SunObstructionMode mode = SunObstructionMode::Off;
    int minute = -1; // WholeDay: -1; TimeStep: the measured clock minute
    GhPreviewDrawables drawables;
    size_t blockerObjects = 0, footprints = 0;
    std::vector<SunObstructionBlocker> blockers;                  // most hours first
    std::vector<std::string> blockerGuids;                        // for the scene's obstruction highlight
    size_t lit = 0, blocked = 0, backFacing = 0, unavailable = 0; // cell-steps
    // What was actually covered, said rather than implied (#167).
    size_t cells = 0, attributedSteps = 0;
    int fromMinute = 0, toMinute = 1440;   // the slice asked for
    int windowFrom = 0, windowTo = 1440;   // the run's measured hours
    int firstMinute = -1, lastMinute = -1; // first and last attributed step
    int timestepMinutes = 0;
    int year = 0, month = 0, day = 0;
    int footprintDivisions = 0;
    // SolarCut: the removed volume, the slice's hours at the point before and
    // after the cut (re-traced, not assumed), the elements cut, and how many
    // study cells the cut brings sun to in the slice.
    double removedVolume = 0.0;
    double facingHours = 0.0, litHours = 0.0, litAfterHours = 0.0;
    std::vector<SunSolarCutObject> cutObjects;
    size_t sectors = 0, wonCells = 0;
    SunSolarCutStage cutStage = SunSolarCutStage::Done;
    evp::sunstudy::SolarExposureResult exposure;
    // SolarCut with an exposure: the cut's own layers without it, so the view
    // can show one step's lit surface (the day diagram's clock) on the same cut
    // without tracing again. `drawables` stays the whole slice, hours-shaded.
    GhPreviewDrawables cutDrawables;
    // The study's cells the cut changes, re-counted over the whole day.
    evp::sunstudy::SolarCutCellsResult cutCells;
    std::shared_ptr<SunSolarCutProgress> progress;
    std::string error;
};

class SunObstructionController final {
  public:
    SunObstructionController ();
    ~SunObstructionController ();
    SunObstructionController (const SunObstructionController&) = delete;
    SunObstructionController& operator= (const SunObstructionController&) = delete;

    void Request (const SunObstructionRequest& request);
    std::shared_ptr<const SunObstructionFrame> Latest ();
    bool Busy () const;
    void Cancel (); // project/document transition; the controller remains reusable
    void Shutdown ();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

SunObstructionController& SunObstructionTasks ();
void ShutdownSunObstructionTasks ();
void ResetSunObstructionTasks ();
bool SunObstructionSnapshotCurrent (const SunObstructionFrame& frame,
                                    const std::shared_ptr<const geomsrv::Snapshot>& shared, uint64_t captureStamp);
// A historical study's exact parts must not be drawn over an edited scene.
bool SunObstructionFrameCurrent (const std::shared_ptr<const SunObstructionFrame>& frame,
                                 const std::string& shownStudyId);

} // namespace geomsrv::archviz

#endif
