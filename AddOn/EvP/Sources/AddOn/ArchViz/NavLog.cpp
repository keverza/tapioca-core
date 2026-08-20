#include "ArchViz/NavLog.hpp"
#include "ArchViz/ArchVizLog.hpp"   // ArchVizLog — the prose log, for lifecycle lines

#include "Python/PathUtils.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace navlog {

namespace {

std::mutex        gMutex;
std::atomic<bool> gRunning { false };
std::atomic<uint32_t> gIntervalMs { 50 };

Stats    gStats;
uint64_t gSessionStartMs   = 0;
uint64_t gLastViewerMs     = 0;
uint64_t gLastArchicadMs   = 0;

uint64_t NowMs ()
{
    using namespace std::chrono;
    return uint64_t (duration_cast<milliseconds> (steady_clock::now ().time_since_epoch ()).count ());
}

GS::UniString NavLogPath ()
{
    const GS::UniString dataDir = evp::EvpDataDir ();
    if (dataDir.IsEmpty ())
        return GS::UniString ();
    return dataDir + GS::UniString ("\\logs\\archviz_nav.log");
}

// ⚠️ BUFFERED, AND THAT IS A CORRECTNESS FIX RATHER THAN AN OPTIMISATION.
//
// This used to write every row straight through: `EvpDataDir()` (an environment
// read plus a legacy-root migration check), `CreateDirectoryChain` (a
// CreateDirectoryW syscall) and `AppendTextLine` (CreateFileW + write +
// CloseHandle) -- PER ROW, on the MAIN THREAD, from inside the very poll whose
// spacing this file exists to measure. Two sources at ~32 Hz meant roughly 64
// open/close cycles a second on Archicad's UI thread.
//
// The first live run (2026-08-13) reported exactly what that predicts: Archicad's
// OWN zoom stopped being smooth while the log was on, and it was noticeably worse
// than the same overlay with the log off. So the instrument was degrading the
// thing under test AND inflating its own numbers -- a gap that includes this
// file's I/O is not a measurement of the poll.
//
// The path is resolved ONCE in Start. Rows accumulate in memory and go out in
// batches.
//
// ⚠️ THE TRADE IS EXPLICIT: up to kFlushRows rows are lost if Archicad dies
// mid-run. At ~32 Hz that is about eight seconds. This log is a diagnostic whose
// runs end in an orderly Stop, and a crash during one is a different
// investigation -- whereas perturbing every sample was corrupting every run.
constexpr size_t kFlushRows = 256;

// A ceiling on what a failing disk may accumulate before rows are admitted
// lost. Reported, never silent.
constexpr size_t kMaxPendingRows = 20000;

GS::UniString    gPath;          // resolved once, in Start
std::vector<std::string> gPending;

// gMutex MUST be held.
void FlushLocked ()
{
    if (gPending.empty () || gPath.IsEmpty ())
        return;
    // One string, one open. AppendTextLine terminates each call's text, so the
    // rows are joined here and handed over as a single block.
    std::string block;
    size_t total = 0;
    for (const std::string& row : gPending)
        total += row.size () + 2;
    block.reserve (total);
    for (const std::string& row : gPending) {
        block += row;
        block += "\r\n";
    }
    // ⚠️ THE ROWS ARE DROPPED ONLY IF THE WRITE SUCCEEDED. They used to be
    // cleared first, so a failed append lost them silently and the run reported
    // healthy sampling with no durable file behind it -- the worst possible
    // outcome for a diagnostic, because the numbers were quoted from a report
    // that had never been written.
    if (!evp::AppendTextLine (gPath, GS::UniString (block.c_str (), CC_UTF8))) {
        ++gStats.writeFailures;
        // Keep the rows and try again on the next flush, but do not let a
        // permanently failing disk grow the buffer without bound.
        if (gPending.size () > kMaxPendingRows) {
            gStats.droppedRows += gPending.size ();
            gPending.clear ();
        }
        return;
    }
    gPending.clear ();
}

// One row in. gMutex MUST be held.
//
// ⚠️ RUNNING IS RE-CHECKED HERE, UNDER THE LOCK. Every caller tests `gRunning`
// before taking the mutex, so a writer could pass that test, block on the lock
// while Stop() ran, and then append AFTER the session footer -- rows landing
// past the end of their own session, to be attributed to the next one or lost.
// The cheap test outside stays (it keeps the log free when nobody is measuring);
// this is the one that is actually authoritative.
// ⚠️ NOT `gRunning`. Stop() clears that FIRST, to stop new writers re-entering,
// and only then writes the session footer -- so testing gRunning here would drop
// Stop's own last rows. This flag is owned by the mutex and is cleared at the
// very end of Stop, after the footer and the final flush.
bool gSessionOpen = false;

void WriteLocked (const std::string& row)
{
    if (gPath.IsEmpty () || !gSessionOpen)
        return;
    gPending.push_back (row);
    if (gPending.size () >= kFlushRows)
        FlushLocked ();
}

// Milliseconds since this session's first row. ⚠️ RELATIVE, not wall clock: the
// entire point is lining two streams up against each other, and a relative
// origin makes a diff readable without date arithmetic. The absolute start is in
// the header for anyone who needs to correlate with archviz.log.
uint64_t SessionMs (uint64_t nowMs)
{
    return (gSessionStartMs == 0 || nowMs < gSessionStartMs) ? 0 : (nowMs - gSessionStartMs);
}

// A row is emitted only if `intervalMs` has passed for THAT source. Returns 0 if
// the row should be dropped, otherwise the gap since the source's last row.
// gMutex MUST be held.
uint64_t Throttle (uint64_t& lastMs, uint64_t nowMs)
{
    const uint32_t interval = gIntervalMs.load ();
    if (lastMs != 0 && (nowMs - lastMs) < interval)
        return 0;
    const uint64_t gap = (lastMs == 0) ? 0 : (nowMs - lastMs);
    lastMs = nowMs;
    return (gap == 0) ? 1 : gap;   // 1 rather than 0 so "emit" is never ambiguous
}

}   // namespace

