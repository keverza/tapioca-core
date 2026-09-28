// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings and
// each cost at least one. This diagnostic installs the Present hook and must only
// observe: it draws nothing, holds nothing of Archicad's across a frame, and
// releases every hook on every exit (§8).
// ArchViz/PlanFrameSession -- see the header. MAIN THREAD, except the report worker.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/PlanFrameSession.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/CameraSyncMode.hpp"
#include "ArchViz/Dxgi/PlanFrameRecord.hpp"
#include "ArchViz/Dxgi/PresentHook.hpp"
#include "ArchViz/ExperimentGuard.hpp"
#include "ArchViz/OverlayController.hpp"
#include "ArchViz/PlanFrameReport.hpp"
#include "ArchViz/PlanViewCamera.hpp"
#include "ArchViz/ViewportOverlayWindow.hpp"
#include "PlanOverlay/PlanTransformMath.hpp"
#include "Python/PathUtils.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <commctrl.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace planframes {

namespace {

namespace rec = dxgi::planframes;

constexpr UINT kTickMs = 15;
constexpr uint64_t kClosingMs = 400; // the readbacks in flight get this long to land
constexpr size_t kMaxSamples = 32768;
constexpr uint32_t kMaxDepth = 32;
constexpr UINT_PTR kSubclassId = 0x504C4E46; // 'PLNF'
// ⚠️ A STILL PLAN DOES NOT PRESENT, AND A STILL FRAME IS WHAT ANCHORS THE ANALYSIS.
// Every frame is placed from one whose transform is known -- still, with every read
// around it agreeing -- so a pause must produce two still frames. After this long
// with no plan Present and no button down, the plan is redrawn, twice per pause.
constexpr uint64_t kIdleMs = 250;
constexpr uint32_t kIdleRedraws = 2;

// Win32 subclassing is resolved from the comctl32 Archicad has already loaded, so
// the add-on gains no static import for a diagnostic.
using SetSubclassFn = BOOL (WINAPI*) (HWND, SUBCLASSPROC, UINT_PTR, DWORD_PTR);
using RemoveSubclassFn = BOOL (WINAPI*) (HWND, SUBCLASSPROC, UINT_PTR);
using DefSubclassFn = LRESULT (WINAPI*) (HWND, UINT, WPARAM, LPARAM);
SetSubclassFn g_setSubclass = nullptr;
RemoveSubclassFn g_removeSubclass = nullptr;
DefSubclassFn g_defSubclass = nullptr;

std::atomic<uint32_t> g_state { uint32_t (SessionState::Idle) };
std::string g_reason;
uint32_t g_seconds = 0;
ULONGLONG g_startedMs = 0;
ULONGLONG g_endedMs = 0;
int64_t g_startQpc = 0;

HWND g_canvas = nullptr;
std::string g_canvasClass;
double g_dpi = 1.0;
uint32_t g_canvasWidth = 0, g_canvasHeight = 0;
uint32_t g_logicalWidth = 0, g_logicalHeight = 0;
uint32_t g_mainThread = 0;
bool g_subclassed = false;
bool g_canvasGone = false;
HHOOK g_messageHook = nullptr;
UINT_PTR g_timer = 0;
bool g_timerPeriodHeld = false;
bool g_breadcrumb = false;
bool g_presentHookOurs = false;

// The canvas's messages in flight, innermost last.
struct InFlight {
    uint32_t message = 0;
    uint64_t serial = 0;
};
InFlight g_stack[kMaxDepth];
uint32_t g_depth = 0;
uint64_t g_messageSerial = 0;
uint64_t g_canvasMessages = 0;
uint64_t g_retrievedSerial = 0;
bool g_sampling = false;
// ⚠️ TRUE WHILE THE SESSION ITSELF IS INSIDE ACAPI_View_Redraw. Archicad may paint
// and present synchronously inside that call, and a read at such a Present would be
// ACAPI called from inside ACAPI -- untested, and those frames are still anyway.
bool g_redrawing = false;
uint64_t g_inputSerial = 0; // pan and zoom input seen on the canvas
uint64_t g_idleInputSerial = 0;
uint32_t g_idleRedraws = 0;
uint32_t g_closingRedraws = 0;
uint32_t g_redraws = 0;
uint64_t g_lastTargetPresents = 0;
ULONGLONG g_lastTargetPresentMs = 0;

std::vector<TransformSample> g_samples;
uint64_t g_sampleSerial = 0;
uint64_t g_samplesDropped = 0;

ChainRow g_target;
bool g_placementKnown = false;
int32_t g_offsetX = 0, g_offsetY = 0;
uint32_t g_targetClientWidth = 0, g_targetClientHeight = 0;
std::wstring g_outputDirectory;
std::string g_stamp;

// What the recording ended with, for the status while the worker runs.
rec::Stats g_finalStats;
// ⚠️ THE WORKER'S RESULT IS READ ONLY AFTER IT PUBLISHES `Done` (release/acquire
// on g_state), and joined before anything here is cleared.
std::thread g_worker;
ReportResult g_result;

SessionState State ()
{
    return SessionState (g_state.load (std::memory_order_acquire));
}

void SetState (SessionState state)
{
    g_state.store (uint32_t (state), std::memory_order_release);
}

int64_t QpcNow ()
{
    LARGE_INTEGER now = {};
    ::QueryPerformanceCounter (&now);
    return now.QuadPart;
}

int64_t QpcFrequency ()
{
    LARGE_INTEGER value = {};
    ::QueryPerformanceFrequency (&value);
    return value.QuadPart;
}

std::string Hex (uint64_t value)
{
    char text[24] = {};
    std::snprintf (text, sizeof (text), "0x%llx", (unsigned long long) value);
    return text;
}

std::string ClassOf (HWND window)
{
    char name[128] = {};
    if (window == nullptr || ::GetClassNameA (window, name, int (sizeof (name))) == 0)
        return std::string ();
    return name;
}

// ---- the transform ------------------------------------------------------------

// The error of a refused read is kept: a read refused inside Archicad's paint pass
// is an answer, and "invalid" alone would cost another run to learn which.
bool PointToCoord (uint32_t x, uint32_t y, planoverlay::PlanPoint& out, int32_t& error)
{
    API_Point point = {};
    point.h = short (x);
    point.v = short (y);
    API_Coord coord = {};
    const GSErrCode result = ACAPI_View_PointToCoord (&point, &coord);
    if (result != NoError) {
        error = int32_t (result);
        return false;
    }
    out.x = coord.x;
    out.y = coord.y;
    return true;
}

// ⚠️ LOGICAL PIXELS, AND THE SCALE IS LEFT TO THE ANALYSIS. ACAPI speaks the
// canvas's logical pixels (PlanViewCamera.cpp); what one of them is in the back
// buffer is exactly what the pixels are about to measure, so the sample does not
// assume it. Three corners fix the affine map whatever the scale; the fourth, a
// repeat of the first, says whether the view moved while it was being asked.
uint64_t TakeSample (SampleSource source, uint32_t message, uint32_t depth, uint64_t messageSerial)
{
    if (State () != SessionState::Recording || g_sampling)
        return 0;
    // ⚠️ NEVER PAST THE RESERVATION. The read at the Present runs inside Archicad's
    // paint pass, and the buffer was reserved at Start so that pushing never allocates.
    if (g_samples.size () >= kMaxSamples) {
        ++g_samplesDropped;
        return 0;
    }
    g_sampling = true;
    TransformSample sample;
    sample.source = source;
    sample.message = message;
    sample.depth = depth;
    sample.messageSerial = messageSerial;
    planoverlay::PlanPoint topLeft, topRight, bottomLeft, topLeftAgain;
    const int64_t began = QpcNow ();
    const bool read =
        PointToCoord (0, 0, topLeft, sample.error) && PointToCoord (g_logicalWidth, 0, topRight, sample.error) &&
        PointToCoord (0, g_logicalHeight, bottomLeft, sample.error) && PointToCoord (0, 0, topLeftAgain, sample.error);
    const int64_t ended = QpcNow ();
    g_sampling = false;
    sample.qpc = began;
    const int64_t frequency = QpcFrequency ();
    sample.costUs = frequency > 0 ? uint32_t ((ended - began) * 1000000 / frequency) : 0;
    if (read) {
        const planoverlay::LogicalSampleRect rect =
            planoverlay::MakeLogicalSampleRect (double (g_logicalWidth), double (g_logicalHeight), 1.0);
        const planoverlay::PlanTransform transform =
            planoverlay::ObservePlanTransform (rect, topLeft, topRight, bottomLeft, topLeftAgain);
        sample.valid = transform.valid;
        sample.torn = !transform.valid && transform.tearPixels > 0.25;
        sample.xx = transform.xx;
        sample.xy = transform.xy;
        sample.yx = transform.yx;
        sample.yy = transform.yy;
        sample.ox = transform.offsetX;
        sample.oy = transform.offsetY;
    }
    sample.serial = ++g_sampleSerial;
    g_samples.push_back (sample);
    if (sample.valid)
        rec::PublishLatestSample (sample.serial);
    return sample.serial;
}

// The read the Present half takes inside the plan's own Present -- see
// PlanFrameRecord.hpp's SetPresentReader for why it may, and when it is called.
uint64_t ReadAtPresent ()
{
    if (g_redrawing)
        return 0;
    return TakeSample (SampleSource::Present, 0, g_depth, 0);
}

// The messages worth a transform read: input, painting, timers, sizing, and every
// private or registered message -- which is how a framework schedules its own
// redraws. Nothing structural: creation, destruction, hit-testing, cursors.
bool Sampled (UINT message)
{
    switch (message) {
        case WM_PAINT:
        case WM_ERASEBKGND:
        case WM_TIMER:
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_CHAR:
        case WM_SIZE:
        case WM_HSCROLL:
        case WM_VSCROLL:
        case WM_CAPTURECHANGED:
            return true;
        default:
            return message >= WM_USER;
    }
}

void PublishTop ()
{
    if (g_depth == 0)
        rec::PublishCanvasMessage (0, 0, 0);
    else
        rec::PublishCanvasMessage (g_depth, g_stack[g_depth - 1].message, g_stack[g_depth - 1].serial);
}

// ⚠️ THE CANVAS'S OWN MESSAGES, BRACKETED. The transform is read as each message
// begins -- before Archicad's handler can move the view -- and again after the
// handler returns, and the Present path is told which message is in flight. A
// Present inside a message whose entry read equals its frame is the one B needs.
LRESULT CALLBACK CanvasProc (HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR)
{
    if (message == WM_NCDESTROY) {
        // The canvas is going: the subclass goes with it, and the session ends on
        // its next tick.
        g_removeSubclass (window, &CanvasProc, id);
        g_subclassed = false;
        g_canvasGone = true;
        return g_defSubclass (window, message, wParam, lParam);
    }
    if (State () != SessionState::Recording)
        return g_defSubclass (window, message, wParam, lParam);

    const uint64_t serial = ++g_messageSerial;
    ++g_canvasMessages;
    if (message == WM_LBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_RBUTTONDOWN ||
        message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL || message == WM_KEYDOWN)
        ++g_inputSerial;
    const bool tracked = g_depth < kMaxDepth;
    if (tracked) {
        g_stack[g_depth].message = message;
        g_stack[g_depth].serial = serial;
        ++g_depth;
        PublishTop ();
    }
    const uint32_t depth = g_depth;
    const bool sampled = Sampled (message);
    if (sampled)
        TakeSample (SampleSource::Entry, message, depth, serial);
    const LRESULT result = g_defSubclass (window, message, wParam, lParam);
    if (sampled)
        TakeSample (SampleSource::Exit, message, depth, serial);
    // A teardown inside this message (a timer nested in Archicad's own loop) has
    // already emptied the stack; never pop below it.
    if (tracked && g_depth > 0) {
        --g_depth;
        PublishTop ();
    }
    return result;
}

LRESULT CALLBACK GetMessageProc (int code, WPARAM wParam, LPARAM lParam)
{
    // ⚠️ PM_NOREMOVE IS A PEEK, NOT A RETRIEVAL -- the message comes back again.
    if (code == HC_ACTION && lParam != 0 && wParam == PM_REMOVE) {
        const MSG* message = reinterpret_cast<const MSG*> (lParam);
        rec::PublishRetrievedMessage (message->message, uint64_t (uintptr_t (message->hwnd)), ++g_retrievedSerial);
    }
    return ::CallNextHookEx (g_messageHook, code, wParam, lParam);
}

// ---- the plan's chain -----------------------------------------------------------

// How a chain's window relates to the canvas; the closer, the better.
int RelationRank (HWND chainWindow, std::string& relation)
{
    if (chainWindow == nullptr || g_canvas == nullptr) {
        relation = "none";
        return 0;
    }
    if (chainWindow == g_canvas) {
        relation = "the canvas itself";
        return 3;
    }
    if (::IsChild (chainWindow, g_canvas)) {
        relation = "an ancestor of the canvas";
        return 2;
    }
    if (::IsChild (g_canvas, chainWindow)) {
        relation = "a child of the canvas";
        return 1;
    }
    relation = "unrelated";
    return 0;
}

std::vector<ChainRow> Inventory ()
{
    dxgi::ChainInfo chains[16];
    const size_t count = dxgi::GetChainInventory (chains, 16);
    std::vector<ChainRow> rows;
    for (size_t i = 0; i < count; ++i) {
        ChainRow row;
        row.chain = chains[i].swapChain;
        row.window = chains[i].window;
        row.presents = chains[i].presents;
        row.width = chains[i].width;
        row.height = chains[i].height;
        row.format = chains[i].format;
        row.swapEffect = chains[i].swapEffect;
        row.bufferCount = chains[i].bufferCount;
        row.flags = chains[i].flags;
        row.ours = chains[i].ours;
        const HWND window = (HWND) (uintptr_t) row.window;
        row.windowClass = (window != nullptr && ::IsWindow (window)) ? ClassOf (window) : std::string ("(gone)");
        RelationRank (window, row.relation);
        rows.push_back (row);
    }
    return rows;
}

// Where the canvas's client area begins inside the chain window's, physical
// pixels -- zero when the chain presents into the canvas itself.
void MeasurePlacement ()
{
    g_placementKnown = false;
    const HWND window = (HWND) (uintptr_t) g_target.window;
    if (window == nullptr || !::IsWindow (window) || g_canvas == nullptr || !::IsWindow (g_canvas))
        return;
    POINT canvasOrigin = { 0, 0 };
    POINT chainOrigin = { 0, 0 };
    ::ClientToScreen (g_canvas, &canvasOrigin);
    ::ClientToScreen (window, &chainOrigin);
    RECT client = {};
    ::GetClientRect (window, &client);
    g_placementKnown = true;
    g_offsetX = canvasOrigin.x - chainOrigin.x;
    g_offsetY = canvasOrigin.y - chainOrigin.y;
    g_targetClientWidth = uint32_t (client.right - client.left);
    g_targetClientHeight = uint32_t (client.bottom - client.top);
}

void Nominate ()
{
    if (rec::Target () != 0)
        return;
    int bestRank = 0;
    ChainRow best;
    for (const ChainRow& row : Inventory ()) {
        if (row.ours)
            continue;
        std::string relation;
        const int rank = RelationRank ((HWND) (uintptr_t) row.window, relation);
        if (rank > bestRank || (rank == bestRank && rank > 0 && row.presents > best.presents)) {
            bestRank = rank;
            best = row;
        }
    }
    if (bestRank == 0)
        return;
    g_target = best;
    rec::SetTarget (best.chain);
    MeasurePlacement ();
    ArchVizLog ("PLAN FRAMES  target " + Hex (best.chain) + " presents into " + Hex (best.window) + " '" +
                best.windowClass + "', " + best.relation + ", " + std::to_string (best.width) + "x" +
                std::to_string (best.height) + " fmt=" + std::to_string (best.format) +
                " swapEffect=" + std::to_string (best.swapEffect) + " buffers=" + std::to_string (best.bufferCount) +
                "; the canvas starts at " + std::to_string (g_offsetX) + "," + std::to_string (g_offsetY) + " of its " +
                std::to_string (g_targetClientWidth) + "x" + std::to_string (g_targetClientHeight) + " client area");
}

void Redraw ()
{
    g_redrawing = true;
    ACAPI_View_Redraw ();
    g_redrawing = false;
}

// Two still frames per pause, so the analysis has an anchor there. Never while a
// button is down or a canvas message is in flight: a redraw inside Archicad's own
// drag loop is not something a diagnostic gets to try.
bool Untouched ()
{
    return g_depth == 0 && (::GetAsyncKeyState (VK_LBUTTON) & 0x8000) == 0 &&
           (::GetAsyncKeyState (VK_MBUTTON) & 0x8000) == 0 && (::GetAsyncKeyState (VK_RBUTTON) & 0x8000) == 0;
}

void RedrawWhenIdle ()
{
    if (rec::Target () == 0 || !Untouched ())
        return;
    const ULONGLONG now = ::GetTickCount64 ();
    const uint64_t presents = rec::GetStats ().targetPresents;
    if (presents != g_lastTargetPresents) {
        g_lastTargetPresents = presents;
        g_lastTargetPresentMs = now;
    }
    if (g_inputSerial != g_idleInputSerial) {
        g_idleInputSerial = g_inputSerial;
        g_idleRedraws = 0;
    }
    if (g_idleRedraws >= kIdleRedraws || now - g_lastTargetPresentMs < kIdleMs)
        return;
    ++g_idleRedraws;
    ++g_redraws;
    Redraw ();
}

// ---- lifecycle --------------------------------------------------------------------

void ReleaseTimer ()
{
    if (g_timer != 0) {
        ::KillTimer (nullptr, g_timer);
        g_timer = 0;
    }
    if (g_timerPeriodHeld) {
        ::timeEndPeriod (1);
        g_timerPeriodHeld = false;
    }
}

// Everything the recording installed, in the order that makes each step safe: the
// recorder stops before its hook comes out, and the hook's removal drains every
// Present in flight before anything it wrote is read.
void ReleaseHooks ()
{
    rec::SetPresentReader (nullptr);
    rec::Disarm ();
    if (g_subclassed && g_canvas != nullptr && ::IsWindow (g_canvas))
        g_removeSubclass (g_canvas, &CanvasProc, kSubclassId);
    g_subclassed = false;
    if (g_messageHook != nullptr) {
        ::UnhookWindowsHookEx (g_messageHook);
        g_messageHook = nullptr;
    }
    g_depth = 0;
    rec::PublishCanvasMessage (0, 0, 0);
    ReleaseTimer ();
    if (g_presentHookOurs) {
        dxgi::RemovePresentHook ();
        g_presentHookOurs = false;
    }
    if (g_breadcrumb) {
        experimentguard::Disarm ();
        g_breadcrumb = false;
    }
}

void JoinWorker ()
{
    if (g_worker.joinable ())
        g_worker.join ();
}

void ClearRecord ()
{
    JoinWorker ();
    std::vector<TransformSample> ().swap (g_samples);
    g_target = ChainRow {};
    g_placementKnown = false;
    g_offsetX = g_offsetY = 0;
    g_targetClientWidth = g_targetClientHeight = 0;
    g_finalStats = rec::Stats {};
    g_result = ReportResult {};
    g_reason.clear ();
    g_sampleSerial = 0;
    g_samplesDropped = 0;
    g_messageSerial = 0;
    g_canvasMessages = 0;
    g_retrievedSerial = 0;
    g_depth = 0;
    g_sampling = false;
    g_canvasGone = false;
    g_startedMs = g_endedMs = 0;
    g_inputSerial = g_idleInputSerial = 0;
    g_idleRedraws = g_closingRedraws = g_redraws = 0;
    g_lastTargetPresents = 0;
    g_lastTargetPresentMs = 0;
}

void EndRecording (const std::string& reason)
{
    if (State () != SessionState::Recording)
        return;
    g_reason = reason;
    g_endedMs = ::GetTickCount64 ();
    g_closingRedraws = 0;
    rec::StopCapturing ();
    SetState (SessionState::Closing);
    ArchVizLog ("PLAN FRAMES  recording ended: " + reason);
}

void Finalize ()
{
    ReportInput input;
    // The last look at the chains, while the hook that learned them still stands.
    input.chains = Inventory ();
    ReleaseHooks ();

    g_finalStats = rec::GetStats ();
    input.stats = g_finalStats;
    input.presents.assign (rec::kMaxPresents, rec::PresentRecord {});
    input.presents.resize (rec::CopyPresents (input.presents.data (), input.presents.size ()));
    const size_t frameBytes = size_t (rec::kFrameWidth) * rec::kFrameHeight;
    for (size_t i = 0; i < rec::kMaxFrames; ++i) {
        rec::FrameInfo info;
        const uint8_t* pixels = nullptr;
        if (!rec::CopyFrame (i, info, pixels))
            continue;
        input.frames.push_back (info);
        input.framePixels.insert (input.framePixels.end (), pixels, pixels + frameBytes);
    }
    rec::Release (true);

    input.reason = g_reason;
    input.qpcFrequency = QpcFrequency ();
    input.startQpc = g_startQpc;
    input.mainThread = g_mainThread;
    input.dpi = g_dpi;
    input.canvasWindow = uint64_t (uintptr_t (g_canvas));
    input.canvasClass = g_canvasClass;
    input.canvasWidth = g_canvasWidth;
    input.canvasHeight = g_canvasHeight;
    input.logicalWidth = g_logicalWidth;
    input.logicalHeight = g_logicalHeight;
    input.placementKnown = g_placementKnown;
    input.offsetX = g_offsetX;
    input.offsetY = g_offsetY;
    input.targetClientWidth = g_targetClientWidth;
    input.targetClientHeight = g_targetClientHeight;
    input.target = g_target;
    input.samplesDropped = g_samplesDropped;
    input.canvasMessages = g_canvasMessages;
    input.retrievedMessages = g_retrievedSerial;
    input.idleRedraws = g_redraws;
    input.samples = g_samples; // a copy: the status still counts them while the worker runs
    input.directory = g_outputDirectory;
    input.stamp = g_stamp;

    SetState (SessionState::Analysing);
    ArchVizLog ("PLAN FRAMES  analysing " + std::to_string (input.frames.size ()) + " frames, " +
                std::to_string (input.presents.size ()) + " Presents, " + std::to_string (input.samples.size ()) +
                " transform samples");
    g_worker = std::thread ([owned = std::move (input)] () {
        g_result = WriteReport (owned);
        SetState (SessionState::Done);
    });
}

void CALLBACK TickProc (HWND, UINT, UINT_PTR, DWORD)
{
    const SessionState state = State ();
    if (state == SessionState::Recording) {
        if (g_canvasGone) {
            EndRecording ("the plan canvas was destroyed");
            return;
        }
        if (!CurrentWindowIsFloorPlan ()) {
            EndRecording ("the floor plan is no longer the window in front");
            return;
        }
        const overlaycontrol::Status overlay = overlaycontrol::GetStatus ();
        if (overlay.injectedRunning || overlay.portableRunning) {
            EndRecording ("an overlay was started during the recording");
            return;
        }
        TakeSample (SampleSource::Timer, 0, g_depth, 0);
        Nominate ();
        RedrawWhenIdle ();
        if (::GetTickCount64 () - g_startedMs >= uint64_t (g_seconds) * 1000u)
            EndRecording ("the recording window ended");
        else if (rec::GetStats ().framesSubmitted >= rec::kMaxFrames)
            EndRecording ("the frame store is full");
        return;
    }
    if (state == SessionState::Closing) {
        const rec::Stats stats = rec::GetStats ();
        const bool drained = stats.framesReady + stats.readbackFailures >= stats.framesSubmitted;
        if (drained || ::GetTickCount64 () - g_endedMs >= kClosingMs) {
            Finalize ();
            return;
        }
        // ⚠️ THE LAST FRAMES ARE READ BACK AT THE PLAN'S NEXT PRESENT, and a still plan
        // makes none: without one the closing pause -- the anchor the last gesture is
        // scored from -- never arrives.
        if (rec::Target () != 0 && g_closingRedraws < 4 && Untouched ()) {
            ++g_closingRedraws;
            ++g_redraws;
            Redraw ();
        }
    }
}

bool ResolveSubclassing (std::string& error)
{
    if (g_setSubclass != nullptr && g_removeSubclass != nullptr && g_defSubclass != nullptr)
        return true;
    const HMODULE comctl = ::GetModuleHandleW (L"comctl32.dll");
    if (comctl == nullptr) {
        error = "comctl32.dll is not loaded in this process, so the canvas cannot be subclassed";
        return false;
    }
    g_setSubclass = reinterpret_cast<SetSubclassFn> (::GetProcAddress (comctl, "SetWindowSubclass"));
    g_removeSubclass = reinterpret_cast<RemoveSubclassFn> (::GetProcAddress (comctl, "RemoveWindowSubclass"));
    g_defSubclass = reinterpret_cast<DefSubclassFn> (::GetProcAddress (comctl, "DefSubclassProc"));
    if (g_setSubclass == nullptr || g_removeSubclass == nullptr || g_defSubclass == nullptr) {
        g_setSubclass = nullptr;
        g_removeSubclass = nullptr;
        g_defSubclass = nullptr;
        error = "the loaded comctl32.dll does not export SetWindowSubclass";
        return false;
    }
    return true;
}

std::string Stamp ()
{
    SYSTEMTIME now = {};
    ::GetLocalTime (&now);
    char text[32] = {};
    std::snprintf (text, sizeof (text), "%04u%02u%02u_%02u%02u%02u", unsigned (now.wYear), unsigned (now.wMonth),
                   unsigned (now.wDay), unsigned (now.wHour), unsigned (now.wMinute), unsigned (now.wSecond));
    return text;
}

// Everything Start refuses on, before anything is installed.
bool Refused (std::string& error)
{
    const SessionState state = State ();
    if (state == SessionState::Recording || state == SessionState::Closing || state == SessionState::Analysing) {
        error = std::string ("a plan frame record is already ") + SessionStateName (state);
        return true;
    }
    if (!CurrentWindowIsFloorPlan ()) {
        error = "bring the floor plan to the front first: the record measures the floor plan's own frames";
        return true;
    }
    const overlaycontrol::Status overlay = overlaycontrol::GetStatus ();
    if (overlay.injectedRunning || overlay.portableRunning || CurrentCameraSyncMode () != CameraSyncMode::Off) {
        error = "turn the Tapioca overlay off first: the record measures Archicad's frames, and an overlay's own "
                "poll, window and render thread would be part of the measurement";
        return true;
    }
    if (dxgi::PresentHookInstalled ()) {
        error = "the Present hook is already installed by something else; run CameraSyncReset and try again";
        return true;
    }
    if (experimentguard::Blocked ()) {
        error = experimentguard::WhyBlocked ();
        return true;
    }
    return !ResolveSubclassing (error);
}

bool Install (std::string& error)
{
    if (!experimentguard::Arm ("planframes", error))
        return false;
    g_breadcrumb = true;
    if (!rec::Arm (g_mainThread, error))
        return false;
    SetState (SessionState::Recording); // the subclass and the hooks record only while this holds
    rec::SetPresentReader (&ReadAtPresent);
    if (!dxgi::InstallPresentHook (error))
        return false;
    g_presentHookOurs = true;
    if (!g_setSubclass (g_canvas, &CanvasProc, kSubclassId, 0)) {
        error = "SetWindowSubclass refused the plan canvas (GetLastError " + std::to_string (::GetLastError ()) + ")";
        return false;
    }
    g_subclassed = true;
    g_messageHook = ::SetWindowsHookExW (WH_GETMESSAGE, &GetMessageProc, nullptr, ::GetCurrentThreadId ());
    if (g_messageHook == nullptr) {
        error = "SetWindowsHookEx(WH_GETMESSAGE) failed with GetLastError " + std::to_string (::GetLastError ());
        return false;
    }
    g_timerPeriodHeld = ::timeBeginPeriod (1) == TIMERR_NOERROR;
    g_timer = ::SetTimer (nullptr, 0, kTickMs, &TickProc);
    if (g_timer == 0) {
        error = "SetTimer failed with GetLastError " + std::to_string (::GetLastError ());
        return false;
    }
    return true;
}

} // namespace

