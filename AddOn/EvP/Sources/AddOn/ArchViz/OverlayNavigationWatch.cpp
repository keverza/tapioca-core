// ArchViz/OverlayNavigationWatch -- see the header.
//
// Overlay contract: private/docs/architecture/diligent/OVERLAY-INVARIANTS.md (§7).

#include "ArchViz/OverlayNavigationWatch.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/OverlayInputEvents.hpp" // windows.h, NOMINMAX first
#include "ArchViz/Dxgi/CameraFreshness.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace geomsrv {
namespace archviz {
namespace navwatch {

namespace {

constexpr uint64_t kReportMs = 5000;
constexpr uint64_t kWheelWindowMs = 300;
// Farther than this from Archicad's last move, a navigation starts somewhere Archicad did not
// see the pointer go.
constexpr double kFarPixels = 8.0;

struct Span {
    uint32_t count = 0;
    uint64_t sumMicros = 0;
    uint32_t maxMicros = 0;

    void Add (uint32_t micros)
    {
        ++count;
        sumMicros += micros;
        maxMicros = (std::max) (maxMicros, micros);
    }
};

struct Window {
    uint32_t presses = 0;
    uint32_t pressesOnLocked = 0;
    uint32_t pressesFar = 0;
    double worstPixels = 0.0;
    uint64_t worstAgeMs = 0;
    uint32_t movesPassed = 0;
    uint32_t movesEaten = 0;
    uint32_t movesEatenFromHost = 0;
    uint32_t wheels = 0;
    uint32_t redraws = 0;
    uint32_t redrawsAfterWheel = 0;
    Span heartbeat;
    Span heartbeatNavigating;
    Span refresh;
    Span refreshNavigating;
    uint32_t keysTaken = 0;
    uint32_t modifierRepeatsTaken = 0;
    uint32_t keysTakenNavigating = 0;
    uint32_t middleDoubleClicks = 0;
};

Window g_window;
uint64_t g_lastReport = 0;
bool g_havePassedMove = false;
long g_passedX = 0;
long g_passedY = 0;
uint64_t g_passedAt = 0;
uint64_t g_lastWheel = 0;
uint64_t g_lastNavigationStart = 0;
uint64_t g_pressGapMs = 0; // between the last two navigations begun

// ---- one gesture of Archicad's, and what the camera did in it ------------------------------
// A gesture runs from a middle press Archicad was given to its release; the camera samples
// arrive up to a heartbeat later, so a closed gesture stays open to them for kTailMs.
constexpr uint64_t kTailMs = 400;
struct Gesture {
    bool used = false;
    bool open = false;
    bool reported = false;
    uint64_t pressMs = 0;
    uint64_t releaseMs = 0;
    bool shiftAtPress = false;
    double pressPixels = -1.0; // from Archicad's last move; -1: no move passed yet
    bool haveStartView = false;
    float startView[16] = {}; // the last decode before the press
    uint32_t movesPassed = 0;
    uint32_t movesEaten = 0;
    uint64_t lastPassedMs = 0;
    uint64_t worstStallMs = 0;
    uint32_t keysTaken = 0;
    uint32_t steps = 0;
    double turned = 0.0;
    double maxTurn = 0.0;
    double travel = 0.0;
    double maxTravel = 0.0;
    uint32_t jumps = 0;
};
Gesture g_current;  // the newest gesture, open or closed
Gesture g_previous; // the one before it, until said

// The rates of recent moving steps: the median a jump is measured against.
constexpr double kJumpDegrees = 30.0;      // anywhere: above every in-motion step measured
constexpr double kStillJumpDegrees = 10.0; // where nothing of the user's was turning the camera
constexpr uint64_t kPressSettleMs = 60;    // a press's first ms: no motion of its own yet
bool g_haveLast = false;
uint64_t g_lastRedrawMs = 0;
uint64_t g_lastShiftUpTakenMs = 0;
RedrawSource g_lastRedrawSource = RedrawSource::Gate;
dxgi::injection::freshness::CameraSample g_last;

uint64_t Now ()
{
    return ::GetTickCount64 ();
}

bool Navigating ()
{
    return (overlayinput::events::PhysicalButtons () & overlayinput::Bit (overlayinput::Button::Middle)) != 0;
}

std::string Describe (const Span& span)
{
    char text[64] = {};
    if (span.count == 0)
        _snprintf_s (text, sizeof (text), _TRUNCATE, "n=0");
    else
        _snprintf_s (text, sizeof (text), _TRUNCATE, "n=%u mean %.1f max %.1f ms", span.count,
                     double (span.sumMicros) / span.count / 1000.0, span.maxMicros / 1000.0);
    return text;
}

// Local wall-clock time of a GetTickCount64 reading, for lines a user's notes can be matched to.
std::string Clock (uint64_t ms)
{
    SYSTEMTIME local = {};
    ::GetLocalTime (&local);
    const int64_t today =
        ((int64_t (local.wHour) * 60 + local.wMinute) * 60 + local.wSecond) * 1000 + local.wMilliseconds;
    int64_t at = today - int64_t (Now () - ms);
    at = ((at % 86400000) + 86400000) % 86400000;
    char text[16] = {};
    _snprintf_s (text, sizeof (text), _TRUNCATE, "%02d:%02d:%02d.%03d", int (at / 3600000), int (at / 60000 % 60),
                 int (at / 1000 % 60), int (at % 1000));
    return text;
}

Gesture* GestureAt (uint64_t ms)
{
    for (Gesture* g : { &g_current, &g_previous })
        if (g->used && ms >= g->pressMs && (g->open || ms <= g->releaseMs + kTailMs))
            return g;
    return nullptr;
}

void SayGesture (Gesture& g)
{
    if (!g.used || g.reported || g.open)
        return;
    g.reported = true;
    char pixels[32] = "no move yet";
    if (g.pressPixels >= 0.0)
        _snprintf_s (pixels, sizeof (pixels), _TRUNCATE, "%.0f px", g.pressPixels);
    char line[512] = {};
    _snprintf_s (line, sizeof (line), _TRUNCATE,
                 "%-12s %s-%s (%llu ms), Shift %s at press, press %s from Archicad's last move | moves passed %u "
                 "eaten %u, longest gap between passed moves %llu ms | keys the HUD took %u | camera: %u steps, "
                 "turned %.1f deg (largest step %.1f), moved %.2f (largest step %.2f) | jumps %u",
                 "NAVGESTURE", Clock (g.pressMs).c_str (), Clock (g.releaseMs).c_str (),
                 (unsigned long long) (g.releaseMs - g.pressMs), g.shiftAtPress ? "held" : "not held", pixels,
                 g.movesPassed, g.movesEaten, (unsigned long long) g.worstStallMs, g.keysTaken, g.steps, g.turned,
                 g.maxTurn, g.travel, g.maxTravel, g.jumps);
    ArchVizLog (line);
}

const char* NameOf (RedrawSource source)
{
    return source == RedrawSource::AfterNavigation ? "after a navigation"
           : source == RedrawSource::Budget        ? "cold-start budget"
                                                   : "layer or HUD change";
}

void SayJump (const CameraStep& step, const float* view, uint64_t ms, uint64_t dt)
{
    const Gesture* g = GestureAt (ms);
    // The gesture last begun before this decode: is the new view the one it started from?
    const Gesture* begun = g;
    for (const Gesture* candidate : { &g_current, &g_previous })
        if (begun == nullptr && candidate->used && candidate->pressMs <= ms)
            begun = candidate;
    char undo[96] = "no gesture's starting view";
    if (begun != nullptr && begun->haveStartView)
        _snprintf_s (undo, sizeof (undo), _TRUNCATE, "%.1f deg from the view before that gesture's press",
                     Step (begun->startView, view).degrees);
    char shift[64] = "no Shift key-up taken";
    if (g_lastShiftUpTakenMs != 0 && g_lastShiftUpTakenMs <= ms && ms - g_lastShiftUpTakenMs <= 2000)
        _snprintf_s (shift, sizeof (shift), _TRUNCATE, "a Shift key-up taken by the HUD %llu ms before",
                     (unsigned long long) (ms - g_lastShiftUpTakenMs));
    char where[256] = {};
    if (g == nullptr)
        _snprintf_s (where, sizeof (where), _TRUNCATE, "outside any gesture");
    else if (g->open || ms <= g->releaseMs)
        _snprintf_s (where, sizeof (where), _TRUNCATE,
                     "in a gesture, %llu ms after its press (Shift %s at press, %u moves passed, %u keys taken, "
                     "longest gap between passed moves %llu ms)",
                     (unsigned long long) (ms - g->pressMs), g->shiftAtPress ? "held" : "not held", g->movesPassed,
                     g->keysTaken, (unsigned long long) g->worstStallMs);
    else
        _snprintf_s (where, sizeof (where), _TRUNCATE, "%llu ms after a gesture's release",
                     (unsigned long long) (ms - g->releaseMs));
    char wheel[48] = {};
    if (g_lastWheel != 0 && g_lastWheel <= ms && ms - g_lastWheel <= 1000)
        _snprintf_s (wheel, sizeof (wheel), _TRUNCATE, ", a wheel step %llu ms before",
                     (unsigned long long) (ms - g_lastWheel));
    char redraw[96] = "no 3D redraw of ours yet";
    if (g_lastRedrawMs != 0 && g_lastRedrawMs <= ms)
        _snprintf_s (redraw, sizeof (redraw), _TRUNCATE, "our last 3D redraw %llu ms before (%s)",
                     (unsigned long long) (ms - g_lastRedrawMs), NameOf (g_lastRedrawSource));
    else if (g_lastRedrawMs != 0)
        _snprintf_s (redraw, sizeof (redraw), _TRUNCATE, "our last 3D redraw %llu ms AFTER the decode (%s)",
                     (unsigned long long) (g_lastRedrawMs - ms), NameOf (g_lastRedrawSource));
    char line[512] = {};
    _snprintf_s (line, sizeof (line), _TRUNCATE,
                 "%-12s %s: the camera turned %.1f deg and moved %.2f between two decodes %llu ms apart | %s%s | "
                 "%s | %s | %s",
                 "NAVJUMP", Clock (ms).c_str (), step.degrees, step.travel, (unsigned long long) dt, where, wheel,
                 redraw, undo, shift);
    ArchVizLog (line);
}

// Every camera decode since the last heartbeat, step by step.
void AnalyseCamera ()
{
    namespace fresh = dxgi::injection::freshness;
    static fresh::CameraSample samples[fresh::kCameraSampleRing];
    uint64_t dropped = 0;
    const size_t count = fresh::TakeCameraSamples (samples, fresh::kCameraSampleRing, dropped);
    for (size_t i = 0; i < count; ++i) {
        const fresh::CameraSample& sample = samples[i];
        if (!g_haveLast || sample.ms < g_last.ms) {
            g_last = sample, g_haveLast = true;
            continue;
        }
        const CameraStep step = Step (g_last.view, sample.view);
        const uint64_t dt = (std::max) (sample.ms - g_last.ms, uint64_t (16)); // the tick's resolution
        g_last = sample;
        if (step.degrees < 1e-4 && step.travel < 1e-6)
            continue; // the same camera again
        Gesture* const at = GestureAt (sample.ms);
        const bool moving =
            at != nullptr && sample.ms >= at->pressMs + kPressSettleMs && (at->open || sample.ms <= at->releaseMs);
        const bool wheeling = g_lastWheel != 0 && g_lastWheel <= sample.ms && sample.ms - g_lastWheel <= 300;
        const bool jump = step.degrees >= kJumpDegrees || (!moving && !wheeling && step.degrees >= kStillJumpDegrees);
        if (Gesture* g = at) {
            ++g->steps;
            g->turned += step.degrees;
            g->maxTurn = (std::max) (g->maxTurn, step.degrees);
            g->travel += step.travel;
            g->maxTravel = (std::max) (g->maxTravel, step.travel);
            g->jumps += jump ? 1 : 0;
        }
        if (jump)
            SayJump (step, sample.view, sample.ms, dt);
    }
    if (dropped != 0)
        ArchVizLog ("NAVJUMP      " + std::to_string (dropped) + " camera samples dropped: the ring was full");
    // A closed gesture is said once its camera samples are in.
    for (Gesture* g : { &g_previous, &g_current })
        if (g->used && !g->open && Now () > g->releaseMs + kTailMs)
            SayGesture (*g);
}

void ReportIfDue ()
{
    const uint64_t now = Now ();
    if (g_lastReport == 0)
        g_lastReport = now;
    if (now - g_lastReport < kReportMs)
        return;
    g_lastReport = now;
    const Window w = g_window;
    g_window = Window {};
    // Nothing navigated, nothing laid out: a still view is not reported (§7, heartbeat lines
    // already say the overlay is alive).
    if (w.presses == 0 && w.wheels == 0 && w.movesEatenFromHost == 0 && w.refresh.count == 0 &&
        w.heartbeatNavigating.count == 0 && w.keysTaken == 0)
        return;
    char line[768] = {};
    _snprintf_s (line, sizeof (line), _TRUNCATE,
                 "%-12s ~5 s: navigations +%u (locked +%u; Archicad's last move >%.0f px away +%u, worst %.0f px "
                 "%llu ms old) | moves passed +%u eaten +%u (in Archicad's gesture after release +%u) | wheel +%u, "
                 "3D redraws +%u (within %llu ms of a wheel +%u) | main thread: heartbeat %s, navigating %s; HUD "
                 "layout %s, navigating %s | keys the HUD took +%u (modifier auto-repeats +%u, middle held +%u), "
                 "middle double-clicks passed +%u",
                 "NAVWATCH", w.presses, w.pressesOnLocked, kFarPixels, w.pressesFar, w.worstPixels,
                 (unsigned long long) w.worstAgeMs, w.movesPassed, w.movesEaten, w.movesEatenFromHost, w.wheels,
                 w.redraws, (unsigned long long) kWheelWindowMs, w.redrawsAfterWheel, Describe (w.heartbeat).c_str (),
                 Describe (w.heartbeatNavigating).c_str (), Describe (w.refresh).c_str (),
                 Describe (w.refreshNavigating).c_str (), w.keysTaken, w.modifierRepeatsTaken, w.keysTakenNavigating,
                 w.middleDoubleClicks);
    ArchVizLog (line);
}

} // namespace

void NoteMove (long x, long y, bool passed, bool eatenFromHostGesture)
{
    if (g_current.open) {
        const uint64_t now = Now ();
        if (passed && g_current.lastPassedMs != 0)
            g_current.worstStallMs = (std::max) (g_current.worstStallMs, now - g_current.lastPassedMs);
        (passed ? g_current.movesPassed : g_current.movesEaten) += 1;
        g_current.lastPassedMs = passed ? now : g_current.lastPassedMs;
    }
    if (passed) {
        ++g_window.movesPassed;
        g_havePassedMove = true;
        g_passedX = x;
        g_passedY = y;
        g_passedAt = Now ();
        return;
    }
    ++g_window.movesEaten;
    g_window.movesEatenFromHost += eatenFromHostGesture ? 1 : 0;
}

void NoteNavigationStart (long x, long y, bool locked)
{
    ++g_window.presses;
    g_window.pressesOnLocked += locked ? 1 : 0;
    g_pressGapMs = g_lastNavigationStart != 0 ? Now () - g_lastNavigationStart : 0;
    g_lastNavigationStart = Now ();
    if (g_current.open) { // a press with the last release unseen: closed here
        g_current.open = false;
        g_current.releaseMs = Now ();
    }
    SayGesture (g_previous); // a third gesture before its samples were in: said as it stands
    g_previous = g_current;
    g_current = Gesture {};
    g_current.used = g_current.open = true;
    g_current.pressMs = Now ();
    g_current.shiftAtPress = ::GetAsyncKeyState (VK_SHIFT) < 0;
    if (g_haveLast) { // the camera at rest before the press: the gesture's starting view
        g_current.haveStartView = true;
        std::copy (g_last.view, g_last.view + 16, g_current.startView);
    }
    if (!g_havePassedMove)
        return;
    const double pixels = std::hypot (double (x - g_passedX), double (y - g_passedY));
    g_current.pressPixels = pixels;
    if (pixels <= kFarPixels)
        return;
    ++g_window.pressesFar;
    if (pixels > g_window.worstPixels) {
        g_window.worstPixels = pixels;
        g_window.worstAgeMs = Now () - g_passedAt;
    }
}

void NoteNavigationEnd ()
{
    if (!g_current.open)
        return;
    g_current.open = false;
    g_current.releaseMs = Now ();
}

void NoteWheel ()
{
    ++g_window.wheels;
    g_lastWheel = Now ();
}

void NoteRedraw (RedrawSource source)
{
    g_lastRedrawMs = Now ();
    g_lastRedrawSource = source;
    ++g_window.redraws;
    g_window.redrawsAfterWheel += g_lastWheel != 0 && Now () - g_lastWheel <= kWheelWindowMs ? 1 : 0;
}

void NoteHudRefresh (uint32_t micros)
{
    (Navigating () ? g_window.refreshNavigating : g_window.refresh).Add (micros);
}

void NoteKeyTaken (uint32_t code, bool down, bool repeat)
{
    if (!down && (code == VK_SHIFT || code == VK_LSHIFT || code == VK_RSHIFT))
        g_lastShiftUpTakenMs = Now ();
    ++g_window.keysTaken;
    g_current.keysTaken += g_current.open ? 1 : 0;
    const bool modifier = code == VK_SHIFT || code == VK_LSHIFT || code == VK_RSHIFT || code == VK_CONTROL ||
                          code == VK_LCONTROL || code == VK_RCONTROL || code == VK_MENU || code == VK_LMENU ||
                          code == VK_RMENU;
    g_window.modifierRepeatsTaken += modifier && repeat ? 1 : 0;
    g_window.keysTakenNavigating += Navigating () ? 1 : 0;
}

void NoteMiddleDoubleClick (long x, long y)
{
    ++g_window.middleDoubleClicks;
    char line[256] = {};
    _snprintf_s (line, sizeof (line), _TRUNCATE,
                 "%-12s a middle double-click passed to Archicad at (%ld, %ld), Shift %s, %llu ms after the "
                 "press before it (Archicad's fit-in-window gesture)",
                 "NAVDBLCLK", x, y, ::GetAsyncKeyState (VK_SHIFT) < 0 ? "held" : "not held",
                 (unsigned long long) g_pressGapMs);
    ArchVizLog (line);
}

Heartbeat::Heartbeat () : navigating_ (Navigating ())
{
    LARGE_INTEGER counter = {};
    ::QueryPerformanceCounter (&counter);
    started_ = uint64_t (counter.QuadPart);
}

Heartbeat::~Heartbeat ()
{
    LARGE_INTEGER counter = {};
    LARGE_INTEGER frequency = {};
    ::QueryPerformanceCounter (&counter);
    ::QueryPerformanceFrequency (&frequency);
    const uint64_t micros = frequency.QuadPart > 0
                                ? (uint64_t (counter.QuadPart) - started_) * 1000000u / uint64_t (frequency.QuadPart)
                                : 0;
    AnalyseCamera ();
    (navigating_ ? g_window.heartbeatNavigating : g_window.heartbeat)
        .Add (uint32_t ((std::min) (micros, uint64_t (UINT32_MAX))));
    ReportIfDue ();
}

void Reset ()
{
    g_window = Window {};
    g_lastReport = 0;
    g_havePassedMove = false;
    g_passedX = g_passedY = 0;
    g_passedAt = 0;
    g_lastWheel = 0;
    g_lastNavigationStart = 0;
    g_pressGapMs = 0;
    g_current = g_previous = Gesture {};
    g_haveLast = false;
    g_lastRedrawMs = 0;
    g_lastShiftUpTakenMs = 0;
}

CameraStep Step (const float* a, const float* b)
{
    // Row-vector rigid views, p * R + t (OVERLAY-INVARIANTS.md finding 1): R in the upper 3x3,
    // t in the last row, the eye at -t * R^T.
    CameraStep step;
    double trace = 0.0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            trace += double (a[i * 4 + j]) * double (b[i * 4 + j]);
    const double cosine = (std::max) (-1.0, (std::min) (1.0, (trace - 1.0) / 2.0));
    step.degrees = std::acos (cosine) * 180.0 / 3.14159265358979323846;
    double travel = 0.0;
    for (int j = 0; j < 3; ++j) {
        double eyeA = 0.0;
        double eyeB = 0.0;
        for (int k = 0; k < 3; ++k) {
            eyeA -= double (a[12 + k]) * double (a[j * 4 + k]);
            eyeB -= double (b[12 + k]) * double (b[j * 4 + k]);
        }
        travel += (eyeB - eyeA) * (eyeB - eyeA);
    }
    step.travel = std::sqrt (travel);
    return step;
}

} // namespace navwatch
} // namespace archviz
} // namespace geomsrv
