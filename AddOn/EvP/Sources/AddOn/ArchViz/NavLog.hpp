#ifndef EVP_ARCHVIZ_NAVLOG_HPP
#define EVP_ARCHVIZ_NAVLOG_HPP

// Both cameras, one file, one clock — so they can be compared OFFLINE.
//
// The question this exists to answer is not "is our camera right" (the HUD says
// that) but "how does our camera move RELATIVE TO ARCHICAD'S". That is the whole
// registration problem for the overlay work (plan Part III), and it cannot be
// answered by looking at two screens: the two motions have to be lined up on one
// timeline, at one sample rate, in one set of units.
//
// ⚠️ CSV, ON PURPOSE, AND NOT THE PROSE LOG. `logs\archviz.log` is a narrative a
// human reads; this is a table a script reads. Mixing them means neither works:
// the interesting comparison here is a numeric diff over hundreds of rows, and
// the interesting content there is one line explaining a failure.
//
// ⚠️ IT ALSO MEASURES THE POLL ITSELF, and that is half its value. Plan §23's
// G9 and G10 are open questions — does `ACAPI_View_Get3DProjectionSets` change
// DURING an orbit drag, or only settle at mouse-up, and can it be polled fast
// enough? Every Archicad row carries the gap since the previous one, so a
// starved poll is visible as a gap rather than being mistaken for a camera that
// did not move. A run of this log is the cheapest possible answer to both.
//
// ⚠️ OPT-IN AND OFF BY DEFAULT. It writes a row per poll per source; leaving it
// on permanently would be an unbounded cost for a diagnostic nobody is reading.
// `EvP.ViewerNavLog {enable, intervalMs}` turns it on.
//
// THREAD SAFETY: `LogViewer` is called from the RENDER thread and `LogArchicad`
// from the MAIN thread, concurrently, by design — that is the whole point. The
// file is behind a mutex and the throttle is per-source.

#include <cstdint>
#include <string>