void Start (uint32_t intervalMs)
{
    std::lock_guard<std::mutex> lock (gMutex);

    // Resolved here and nowhere else -- see WriteLocked for why this must not
    // happen per row. The directory is created once, for the same reason.
    gPath = NavLogPath ();
    if (!gPath.IsEmpty ())
        evp::CreateDirectoryChain (evp::EvpDataDir () + GS::UniString ("\\logs"));
    gPending.clear ();

    // ⚠️ 0 MEANS NO THROTTLE, and it has to survive the floor. The floor exists to
    // stop a careless 1 ms from filling the file, but silently promoting 0 to 5
    // would re-create the aliasing this exists to avoid: a caller asking for
    // every sample would still lose the ones arriving under 5 ms apart, and
    // nothing would say so.
    gIntervalMs.store ((intervalMs == 0 || intervalMs >= 5) ? intervalMs : 5);
    gSessionStartMs = NowMs ();
    gLastViewerMs   = 0;
    gLastArchicadMs = 0;
    gStats          = Stats {};
    gStats.running    = true;
    gStats.intervalMs = gIntervalMs.load ();
    gSessionOpen = true;
    gRunning.store (true);

    WriteLocked ("");
    WriteLocked ("# ---- ArchViz navigation comparison, new session ----");
    WriteLocked ("# Two cameras on ONE timeline. `t_ms` is milliseconds since this");
    WriteLocked ("# header; `gap_ms` is the time since the PREVIOUS row FROM THE SAME");
    WriteLocked ("# SOURCE, which is how a starved poll is told apart from a camera");
    WriteLocked ("# that did not move (plan G9/G10).");
    WriteLocked ("# source=viewer : our bgfx camera, render thread, Z-up metres.");
    WriteLocked ("# source=archicad : ACAPI_View_Get3DProjectionSets, main thread.");
    WriteLocked ("#   mode=persp -> eye/target/dist/azimuth/roll/viewCone are filled.");
    WriteLocked ("#   mode=axono -> ONLY azimuth + the raw 3x4 tranmat; Archicad gives");
    WriteLocked ("#     no eye or target for a parallel projection and this file will");
    WriteLocked ("#     not invent one. See plan §24 block B.");
    WriteLocked ("#   mode=plan  -> the FLOOR PLAN's own camera, from the three-corner");
    WriteLocked ("#     PointToCoord fit, not from Get3DProjectionSets. A parallel");
    WriteLocked ("#     top-down projection has no eye and no field of view, so those");
    WriteLocked ("#     columns are 0 and the plan's two numbers reuse the nearest");
    WriteLocked ("#     columns instead: dist = ORTHO HALF-HEIGHT IN METRES (the zoom),");
    WriteLocked ("#     azimuth_deg = the plan's ROTATION. tgtX/tgtY = the model point");
    WriteLocked ("#     under the window's centre.");
    WriteLocked ("#   window= tells you which window was FRONT. Get3DProjectionSets");
    WriteLocked ("#     reads the 3D settings whatever is front (plan G11), so a row");
    WriteLocked ("#     taken over the floor plan describes settings, not the view.");
    WriteLocked ("# mark rows (source=mark) delimit one matrix cell; `extra` names it.");
    // ⚠️ THE THROTTLE IS RECORDED IN THE FILE, so a reader can tell a starved
    // poll from a decimated LOG. A run throttled at the poll interval drops every
    // sample that lands fractionally early and the resulting rate reads as a
    // timer that missed -- which is exactly how two 2026-08-13 runs came to be
    // analysed as "the poll is starved" when part of it was this file. A number
    // the report can check beats a rule someone has to remember.
    WriteLocked ("# throttle_ms=" + std::to_string (gIntervalMs.load ()));
    WriteLocked ("t_ms,gap_ms,source,window,mode,eyeX,eyeY,eyeZ,tgtX,tgtY,tgtZ,"
                 "dist,azimuth_deg,roll_or_pitch_deg,cone_or_fov_deg,extra");

    ArchVizLog ("nav log: started at " + std::to_string (gIntervalMs.load ()) +
                " ms per source -> logs\\archviz_nav.log");
}

