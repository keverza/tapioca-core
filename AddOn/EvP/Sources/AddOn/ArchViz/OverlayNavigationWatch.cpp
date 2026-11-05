// ArchViz/OverlayNavigationWatch -- see the header.
//
// Overlay contract: private/docs/architecture/diligent/OVERLAY-INVARIANTS.md (§7).

#include "ArchViz/OverlayNavigationWatch.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/OverlayInputEvents.hpp" // windows.h, NOMINMAX first

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
};

Window g_window;
uint64_t g_lastReport = 0;
bool g_havePassedMove = false;
long g_passedX = 0;
long g_passedY = 0;
uint64_t g_passedAt = 0;
uint64_t g_lastWheel = 0;

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
        w.heartbeatNavigating.count == 0)
        return;
    char line[512] = {};
    _snprintf_s (line, sizeof (line), _TRUNCATE,
                 "%-12s ~5 s: navigations +%u (locked +%u; Archicad's last move >%.0f px away +%u, worst %.0f px "
                 "%llu ms old) | moves passed +%u eaten +%u (in Archicad's gesture after release +%u) | wheel +%u, "
                 "3D redraws +%u (within %llu ms of a wheel +%u) | main thread: heartbeat %s, navigating %s; HUD "
                 "layout %s, navigating %s",
                 "NAVWATCH", w.presses, w.pressesOnLocked, kFarPixels, w.pressesFar, w.worstPixels,
                 (unsigned long long) w.worstAgeMs, w.movesPassed, w.movesEaten, w.movesEatenFromHost, w.wheels,
                 w.redraws, (unsigned long long) kWheelWindowMs, w.redrawsAfterWheel, Describe (w.heartbeat).c_str (),
                 Describe (w.heartbeatNavigating).c_str (), Describe (w.refresh).c_str (),
                 Describe (w.refreshNavigating).c_str ());
    ArchVizLog (line);
}

} // namespace

void NoteMove (long x, long y, bool passed, bool eatenFromHostGesture)
{
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
    if (!g_havePassedMove)
        return;
    const double pixels = std::hypot (double (x - g_passedX), double (y - g_passedY));
    if (pixels <= kFarPixels)
        return;
    ++g_window.pressesFar;
    if (pixels > g_window.worstPixels) {
        g_window.worstPixels = pixels;
        g_window.worstAgeMs = Now () - g_passedAt;
    }
}

void NoteWheel ()
{
    ++g_window.wheels;
    g_lastWheel = Now ();
}

void NoteRedraw ()
{
    ++g_window.redraws;
    g_window.redrawsAfterWheel += g_lastWheel != 0 && Now () - g_lastWheel <= kWheelWindowMs ? 1 : 0;
}

void NoteHudRefresh (uint32_t micros)
{
    (Navigating () ? g_window.refreshNavigating : g_window.refresh).Add (micros);
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
}

} // namespace navwatch
} // namespace archviz
} // namespace geomsrv