namespace geomsrv {
namespace archviz {
namespace navlog {

// Open a session: write a header block and start sampling. `intervalMs` is the
// minimum spacing between rows FROM EACH SOURCE — the render thread is asked
// 60+ times a second and the Archicad poller as often as Windows will deliver a
// timer, and neither should fill the file at that rate.
void Start (uint32_t intervalMs);
void Stop ();
bool     IsRunning ();
uint32_t IntervalMs ();

// ---- the viewer's camera. RENDER THREAD. Call every frame; it throttles. ----
// `nav` is "pan" / "orbit" / "-", so a row can be attributed to a gesture.
void LogViewer (const char* nav, const float eye[3], const float target[3],
                float distance, float yawDeg, float pitchDeg, float fovDeg);

// ---- Archicad's camera. MAIN THREAD ONLY (it comes from ACAPI). ------------
// `window` is the current window's kind, because plan §23's G11 says
// Get3DProjectionSets reads whatever window is front — a row logged while the
// floor plan is up describes the 3D window's SETTINGS, not what the user is
// looking at, and the two must be distinguishable afterwards.
void LogArchicadPersp (const char* window, const double pos[3], const double target[3],
                       double distance, double azimuthDeg, double rollDeg, double viewConeDeg);
// ---- what the viewer PRESENTED, on the plan. RENDER THREAD. ----------------
// ⚠️ THIS IS THE OTHER HALF OF AN OBJECTIVE DESYNC MEASUREMENT (PLAT-RE84), and
// until it existed there was no way to get one. Everything else in this file
// records what ARCHICAD was doing. Pairing that against what the overlay was
// actually showing, on one timeline, is what turns "it trails a bit" into a
// number -- and the number is computed offline by tools/navlog_report.py, which
// interpolates Archicad's camera to each presented frame's timestamp and reports
// the difference in PIXELS.
//
// ⚠️ `LogViewer` above has never been called by anything. It has been declared
// and defined since this file was written, so the comparison the whole file
// exists for has never once been performed. This is the plan-shaped version, and
// it IS called -- from the render thread, right after Present.
//
// `widthPx`/`heightPx` are the surface's, and they are what makes the result
// expressible in pixels rather than metres: a metre of error means nothing
// without knowing the zoom, and pixels are what the user actually sees.
void LogViewerPlan (const double centre[2], double halfHeightMetres, double rotationRadians,
                    uint32_t widthPx, uint32_t heightPx, uint64_t frameIndex);

// ---- what the viewer PRESENTED, in 3D. RENDER THREAD. ----------------------
// ⚠️ WITHOUT THIS THE 3D PATH CANNOT BE MEASURED AT ALL, and the matrix has been
// silently unable to score it: `LogPresentedPlanFrame` rejects a perspective
// camera outright, so a `path=3d` run recorded Archicad's camera and NOTHING to
// compare it against. Every 3D verdict has therefore been by eye only.
//
// The columns mirror `LogArchicadPersp` deliberately -- eye, target and a
// HORIZONTAL cone in degrees -- so the two streams are directly comparable.
// ⚠️ HORIZONTAL, because that is what Archicad's `viewCone` is and converting
// one of the pair rather than both is how a systematic error gets built in.
// `widthPx`/`heightPx` ride along because turning an angular error into pixels
// needs the surface, exactly as the plan path needs it to turn metres into them.
void LogViewerPersp (const double eye[3], const double target[3], double viewConeDegHorizontal,
                     uint32_t widthPx, uint32_t heightPx, uint64_t frameIndex);

// ---- Archicad's camera on the FLOOR PLAN. MAIN THREAD ONLY. ----------------
// ⚠️ WITHOUT THIS THE PLAN PATH HAS NO NAV LOG AT ALL. `LogArchicadPersp` above
// is fed from `Get3DProjectionSets`, which answers for the 3D window whatever is
// in front — so every plan-path run produced rows describing a window the user
// was not looking at, and the plan half of a sync comparison had no evidence
// behind it. The plan camera comes from a different place entirely
// (`ReadPlanViewCamera`'s three-corner `PointToCoord` fit), and this is where it
// lands.
//
// ⚠️ IT SHARES THE ARCHICAD THROTTLE AND GAP with the two above, deliberately:
// all three come from the SAME main-thread timer, so the gap between rows is one
// measurement of one poll regardless of which window answered it. Splitting the
// throttle would make a window switch look like a starved poll.
//
// A parallel top-down projection has no eye and no field of view, so those
// columns are zero; see the header block written by `Start` for which columns
// carry the plan's own two numbers.
//
// `readMicroseconds` is how long the ACAPI read that produced this row took.
// ⚠️ IT DISTINGUISHES THE TWO REASONS THE POLL IS SLOW, which no other column
// can. A 15 ms timer measured 21 ms between samples at idle even with
// timeBeginPeriod(1) held, and the missing 6 ms is either the read itself
// (fixable only by reading less) or the main thread being busy elsewhere
// (fixable only by getting scheduled sooner). Those have opposite remedies, so
// guessing between them would send the next rung the wrong way.
void LogArchicadPlan (const char* window, const double centre[2], double halfHeightMetres,
                      double rotationRadians, uint64_t readMicroseconds);

// ---- when a frame actually went out (PLAT-RE78) ----------------------------
// MAIN THREAD, flushed from the Present hook's ring -- never called from the
// detour itself, which must not touch a file.
//
// ⚠️ `timestampUs` IS ON THE QPC CLOCK, NOT THIS FILE'S. The detour records with
// QueryPerformanceCounter because it is measuring sub-millisecond spacing; every
// other row here is milliseconds since the session header. The rows are
// therefore comparable to EACH OTHER but must be aligned to the rest of the log
// by their own first sample, which is `navlog_report`'s job. Mixing the two
// clocks silently would put the frame clock at a plausible but wrong offset.
void LogPresent (uint64_t sessionMs, uint64_t swapChain, uint64_t timestampUs,
                 uint32_t syncInterval, bool ours);

// Milliseconds since this log session's header, on the SAME clock every other
// row uses. The Present hook needs it to place its QPC-stamped ring samples back
// onto the shared timeline; without that the frame clock is internally
// consistent but floats at an unknown offset against the cameras, which is
// exactly the comparison it was recorded for.
uint64_t SessionNowMs ();

// Axono carries no eye/target at all — only a 3x4 model->projected matrix. It is
// logged raw; reconstructing a camera from it is plan §24 block B's job, not
// this file's, and inventing one here would fabricate the very numbers the probe
// is supposed to measure.
void LogArchicadAxono (const char* window, double azimuthDeg, int32_t projMod,
                       const double tranmat12[12]);
// The read failed, or there is no 3D view yet. Logged rather than skipped: a
// missing row and a failed row look identical in a gap-based analysis.
void LogArchicadFailure (const char* window, const std::string& why);

// ---- cell delimiters for the camera-sync matrix (PLAT-RE73) ----------------
// One row, `source=mark`, carrying `label` in the last column. The matrix probe
// writes one before and one after each gesture so `tools/navlog_report.py` can
// slice the file per cell — otherwise a run is one undifferentiated stream and
// "the gap during a fast orbit" cannot be separated from "the gap while the user
// was reading the next instruction".
//
// ⚠️ NOT THROTTLED, NOT COUNTED, AND IT DOES NOT MOVE THE GAP CLOCK. Any of the
// three would corrupt what it exists to delimit; see the implementation.
// Callable from any thread, like the rest of this file.
void Mark (const std::string& label);

struct Stats {
    bool     running        = false;
    uint32_t intervalMs     = 0;
    uint64_t viewerRows     = 0;
    uint64_t archicadRows   = 0;
    uint64_t archicadFails  = 0;
    // ⚠️ THE GAP IS THE MEASUREMENT, not an error counter. A WM_TIMER is a
    // low-priority message: Windows generates it only when the queue is
    // otherwise empty, so during a fast drag inside Archicad it can be starved
    // for a long time. If this is large, G9 was not answered by this run and the
    // canvas-subclass path (plan §15.2) is the next thing to try.
    uint64_t maxArchicadGapMs = 0;
    // ⚠️ A RUN WITH WRITE FAILURES HAS NO DURABLE EVIDENCE BEHIND IT, and
    // used to report success anyway: pending rows were cleared before the
    // append was checked. Reported so a probe can refuse to quote numbers
    // from a file that was never written.
    uint64_t writeFailures = 0;
    uint64_t droppedRows = 0;
};
Stats GetStats ();

}   // namespace navlog
}   // namespace archviz
}   // namespace geomsrv

#endif