void Stop ()
{
    if (!gRunning.exchange (false))
        return;
    std::lock_guard<std::mutex> lock (gMutex);
    gStats.running = false;
    WriteLocked ("# ---- session end: " + std::to_string (gStats.viewerRows) + " viewer rows, " +
                 std::to_string (gStats.archicadRows) + " archicad rows, " +
                 std::to_string (gStats.archicadFails) + " failures, max archicad gap " +
                 std::to_string (gStats.maxArchicadGapMs) + " ms ----");
    FlushLocked ();   // ⚠️ THE ONLY GUARANTEED FLUSH. Nothing reads the file until Stop.
    gSessionOpen = false;   // after the footer and the flush, never before
    if (gStats.writeFailures > 0)
        ArchVizLog ("nav log: " + std::to_string (gStats.writeFailures) +
                    " WRITE FAILURE(S) and " + std::to_string (gStats.droppedRows) +
                    " row(s) lost -- the file on disk is INCOMPLETE");
    ArchVizLog ("nav log: stopped");
}

bool     IsRunning ()  { return gRunning.load (); }
uint32_t IntervalMs () { return gIntervalMs.load (); }

Stats GetStats ()
{
    std::lock_guard<std::mutex> lock (gMutex);
    Stats out = gStats;
    out.running = gRunning.load ();
    return out;
}

void LogViewer (const char* nav, const float eye[3], const float target[3],
                float distance, float yawDeg, float pitchDeg, float fovDeg)
{
    if (!gRunning.load ())
        return;

    const uint64_t now = NowMs ();
    std::lock_guard<std::mutex> lock (gMutex);
    const uint64_t gap = Throttle (gLastViewerMs, now);
    if (gap == 0)
        return;

    char buf[512] = {};
    std::snprintf (buf, sizeof (buf),
                   "%llu,%llu,viewer,-,%s,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,",
                   (unsigned long long) SessionMs (now), (unsigned long long) gap,
                   nav != nullptr ? nav : "-",
                   eye[0], eye[1], eye[2], target[0], target[1], target[2],
                   distance, yawDeg, pitchDeg, fovDeg);
    WriteLocked (buf);
    ++gStats.viewerRows;
}