const char* SessionStateName (SessionState state)
{
    switch (state) {
        case SessionState::Idle:
            return "idle";
        case SessionState::Recording:
            return "recording";
        case SessionState::Closing:
            return "closing";
        case SessionState::Analysing:
            return "analysing";
        case SessionState::Done:
            return "done";
        case SessionState::Failed:
            return "failed";
    }
    return "?";
}

bool Start (uint32_t seconds, std::string& error)
{
    if (Refused (error))
        return false;
    const viewportoverlay::OverlayTarget canvas = viewportoverlay::FindOverlayTarget ();
    if (!canvas.valid || canvas.window == nullptr) {
        error = "the plan canvas could not be found: " + canvas.how;
        return false;
    }

    // Every Start resets what every Stop leaves behind (§8).
    ClearRecord ();
    g_canvas = canvas.window;
    g_canvasClass = canvas.windowClass;
    RECT client = {};
    ::GetClientRect (g_canvas, &client);
    g_canvasWidth = uint32_t (client.right - client.left);
    g_canvasHeight = uint32_t (client.bottom - client.top);
    const UINT dpi = ::GetDpiForWindow (g_canvas);
    g_dpi = dpi != 0 ? double (dpi) / 96.0 : 1.0;
    g_logicalWidth = uint32_t (std::lround (double (g_canvasWidth) / g_dpi));
    g_logicalHeight = uint32_t (std::lround (double (g_canvasHeight) / g_dpi));
    if (g_logicalWidth < 2 || g_logicalHeight < 2 || g_logicalWidth > 32000 || g_logicalHeight > 32000) {
        error = "the plan canvas is " + std::to_string (g_canvasWidth) + "x" + std::to_string (g_canvasHeight) +
                ", which cannot be sampled";
        return false;
    }
    g_mainThread = uint32_t (::GetCurrentThreadId ());
    g_seconds = std::max (2u, std::min (30u, seconds));
    g_stamp = Stamp ();
    const GS::UniString dataDir = evp::EvpDataDir ();
    g_outputDirectory = dataDir.IsEmpty ()
                            ? std::wstring ()
                            : std::wstring ((const wchar_t*) dataDir.ToUStr ().Get ()) + L"\\logs\\plan_frames";
    g_samples.reserve (kMaxSamples);

    if (!Install (error)) {
        SetState (SessionState::Idle);
        ReleaseHooks ();
        rec::Release (true);
        return false;
    }
    g_startedMs = ::GetTickCount64 ();
    g_startQpc = QpcNow ();
    TakeSample (SampleSource::Timer, 0, 0, 0);
    ArchVizLog ("PLAN FRAMES  recording " + std::to_string (g_seconds) + " s over the plan canvas " +
                Hex (uint64_t (uintptr_t (g_canvas))) + " '" + g_canvasClass + "' " + std::to_string (g_canvasWidth) +
                "x" + std::to_string (g_canvasHeight) + " physical, " + std::to_string (g_logicalWidth) + "x" +
                std::to_string (g_logicalHeight) + " logical at scaling " + std::to_string (g_dpi) + ", main thread " +
                std::to_string (g_mainThread));
    // ⚠️ ONE REDRAW, SO THE PLAN'S CHAIN PRESENTS AT ONCE. A still plan does not
    // present at all, and the chain can only be identified once it has.
    Redraw ();
    return true;
}

