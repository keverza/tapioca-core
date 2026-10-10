// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings and each
// cost at least one. The plan overlay draws at the plan's Present with the transform
// read at that Present (finding 14), owns only what it installs, and every Stop takes
// all of it back (§8).
// ArchViz/PlanOverlayRuntime -- see the header. MAIN THREAD.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/PlanOverlayRuntime.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/Dxgi/PlanGuest.hpp"
#include "ArchViz/Dxgi/PlanOverlayLayer.hpp"
#include "ArchViz/Dxgi/PresentHook.hpp"
#include "ArchViz/ExperimentGuard.hpp"
#include "ArchViz/OverlayController.hpp"
#include "ArchViz/OverlayGuestText.hpp"
#include "ArchViz/OverlayHover.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayHudModel.hpp"
#include "ArchViz/OverlayInput.hpp"
#include "ArchViz/OverlayLayers.hpp"
#include "ArchViz/OverlayVisibility.hpp"
#include "ArchViz/PlanFrameSession.hpp"
#include "ArchViz/PlanOverlayContent.hpp"
#include "ArchViz/PlanViewCamera.hpp"
#include "ArchViz/PlanViewTransform.hpp"
#include "ArchViz/ViewportOverlayWindow.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace geomsrv {
namespace archviz {
namespace planruntime {

namespace {

namespace layer = dxgi::planlayer;

constexpr UINT kTickMs = 100;
constexpr ULONGLONG kReportMs = 1000;
constexpr ULONGLONG kDeviceRedrawMs = 1000;
// The DevKit's reading of an arc: a positive angle puts it on the chord's right.
constexpr double kArcSign = 1.0;
// A centimetre per chord: a metre-radius arc strays 0.0125 mm from its chords.
constexpr double kArcChordMetres = 0.01;

ContentReader g_contentReader = nullptr;
bool g_running = false;
HWND g_canvas = nullptr;
std::string g_canvasClass;
double g_dpi = 1.0;
uint32_t g_canvasWidth = 0, g_canvasHeight = 0;
uint32_t g_logicalWidth = 0, g_logicalHeight = 0;
uint32_t g_mainThread = 0;
UINT_PTR g_timer = 0;
bool g_presentHookOurs = false;
bool g_breadcrumb = false;
// ⚠️ ABOVE ZERO WHILE THIS SESSION IS INSIDE AN ACAPI CALL OF ITS OWN (§11). Its
// redraw presents synchronously, and a read at that Present would be ACAPI inside
// ACAPI; the layer draws such a frame with the last transform read, which a redraw
// has not moved.
int g_ownAcapi = 0;
short g_storey = 0;
bool g_storeyKnown = false;
plancontent::Content g_content;
uint64_t g_generation = 0; // never reset: a new session's content is always new to the layer
bool g_readyLogged = false;
bool g_redrawPending = false;
uint32_t g_hudBeats = 0; // ticks, for the HUD's own heartbeat
std::string g_lastError;
std::string g_lastGuestError;
uint64_t g_reportedGuestDraws = 0;
uint64_t g_reportedGuestCalls = 0;
ULONGLONG g_startedMs = 0;
ULONGLONG g_lastReportMs = 0;
ULONGLONG g_lastDeviceRedrawMs = 0;
ULONGLONG g_lastTargetCheckMs = 0;
layer::Stats g_reported;

struct OwnAcapi {
    OwnAcapi ()
    {
        ++g_ownAcapi;
    }
    ~OwnAcapi ()
    {
        --g_ownAcapi;
    }
    OwnAcapi (const OwnAcapi&) = delete;
    OwnAcapi& operator= (const OwnAcapi&) = delete;
};

std::string Hex (uint64_t value)
{
    char text[24] = {};
    std::snprintf (text, sizeof (text), "0x%llx", (unsigned long long) value);
    return text;
}

// ⚠️ LOGICAL TO PHYSICAL IS THE WINDOW'S SCALING AND NOTHING ELSE. The canvas IS the
// chain's window, so its client origin is the buffer's (the frame record measured 0,0),
// and the record placed its frames at p95 0.50 px with exactly this factor.
plancontent::PixelTransform ToPhysical (const PlanViewTransform& t)
{
    plancontent::PixelTransform p;
    p.xx = t.xx * g_dpi;
    p.xy = t.xy * g_dpi;
    p.yx = t.yx * g_dpi;
    p.yy = t.yy * g_dpi;
    p.ox = t.ox * g_dpi;
    p.oy = t.oy * g_dpi;
    return p;
}

// INSIDE THE PLAN'S PRESENT: the layer has already checked the chain is the canvas's
// and the thread is ACAPI's; this checks the third condition of §11's exception.
layer::Read ReadAtPresent (plancontent::PixelTransform& out, int32_t& error)
{
    if (g_ownAcapi > 0)
        return layer::Read::Refused;
    const PlanViewTransform transform = ReadPlanViewTransform (g_logicalWidth, g_logicalHeight);
    if (!transform.valid) {
        error = transform.error;
        return layer::Read::Invalid;
    }
    out = ToPhysical (transform);
    return layer::Read::Fresh;
}

bool PlanInFront ()
{
    const OwnAcapi own;
    return CurrentWindowIsFloorPlan ();
}

// ⚠️ NOTHING OF OURS INSIDE ARCHICAD'S OWN DRAG LOOP. A pan is a middle-drag and an
// edit is a left one; while a button is down the tick calls no ACAPI at all, and a
// redraw it wants waits for the release -- the frame record's rule, kept.
bool Untouched ()
{
    return (::GetAsyncKeyState (VK_LBUTTON) & 0x8000) == 0 && (::GetAsyncKeyState (VK_MBUTTON) & 0x8000) == 0 &&
           (::GetAsyncKeyState (VK_RBUTTON) & 0x8000) == 0;
}

void Redraw ()
{
    const OwnAcapi own;
    ACAPI_View_Redraw ();
}

bool ReadStorey (short& storey)
{
    const OwnAcapi own;
    API_StoryInfo info = {};
    if (ACAPI_ProjectSetting_GetStorySettings (&info) != NoError)
        return false;
    storey = info.actStory;
    if (info.data != nullptr)
        BMKillHandle (reinterpret_cast<GSHandle*> (&info.data));
    return true;
}

bool ReadContent (std::string& error)
{
    std::vector<std::vector<double>> rings, arcs;
    {
        const OwnAcapi own;
        if (!g_contentReader (rings, arcs, error))
            return false;
    }
    g_content = plancontent::BuildContent (rings, arcs, kArcSign, kArcChordMetres);
    ++g_generation;
    return true;
}

// The canvas's size and scaling, again on every tick: a resize or a move to another
// monitor changes both, and the read at the Present samples the canvas's corners.
bool MeasureCanvas (std::string& error)
{
    RECT client = {};
    if (g_canvas == nullptr || !::GetClientRect (g_canvas, &client)) {
        error = "the plan canvas has no client area";
        return false;
    }
    const uint32_t width = uint32_t (client.right - client.left);
    const uint32_t height = uint32_t (client.bottom - client.top);
    const UINT dpi = ::GetDpiForWindow (g_canvas);
    const double scale = dpi != 0 ? double (dpi) / 96.0 : 1.0;
    const uint32_t logicalWidth = uint32_t (std::lround (double (width) / scale));
    const uint32_t logicalHeight = uint32_t (std::lround (double (height) / scale));
    // `API_Point` is a pair of shorts: a wider canvas would wrap silently.
    if (logicalWidth < 2 || logicalHeight < 2 || logicalWidth > 32000 || logicalHeight > 32000) {
        error = "the plan canvas is " + std::to_string (width) + "x" + std::to_string (height) +
                ", which cannot be sampled";
        return false;
    }
    g_canvasWidth = width;
    g_canvasHeight = height;
    g_dpi = scale;
    g_logicalWidth = logicalWidth;
    g_logicalHeight = logicalHeight;
    return true;
}

// ⚠️ DELTAS, NOT TOTALS (§7), and every decline by name. A line is written for every
// interval in which the canvas presented or anything declined, and once at the end.
void Report (bool final)
{
    const ULONGLONG now = ::GetTickCount64 ();
    if (!final && now - g_lastReportMs < kReportMs)
        return;
    const layer::Stats stats = layer::GetStats ();
    const uint64_t presents = stats.canvasPresents - g_reported.canvasPresents;
    uint64_t declined = 0;
    std::string declines;
    for (size_t i = 0; i < size_t (layer::Decline::Count); ++i) {
        const uint64_t delta = stats.declines[i] - g_reported.declines[i];
        if (delta == 0)
            continue;
        declined += delta;
        declines += " " + std::string (layer::DeclineName (layer::Decline (i))) + "=" + std::to_string (delta);
    }
    const double seconds = double (now - g_lastReportMs) / 1000.0;
    g_lastReportMs = now;
    if (presents == 0 && declined == 0 && !final) {
        g_reported = stats;
        return;
    }
    // The Diligent guest's share of those frames, when it has anything to draw.
    const dxgi::planguest::Stats guest = dxgi::planguest::GetStats ();
    std::string guestPart;
    if (dxgi::planguest::HasContent () || guest.draws != g_reportedGuestDraws)
        guestPart = "; guest drew " + std::to_string (guest.draws - g_reportedGuestDraws) + " (" +
                    std::to_string (guest.drawCalls - g_reportedGuestCalls) + " calls)";
    g_reportedGuestDraws = guest.draws;
    g_reportedGuestCalls = guest.drawCalls;
    char line[512] = {};
    std::snprintf (line, sizeof (line),
                   "PLAN OVERLAY  %s%.1f s: %llu canvas Presents, %llu drawn (%llu with the last read, %llu "
                   "hidden by the user); reads fresh %llu refused %llu invalid %llu (last error %d); read %u us "
                   "(worst %u), draw %u us (worst %u); declined %llu%s%s",
                   final ? "final " : "", seconds, (unsigned long long) presents,
                   (unsigned long long) (stats.drawn - g_reported.drawn),
                   (unsigned long long) (stats.drawnWithLastRead - g_reported.drawnWithLastRead),
                   (unsigned long long) (stats.hidden - g_reported.hidden),
                   (unsigned long long) (stats.readsFresh - g_reported.readsFresh),
                   (unsigned long long) (stats.readsRefused - g_reported.readsRefused),
                   (unsigned long long) (stats.readsInvalid - g_reported.readsInvalid), stats.lastReadError,
                   stats.readUsLast, stats.readUsMax, stats.drawUsLast, stats.drawUsMax, (unsigned long long) declined,
                   declines.c_str (), guestPart.c_str ());
    ArchVizLog (line);
    g_reported = stats;
    layer::TakeMaxima ();
}

// Everything this session installed, in the order that makes each step safe: the
// layer stops drawing first (the plan presents on this thread, so no draw is in flight
// here), the hook comes out -- draining other threads' Presents -- and only then do
// the D3D objects and the device go.
void Teardown (const std::string& reason, bool acapi)
{
    const bool wasRunning = g_running;
    g_running = false;
    overlayinput::Detach (overlayinput::View::Plan);
    layer::Disarm ();
    if (g_timer != 0) {
        ::KillTimer (nullptr, g_timer);
        g_timer = 0;
    }
    if (wasRunning)
        Report (true);
    if (g_presentHookOurs) {
        dxgi::RemovePresentHook ();
        g_presentHookOurs = false;
    }
    layer::Release ();
    dxgi::planguest::Release ();
    g_reportedGuestDraws = 0;
    g_reportedGuestCalls = 0;
    g_lastGuestError.clear ();
    if (g_breadcrumb) {
        experimentguard::Disarm ();
        g_breadcrumb = false;
    }
    if (!wasRunning)
        return;
    ArchVizLog ("PLAN OVERLAY  stopped: " + reason);
    // The last frame presented still carries the overlay; a still plan would keep it.
    if (acapi && PlanInFront ())
        Redraw ();
}

// Whether the plan's HUD is on screen: the session composes at every canvas Present.
// Called from the HUD's message hook, on this thread: a plain read.
bool HudShown ()
{
    return g_running;
}

// ⚠️ HOVER MODE IN THE PLAN (OverlayHover.hpp, D13): what is under the pointer, picked on the
// main thread in model metres -- the HUD's layout is where the pointer is known, and the
// plan's transform is read here as its Present reads it (finding 14). A slice is tinted
// whole, a heatmap's cell alone.
constexpr size_t kTintTriangles = 4096; // a mesh larger than this tints only the triangle hit
overlayhud::Hover HoverAt (float x, float y)
{
    overlayhud::Hover none;
    if (!overlayvisibility::ContentShown ())
        return none;
    PlanViewTransform read;
    {
        const OwnAcapi own;
        read = ReadPlanViewTransform (g_logicalWidth, g_logicalHeight);
    }
    if (!read.valid)
        return none;
    const plancontent::PixelTransform t = ToPhysical (read);
    const double det = t.xx * t.yy - t.xy * t.yx;
    if (std::fabs (det) < 1e-18)
        return none;
    const double px = double (x) - t.ox, py = double (y) - t.oy;
    const std::vector<std::shared_ptr<const overlaylayers::Layer>> layers = overlaycontrol::ShownLayers ();
    const overlayhover::Hit hit =
        overlayhover::PickPlan (layers, (t.yy * px - t.xy * py) / det, (t.xx * py - t.yx * px) / det);
    if (!hit.found)
        return none;
    const overlaylayers::Mesh& mesh = layers[hit.layer]->meshes[hit.mesh];
    overlayhud::Hover hover = overlayhover::Readout (*layers[hit.layer], hit);
    const bool whole = mesh.values.empty () && mesh.indices.size () / 3 <= kTintTriangles;
    // In model metres: the plan guest draws it through the transform it reads at Present.
    overlayhover::TintModel (mesh, hover.tintModel, whole ? -1 : int64_t (hit.triangle));
    return hover;
}

bool Hovering ()
{
    return overlayvisibility::Hovering ();
}

// The input layer's refresh: the HUD laid out again for the pointer, uploaded when what
// it draws changed.
std::string g_lastHudError;
// The pointer for the HUD's layout, with what hover mode reads under it: the presses since
// the last layout handed over (`take`), or none -- a heartbeat's layout replays no press.
overlayhud::Input HudInput (bool take)
{
    overlayhud::Input input = take ? overlayinput::TakeInput (overlayinput::View::Plan)
                                   : overlayinput::CurrentInput (overlayinput::View::Plan);
    if (!take)
        input.buttons.clear ();
    if (overlayvisibility::Hovering ()) {
        if (input.pointer)
            input.hover = HoverAt (input.x, input.y);
        input.hover.picks = true; // the plan reads under the pointer: its readout may say "nothing"
    }
    // ⚠️ LOCKED, THE FLOOR PLAN IS EDITED ON THE VIEW (the user, 2026-10-10): the HUD is handed the
    // plan's transform, read here as HoverAt reads it, to place the pointer in model metres.
    if (overlayhud::EditLocked (*guesttext::HudState ()) && overlayvisibility::ContentShown ()) {
        PlanViewTransform read;
        {
            const OwnAcapi own;
            read = ReadPlanViewTransform (g_logicalWidth, g_logicalHeight);
        }
        if (read.valid) {
            const plancontent::PixelTransform t = ToPhysical (read);
            input.planar = true;
            const double plan[6] = { t.xx, t.xy, t.ox, t.yx, t.yy, t.oy };
            std::copy (std::begin (plan), std::end (plan), input.plan);
        }
    }
    return input;
}

bool RefreshHud ()
{
    if (!g_running)
        return false;
    bool changed = false;
    std::string error;
    const overlayhud::Input input = HudInput (true);
    // The HUD is there with or without a layer: the overlay runs, its own pages say what.
    overlayhudmodel::Prepare (overlayinput::View::Plan);
    if (!dxgi::planguest::RefreshHud (overlaylayers::Layers (), input, changed, error)) {
        if (error != g_lastHudError)
            ArchVizLog ("PLAN OVERLAY  the HUD NOT LAID OUT for the pointer: " + error);
        g_lastHudError = error;
        return false;
    }
    // Every time, not only when the pixels changed: the hand can change without them.
    overlayinput::SetHitMap (overlayinput::View::Plan, dxgi::planguest::HitMap ());
    // What the user did there may be the dock's circle or a layer hidden.
    overlaycontrol::FollowHudState ();
    return changed;
}

// The input layer's paced redraw: a plan the HUD's moves are taken from presents nothing.
void RedrawHud ()
{
    if (g_running && PlanInFront ())
        Redraw ();
}

// The HUD takes its input from the canvas the session composes into; a failure is said
// once and costs the overlay nothing else.
void AttachHudInput ()
{
    std::string error;
    overlayinput::HudOwner owner;
    owner.shown = &HudShown;
    owner.refresh = &RefreshHud;
    owner.redraw = &RedrawHud;
    owner.hovering = &Hovering;
    if (!overlayinput::Attach (overlayinput::View::Plan, g_canvas, owner, error))
        ArchVizLog ("PLAN OVERLAY  the HUD takes no input: " + error);
}

void CALLBACK TickProc (HWND, UINT, UINT_PTR, DWORD)
{
    if (!g_running)
        return;
    std::string error;
    const bool inFront = Untouched () && PlanInFront ();
    // ⚠️ THE CANVAS IS ASKED FOR AGAIN, ONCE A SECOND, AND AT ONCE WHEN IT HAS GONE. A
    // plan canvas Archicad replaced would otherwise leave the layer matching a window
    // nobody presents into -- no frame drawn and nothing said. The same finder Start
    // used, the same answer; only a canvas gone with no plan in front ends the session.
    const bool gone = g_canvas == nullptr || !::IsWindow (g_canvas);
    const ULONGLONG checked = ::GetTickCount64 ();
    if (inFront && (gone || checked - g_lastTargetCheckMs >= kReportMs)) {
        g_lastTargetCheckMs = checked;
        viewportoverlay::OverlayTarget target;
        {
            const OwnAcapi own;
            target = viewportoverlay::FindOverlayTarget ();
        }
        if (target.valid && target.window != nullptr && target.window != g_canvas) {
            ArchVizLog ("PLAN OVERLAY  the plan canvas changed: " + Hex (uint64_t (uintptr_t (g_canvas))) + " '" +
                        g_canvasClass + "' -> " + Hex (uint64_t (uintptr_t (target.window))) + " '" +
                        target.windowClass + "'");
            g_canvas = target.window;
            g_canvasClass = target.windowClass;
            layer::Retarget (uint64_t (uintptr_t (g_canvas)));
            AttachHudInput ();
        }
    }
    if (g_canvas == nullptr || !::IsWindow (g_canvas)) {
        Teardown ("the plan canvas was destroyed", false);
        return;
    }
    MeasureCanvas (error); // a minimised canvas keeps its last good size

    short storey = 0;
    if (inFront && ReadStorey (storey) && (!g_storeyKnown || storey != g_storey)) {
        const short was = g_storey;
        g_storey = storey;
        g_storeyKnown = true;
        error.clear ();
        if (ReadContent (error))
            ArchVizLog ("PLAN OVERLAY  storey " + std::to_string (was) + " -> " + std::to_string (storey) + ": " +
                        std::to_string (g_content.rings) + " rings, " + std::to_string (g_content.segments.size ()) +
                        " segments");
        else
            ArchVizLog ("PLAN OVERLAY  storey " + std::to_string (storey) +
                        " came forward and its walls could not be read: " + error);
    }

    bool changed = false;
    error.clear ();
    const layer::Prepared prepared = layer::Prepare (g_content, g_generation, changed, error);
    const ULONGLONG now = ::GetTickCount64 ();
    if (prepared == layer::Prepared::Failed) {
        if (error != g_lastError)
            ArchVizLog ("PLAN OVERLAY  NOT DRAWING: " + error);
        g_lastError = error;
    }
    else if (prepared == layer::Prepared::WaitingForDevice) {
        // A plan that has not presented since the layer let a device go: ask for one.
        if (now - g_lastDeviceRedrawMs >= kDeviceRedrawMs) {
            g_lastDeviceRedrawMs = now;
            g_redrawPending = true;
        }
    }
    else if (changed) {
        g_lastError.clear ();
        if (!g_readyLogged) {
            const layer::Stats stats = layer::GetStats ();
            ArchVizLog ("PLAN OVERLAY  ready on the canvas's chain " + Hex (stats.chain) + ": " +
                        std::to_string (stats.segments) + " segments");
            g_readyLogged = true;
        }
        // Something new to draw on a plan that may not present again by itself.
        g_redrawPending = true;
    }
    // ⚠️ THE CALLER'S LAYERS (Tapioca.SetOverlayLayer), rebuilt only when the store
    // moved: preparing them is a walk over every point, and the tick runs ten times a
    // second whether anything changed or not.
    if (prepared == layer::Prepared::Ready) {
        const uint64_t layersGeneration = overlaylayers::Generation ();
        if (!layer::HoldsLayers (layersGeneration)) {
            // The layers the user shows (the HUD's Settings hides the others).
            const overlaylayers::Prepared2D layers = overlaylayers::Prepare2D (overlaycontrol::ShownLayers ());
            bool layersChanged = false;
            error.clear ();
            if (layer::PrepareLayers (layers, layersGeneration, layersChanged, error)) {
                if (layersChanged) {
                    ArchVizLog ("PLAN OVERLAY  layers: " + std::to_string (layers.strokes.size ()) + " strokes, " +
                                std::to_string (layers.fills.size () / 3) + " filled triangles");
                    g_redrawPending = true;
                }
            }
            else if (error != g_lastError) {
                ArchVizLog ("PLAN OVERLAY  layers NOT DRAWN: " + error);
                g_lastError = error;
            }
        }
        // ⚠️ WHAT THE DILIGENT GUEST DRAWS -- texts, dimensions, legends, styled and
        // heatmap meshes, dashed polylines -- made here, outside any Present, and only
        // when the store moved (§11). With none of those, nothing attaches.
        bool guestChanged = false;
        error.clear ();
        overlayhudmodel::Prepare (overlayinput::View::Plan);
        if (dxgi::planguest::Prepare (layer::Device (), overlaycontrol::ShownLayers (), overlaylayers::Layers (),
                                      layersGeneration, float (g_dpi),
                                      overlayinput::CurrentInput (overlayinput::View::Plan), guestChanged, error)) {
            if (guestChanged) {
                overlayinput::SetHitMap (overlayinput::View::Plan, dxgi::planguest::HitMap ());
                const dxgi::planguest::Stats guest = dxgi::planguest::GetStats ();
                ArchVizLog ("PLAN OVERLAY  guest: " + std::to_string (guest.fills / 3) + " filled triangles, " +
                            std::to_string (guest.lines) + " lines, " + std::to_string (guest.glyphVertices / 6) +
                            " glyph quads, " + std::to_string (guest.pages) + " atlas pages" +
                            (guest.attached
                                 ? "; attached to Archicad's device in " + std::to_string (guest.attachMilliseconds) +
                                       " ms, built in " + std::to_string (guest.buildMilliseconds) + " ms"
                                 : std::string ()) +
                            (guest.textsNotLaidOut + guest.dimensionsNotResolved + guest.truncated > 0
                                 ? "; NOT DRAWN: " + std::to_string (guest.textsNotLaidOut) + " texts, " +
                                       std::to_string (guest.dimensionsNotResolved) + " dimensions, " +
                                       std::to_string (guest.truncated) + " past the budget (" + guest.lastError + ")"
                                 : std::string ()));
                g_redrawPending = true;
                g_lastGuestError.clear ();
            }
        }
        else if (error != g_lastGuestError) {
            ArchVizLog ("PLAN OVERLAY  guest NOT DRAWING: " + error);
            g_lastGuestError = error;
        }
        // ⚠️ THE OWN PAGES MOVE WITHOUT THE POINTER, twice a second; a change asks for a frame
        // except on Debug (OverlayController.cpp's heartbeat says why).
        if (++g_hudBeats % 5 == 0 && overlayhud::HudOpen (*guesttext::HudState ())) {
            bool hudChanged = false;
            error.clear ();
            if (dxgi::planguest::RefreshHud (overlaylayers::Layers (), HudInput (false), hudChanged, error) &&
                hudChanged) {
                overlayinput::SetHitMap (overlayinput::View::Plan, dxgi::planguest::HitMap ());
                if (overlayhud::SelectedKey (*guesttext::HudState ()) != hudshell::kDebugKey)
                    g_redrawPending = true;
            }
        }
    }
    if (g_redrawPending && inFront) {
        g_redrawPending = false;
        Redraw ();
    }
    Report (false);
}

StartResult Refuse (StartError code, const std::string& message)
{
    StartResult result;
    result.code = code;
    result.message = message;
    ArchVizLog (std::string ("PLAN OVERLAY  NOT STARTED (") + StartErrorName (code) + ") - " + message);
    return result;
}

} // namespace

void SetContentReader (ContentReader reader)
{
    g_contentReader = reader;
}

const char* StartErrorName (StartError error)
{
    switch (error) {
        case StartError::None:
            return "None";
        case StartError::AlreadyRunning:
            return "AlreadyRunning";
        case StartError::NotFloorPlan:
            return "NotFloorPlan";
        case StartError::RecordRunning:
            return "RecordRunning";
        case StartError::Blocked:
            return "Blocked";
        case StartError::NoCanvas:
            return "NoCanvas";
        case StartError::CanvasSize:
            return "CanvasSize";
        case StartError::NoContentReader:
            return "NoContentReader";
        case StartError::Content:
            return "Content";
        case StartError::PresentHook:
            return "PresentHook";
        case StartError::Timer:
            return "Timer";
    }
    return "?";
}

StartResult Start ()
{
    if (g_running)
        return Refuse (StartError::AlreadyRunning, "the plan overlay is already running");
    if (!PlanInFront ())
        return Refuse (StartError::NotFloorPlan, "bring the floor plan to the front first");
    const planframes::SessionState record = planframes::GetStatus ().state;
    if (record == planframes::SessionState::Recording || record == planframes::SessionState::Closing ||
        record == planframes::SessionState::Analysing)
        return Refuse (StartError::RecordRunning, "a plan frame record is running: it measures Archicad's own "
                                                  "frames, so the overlay waits until it has finished");
    if (experimentguard::Blocked ())
        return Refuse (StartError::Blocked, experimentguard::WhyBlocked ());
    if (g_contentReader == nullptr)
        return Refuse (StartError::NoContentReader,
                       "no plan content reader is registered; the add-on registers it when it loads");
    const viewportoverlay::OverlayTarget canvas = viewportoverlay::FindOverlayTarget ();
    if (!canvas.valid || canvas.window == nullptr)
        return Refuse (StartError::NoCanvas, "the plan canvas could not be found: " + canvas.how);

    // Every Start resets what every Stop leaves behind (§8).
    Teardown ("", false);
    g_canvas = canvas.window;
    g_canvasClass = canvas.windowClass;
    g_storeyKnown = false;
    g_readyLogged = false;
    g_redrawPending = false;
    g_hudBeats = 0;
    g_lastError.clear ();
    g_reported = layer::Stats {};
    g_content = plancontent::Content {};
    g_lastDeviceRedrawMs = 0;
    g_lastTargetCheckMs = 0;
    std::string error;
    if (!MeasureCanvas (error))
        return Refuse (StartError::CanvasSize, error);
    g_mainThread = uint32_t (::GetCurrentThreadId ());
    g_storeyKnown = ReadStorey (g_storey);
    if (!ReadContent (error))
        return Refuse (StartError::Content, "the storey's walls could not be read: " + error);
    PlanViewTransform seed;
    {
        const OwnAcapi own;
        seed = ReadPlanViewTransform (g_logicalWidth, g_logicalHeight);
    }

    if (!experimentguard::Arm ("planoverlay", error))
        return Refuse (StartError::Blocked, error);
    g_breadcrumb = true;
    layer::Arm (uint64_t (uintptr_t (g_canvas)), g_mainThread, &ReadAtPresent, layer::Style {});
    if (seed.valid)
        layer::SeedTransform (ToPhysical (seed));
    // ⚠️ OURS ONLY IF WE PUT IT THERE. A hook already installed belongs to whoever
    // installed it, and taking it out at our Stop would take theirs with it.
    if (!dxgi::PresentHookInstalled ()) {
        if (!dxgi::InstallPresentHook (error)) {
            Teardown ("", false);
            return Refuse (StartError::PresentHook, error);
        }
        g_presentHookOurs = true;
    }
    g_timer = ::SetTimer (nullptr, 0, kTickMs, &TickProc);
    if (g_timer == 0) {
        const std::string why = "SetTimer failed with GetLastError " + std::to_string (::GetLastError ());
        Teardown ("", false);
        return Refuse (StartError::Timer, why);
    }
    g_running = true;
    g_startedMs = ::GetTickCount64 ();
    AttachHudInput ();
    g_lastReportMs = g_startedMs;

    char scale[16] = {};
    std::snprintf (scale, sizeof (scale), "%.2f", g_dpi);
    ArchVizLog ("PLAN OVERLAY  started over the plan canvas " + Hex (uint64_t (uintptr_t (g_canvas))) + " '" +
                g_canvasClass + "' " + std::to_string (g_canvasWidth) + "x" + std::to_string (g_canvasHeight) +
                " physical, " + std::to_string (g_logicalWidth) + "x" + std::to_string (g_logicalHeight) +
                " logical at scaling " + scale + "; storey " + (g_storeyKnown ? std::to_string (g_storey) : "?") +
                ": " + std::to_string (g_content.rings) + " rings, " + std::to_string (g_content.segments.size ()) +
                " segments; Present hook " + (g_presentHookOurs ? "installed by this session" : "already installed") +
                "; seed read " + (seed.valid ? "valid" : "INVALID (error " + std::to_string (seed.error) + ")") +
                ". Content is the storey's walls as they are now: edits are not followed until the overlay is "
                "started again");
    // The chain presents, and the layer learns the device it presents with.
    Redraw ();
    StartResult result;
    result.ok = true;
    return result;
}

void Stop (const char* reason)
{
    Teardown (reason != nullptr ? reason : "stopped", true);
}

void Shutdown ()
{
    Teardown ("shut down", false);
}

bool Running ()
{
    return g_running;
}

Status GetStatus ()
{
    Status status;
    status.running = g_running;
    status.canvasClass = g_canvasClass;
    status.canvasWidth = g_canvasWidth;
    status.canvasHeight = g_canvasHeight;
    status.dpi = g_dpi;
    status.storey = g_storey;
    status.rings = g_content.rings;
    status.segments = uint32_t (g_content.segments.size ());
    status.generation = g_generation;
    const layer::Stats stats = layer::GetStats ();
    status.canvasPresents = stats.canvasPresents;
    status.drawn = stats.drawn;
    status.readsFresh = stats.readsFresh;
    status.drawnWithLastRead = stats.drawnWithLastRead;
    status.lastError = g_lastError;
    return status;
}

} // namespace planruntime
} // namespace archviz
} // namespace geomsrv