void LogViewerPersp (const double eye[3], const double target[3],
                     double viewConeDegHorizontal, uint32_t widthPx, uint32_t heightPx,
                     uint64_t frameIndex)
{
    if (!gRunning.load ())
        return;

    const uint64_t now = NowMs ();
    std::lock_guard<std::mutex> lock (gMutex);
    const uint64_t gap = Throttle (gLastViewerMs, now);
    if (gap == 0)
        return;

    char buf[512] = {};
    std::snprintf (buf, sizeof (buf),
                   "%llu,%llu,viewer,3D,persp,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,0,0,0,%.3f,"
                   "h=%u;w=%u;frame=%llu",
                   (unsigned long long) SessionMs (now), (unsigned long long) gap,
                   eye[0], eye[1], eye[2], target[0], target[1], target[2],
                   viewConeDegHorizontal, heightPx, widthPx,
                   (unsigned long long) frameIndex);
    WriteLocked (buf);
    ++gStats.viewerRows;
}

void LogArchicadPersp (const char* window, const double pos[3], const double target[3],
                       double distance, double azimuthDeg, double rollDeg, double viewConeDeg)
{
    if (!gRunning.load ())
        return;

    const uint64_t now = NowMs ();
    std::lock_guard<std::mutex> lock (gMutex);
    const uint64_t gap = Throttle (gLastArchicadMs, now);
    if (gap == 0)
        return;
    if (gap > gStats.maxArchicadGapMs)
        gStats.maxArchicadGapMs = gap;

    char buf[512] = {};
    std::snprintf (buf, sizeof (buf),
                   "%llu,%llu,archicad,%s,persp,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,",
                   (unsigned long long) SessionMs (now), (unsigned long long) gap,
                   window != nullptr ? window : "?",
                   pos[0], pos[1], pos[2], target[0], target[1], target[2],
                   distance, azimuthDeg, rollDeg, viewConeDeg);
    WriteLocked (buf);
    ++gStats.archicadRows;
}

void LogViewerPlan (const double centre[2], double halfHeightMetres, double rotationRadians,
                    uint32_t widthPx, uint32_t heightPx, uint64_t frameIndex)
{
    if (!gRunning.load ())
        return;

    const uint64_t now = NowMs ();
    std::lock_guard<std::mutex> lock (gMutex);
    const uint64_t gap = Throttle (gLastViewerMs, now);
    if (gap == 0)
        return;

    constexpr double kRadToDeg = 57.29577951308232;
    // ⚠️ THE SURFACE HEIGHT GOES IN `extra`, and it is not decoration: it is what
    // converts a model-metre error into the PIXELS the user actually sees, and
    // the surface can be resized mid-run. Reading it from the row rather than
    // assuming a constant is the difference between a number and a guess.
    char buf[512] = {};
    std::snprintf (buf, sizeof (buf),
                   "%llu,%llu,viewer,FloorPlan,plan,0,0,0,%.4f,%.4f,0,%.4f,%.3f,0,0,"
                   "h=%u;w=%u;frame=%llu",
                   (unsigned long long) SessionMs (now), (unsigned long long) gap,
                   centre[0], centre[1], halfHeightMetres, rotationRadians * kRadToDeg,
                   (unsigned) heightPx, (unsigned) widthPx,
                   (unsigned long long) frameIndex);
    WriteLocked (buf);
    ++gStats.viewerRows;
}

void LogArchicadPlan (const char* window, const double centre[2], double halfHeightMetres,
                      double rotationRadians, uint64_t readMicroseconds)
{
    if (!gRunning.load ())
        return;

    const uint64_t now = NowMs ();
    std::lock_guard<std::mutex> lock (gMutex);
    // Same throttle and same gap as the perspective rows -- see the header's note
    // for why one main-thread poll must not look starved just because the front
    // window changed.
    const uint64_t gap = Throttle (gLastArchicadMs, now);
    if (gap == 0)
        return;
    if (gap > gStats.maxArchicadGapMs)
        gStats.maxArchicadGapMs = gap;

    constexpr double kRadToDeg = 57.29577951308232;
    char buf[512] = {};
    std::snprintf (buf, sizeof (buf),
                   "%llu,%llu,archicad,%s,plan,0,0,0,%.4f,%.4f,0,%.4f,%.3f,0,0,read_us=%llu",
                   (unsigned long long) SessionMs (now), (unsigned long long) gap,
                   window != nullptr ? window : "?",
                   centre[0], centre[1], halfHeightMetres, rotationRadians * kRadToDeg,
                   (unsigned long long) readMicroseconds);
    WriteLocked (buf);
    ++gStats.archicadRows;
}