void Stop (const char* reason)
{
    if (State () == SessionState::Recording)
        EndRecording (reason != nullptr ? reason : "stopped");
}

void Reset ()
{
    const SessionState state = State ();
    if (state == SessionState::Recording || state == SessionState::Closing) {
        Shutdown ();
        return;
    }
    ClearRecord ();
    SetState (SessionState::Idle);
}

void Shutdown ()
{
    const SessionState state = State ();
    if (state == SessionState::Recording || state == SessionState::Closing) {
        ReleaseHooks ();
        rec::Release (true);
        ArchVizLog ("PLAN FRAMES  shut down while " + std::string (SessionStateName (state)) +
                    "; nothing was analysed");
    }
    ClearRecord ();
    SetState (SessionState::Idle);
}

SessionStatus GetStatus ()
{
    SessionStatus status;
    const SessionState state = State ();
    status.state = state;
    const bool recording = state == SessionState::Recording || state == SessionState::Closing;
    const rec::Stats stats = recording ? rec::GetStats () : g_finalStats;
    status.reason = g_reason;
    status.seconds = g_seconds;
    if (g_startedMs != 0)
        status.elapsedMs = (state == SessionState::Recording ? ::GetTickCount64 () : g_endedMs) - g_startedMs;
    status.presents = stats.presents;
    status.presentsDropped = stats.presentsDropped;
    status.targetPresents = stats.targetPresents;
    status.framesSubmitted = stats.framesSubmitted;
    status.framesReady = stats.framesReady;
    status.slotsBusy = stats.slotsBusy;
    status.readbackFailures = stats.readbackFailures;
    status.createFailures = stats.createFailures;
    status.unsupportedFormat = stats.unsupportedFormat;
    status.targetChanges = stats.targetChanges;
    status.format = stats.format;
    for (const TransformSample& sample : g_samples) {
        if (sample.source == SampleSource::Entry)
            ++status.samplesEntry;
        else if (sample.source == SampleSource::Exit)
            ++status.samplesExit;
        else
            ++status.samplesTimer;
        if (!sample.valid)
            ++status.samplesInvalid;
        if (sample.torn)
            ++status.samplesTorn;
    }
    status.samplesDropped = g_samplesDropped;
    status.canvasMessages = g_canvasMessages;
    status.retrievedMessages = g_retrievedSerial;
    status.idleRedraws = g_redraws;
    status.mainThread = g_mainThread;
    status.dpi = g_dpi;
    status.canvasClass = g_canvasClass;
    status.canvasWidth = g_canvasWidth;
    status.canvasHeight = g_canvasHeight;
    if (g_target.chain != 0) {
        status.target = Hex (g_target.chain);
        status.targetClass = g_target.windowClass;
        status.targetRelation = g_target.relation;
    }
    if (state == SessionState::Done) {
        status.pairs = g_result.pairs;
        status.pairsValid = g_result.pairsValid;
        status.analysisMs = g_result.analysisMs;
        status.jsonPath = g_result.jsonPath;
        status.framesPath = g_result.framesPath;
    }
    return status;
}

} // namespace planframes
} // namespace archviz
} // namespace geomsrv