uint64_t SessionNowMs ()
{
    return SessionMs (NowMs ());
}

void LogPresent (uint64_t sessionMs, uint64_t swapChain, uint64_t timestampUs,
                 uint32_t syncInterval, bool ours)
{
    if (!gRunning.load ())
        return;

    std::lock_guard<std::mutex> lock (gMutex);
    // ⚠️ NOT THROTTLED. Present rows ARE the frame clock; dropping some would
    // turn "Archicad presented at 60 Hz" into "Archicad presented at 30 Hz" with
    // nothing to show which. They arrive in one flushed batch from a ring, not
    // continuously, so there is no per-row cost to control here.
    char buf[256] = {};
    std::snprintf (buf, sizeof (buf),
                   "%llu,0,present,-,frame,0,0,0,0,0,0,0,0,0,0,chain=%llu;qpc_us=%llu;sync=%u;ours=%d",
                   (unsigned long long) sessionMs,
                   (unsigned long long) swapChain, (unsigned long long) timestampUs,
                   (unsigned) syncInterval, ours ? 1 : 0);
    WriteLocked (buf);
}

void Mark (const std::string& label)
{
    if (!gRunning.load ())
        return;

    const uint64_t now = NowMs ();
    std::lock_guard<std::mutex> lock (gMutex);
    // ⚠️ NOT THROTTLED AND NOT COUNTED. A mark is a delimiter, not a sample: the
    // throttle exists to stop 60 Hz sources filling the file, and dropping the
    // one row that says where a cell began would silently merge two cells into
    // one in the report. It also must not move gLastArchicadMs, or the first
    // real row of a cell would show a fabricated gap.
    char buf[128] = {};
    std::snprintf (buf, sizeof (buf), "%llu,0,mark,-,-,0,0,0,0,0,0,0,0,0,0,",
                   (unsigned long long) SessionMs (now));
    // The label goes in `extra`, the last column, so a comma in it cannot shift
    // any other field -- but it would still split the label itself, so strip.
    std::string safe = label;
    for (char& c : safe) {
        if (c == ',' || c == '\n' || c == '\r')
            c = ';';
    }
    WriteLocked (std::string (buf) + safe);
}

void LogArchicadAxono (const char* window, double azimuthDeg, int32_t projMod,
                       const double tranmat12[12])
{
    if (!gRunning.load ())
        return;

    const uint64_t now = NowMs ();
    std::lock_guard<std::mutex> lock (gMutex);
    const uint64_t gap = Throttle (gLastArchicadMs, now);
    if (gap == 0)
        return;
    if (gap > gStats.maxArchicadGapMs)
        gStats.maxArchicadGapMs = gap;

    // The eye/target/dist columns stay EMPTY rather than being filled with
    // zeroes: an empty cell reads as "not applicable" in any tool, a zero reads
    // as a camera at the origin and would silently poison an offline diff.
    std::string row = std::to_string (SessionMs (now)) + "," + std::to_string (gap) +
                      ",archicad," + (window != nullptr ? window : "?") +
                      ",axono,,,,,,,," + std::to_string (azimuthDeg) + ",,,";
    row += "projMod=" + std::to_string (projMod) + " tranmat=";
    for (int i = 0; i < 12; ++i) {
        char cell[32] = {};
        std::snprintf (cell, sizeof (cell), "%.6f", tranmat12[i]);
        row += cell;
        if (i < 11)
            row += " ";
    }
    WriteLocked (row);
    ++gStats.archicadRows;
}

void LogArchicadFailure (const char* window, const std::string& why)
{
    if (!gRunning.load ())
        return;

    const uint64_t now = NowMs ();
    std::lock_guard<std::mutex> lock (gMutex);
    const uint64_t gap = Throttle (gLastArchicadMs, now);
    if (gap == 0)
        return;

    WriteLocked (std::to_string (SessionMs (now)) + "," + std::to_string (gap) +
                 ",archicad," + (window != nullptr ? window : "?") + ",FAILED,,,,,,,,,,," + why);
    ++gStats.archicadFails;
}

}   // namespace navlog
}   // namespace archviz
}   // namespace geomsrv
