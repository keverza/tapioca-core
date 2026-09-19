// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.
// ArchViz/InjectedOverlayRuntime -- see the header. Every rule about this file is
// in that header's comments; this is the mechanism.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/InjectedOverlayRuntime.hpp"

#include "ArchViz/CameraSyncMode.hpp"
#include "ArchViz/Dxgi/CameraCensus.hpp"
#include "ArchViz/Dxgi/CameraRecognizer.hpp"
#include "ArchViz/Dxgi/ContextHook.hpp"
#include "ArchViz/Dxgi/HostOccluders.hpp"
#include "ArchViz/Dxgi/InjectedDiligentContext.hpp"
#include "ArchViz/Dxgi/GhostMesh.hpp"
#include "ArchViz/Dxgi/HostOverlay.hpp"
#include "ArchViz/Dxgi/CameraFreshness.hpp"
#include "ArchViz/OverlayRedrawBudget.hpp"
#include "ArchViz/OverlayRuntimeReport.hpp"
#include "ArchViz/Dxgi/OverlayComposer.hpp"
#include "ArchViz/ModelWatch.hpp"
#include "ArchViz/Dxgi/InjectionDepth.hpp"
#include "ArchViz/Dxgi/InjectionRenderer.hpp"
#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/ArchVizPanel.hpp"
#include "ArchViz/ExtractionThread.hpp"
#include "ArchViz/PatchProfile.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace overlayruntime {

namespace {

namespace cen = dxgi::census;
namespace inj = dxgi::injection;
namespace host = dxgi::hostocclusion;
namespace ho = dxgi::hostoverlay;
namespace composer = dxgi::overlaycompose;
namespace ghost = dxgi::ghost;
namespace depth = dxgi::injection::depth;

bool g_running = false;
bool g_visible = false;
// ⚠️ THE ARMING IS NOT FINISHED WHEN `Start` RETURNS, and pretending otherwise is
// what would make this a command rather than a service. The context hook cannot
// install until the Present detour has identified Archicad's swap chain, which
// takes about sixty of Archicad's frames -- and if the 3D window has never been
// drawn there is nothing to identify at all. `Tick` finishes the job.
bool g_armPending = false;
bool g_hostRequested = false;
// Whether THIS runtime armed the model watch, so stopping the overlay does not
// stop a watch the portable viewport is relying on.
bool g_modelWatchStarted = false;
// ⚠️ HOW MANY REDRAWS THIS STALL HAS ALREADY ASKED FOR.
// Present re-raises the request on every suppressed frame, so without a bound a
// stall that Archicad cannot answer would ask forever at four requests a second.
// The count resets the moment anything composes, because that IS the answer.
// ⚠️ THE 3D WINDOW THIS SESSION SERVES, NOT WHICHEVER ONE IS
// IN FRONT. `APIdefs_Database.h` says `typeID` identifies the Floor Plan and 3D
// Model databases and `index` is the window index, so the pair is the session
// identity. Resizing one 3D window and switching away before the heartbeat fires
// must not send that repair to whatever is in front now.
uint64_t g_reacquisitions = 0;
CameraState g_lastCamera = CameraState::Unavailable;
HostState g_lastHost = HostState::Idle;
uint64_t g_lastDraws = 0;
bool g_anchored = false;
std::string g_anchorHow;
std::string g_lastChain;
StartError g_lastError = StartError::None;
std::string g_lastMessage;

// ⚠️ THE RUNTIME OWNS ITS OWN HEARTBEAT AND BORROWS NOBODY ELSE'S.
// The camera-sync timer belongs to the portable overlay's mode machinery, and
// stage 9d is the record of what sharing a lifecycle with that costs: opening the
// Diligent overlay changed the sync mode, whose teardown called
// `census::Shutdown`, which dropped a camera fingerprint that had just been
// selected. A timer is cheap; a shared lifecycle is not.
//
// Quarter of a second: this only retries arming and notices state transitions,
// and nothing a user can see depends on it firing sooner.
UINT_PTR g_timer = 0;
constexpr UINT kTickMs = 250;

// ⚠️ WHO DELIVERS THE HEARTBEAT IS A MEASUREMENT, NOT A
// DETAIL, BECAUSE THE MENU AND THE COMMAND BEHAVE DIFFERENTLY AND ARM
// IDENTICALLY. The 2026-09-18 log carries `camera sync mode: hookdiag at 33 ms,
// hideOnNav off, gpuState on` for BOTH, sixty-seven times, so the difference is
// not in the arming. It is here: `Tick` has two callers -- this timer, and
// `Tapioca.OverlayRuntime`, which a diagnostic polls in a loop. The menu has
// only the timer.
//
// ⚠️ AND A `WM_TIMER` IS THE LOWEST-PRIORITY MESSAGE
// WINDOWS HAS. `CameraWake.hpp` wrote that down after measuring it: "synthesised
// only when the queue has nothing else in it ... during a drag Archicad's queue
// is never empty, so the timer is served last however short its interval". A
// heartbeat that stops during navigation is a heartbeat that stops exactly when
// the overlay is being asked to follow something -- and `OverlayRedrawBudget::
// Consider`, the only thing that ends a suppressed frame, rides on it.
//
// ⚠️ `timerTicks` AGAINST `ticks` IS SECTION 9 AS A
// NUMBER. "Production must not depend on a diagnostic having run": if a session
// is healthy only while something else is polling the command, the two counters
// say so outright instead of leaving it to be argued from symptoms.
uint64_t g_ticks = 0;
uint64_t g_timerTicks = 0;
uint64_t g_lastTickMs = 0;
uint32_t g_tickGapMaxMs = 0;
uint64_t g_tickGapsOverASecond = 0;

void CALLBACK TickProc (HWND, UINT, UINT_PTR, DWORD)
{
    // ⚠️ THE SAME GUARD `Tick` USES, BECAUSE THE SPLIT MUST
    // NOT GO NEGATIVE. `StopHeartbeat` kills the timer but a WM_TIMER already in
    // the queue still arrives, and `Tick` returns early on it -- so counting it
    // here unconditionally would leave `timerTicks > ticks` and the report's
    // `ticks - timerTicks` would print an unsigned underflow as the caller's
    // share.
    if (g_running)
        ++g_timerTicks;
    Tick ();
}

void StartHeartbeat ()
{
    if (g_timer == 0)
        g_timer = ::SetTimer (nullptr, 0, kTickMs, TickProc);
}

void StopHeartbeat ()
{
    if (g_timer != 0) {
        ::KillTimer (nullptr, g_timer);
        g_timer = 0;
    }
}

// The census gate wants a median area of 50 px squared and a longest edge of
// 10 px: a transform that collapses the primitive to a point passes any test
// that looks at one corner. A fixed size is invisible across a site plan and
// fills the screen in a bathroom, so it scales -- clamped at both ends.
float ClampAnchorSize (float size)
{
    if (!(size > 0.25f))
        return 0.25f;
    return size > 50.0f ? 50.0f : size;
}

void Remember (StartError code, const std::string& message)
{
    g_lastError = code;
    g_lastMessage = message;
}

CameraState ReadCameraState ()
{
    // ⚠️ A CAMERA MEASURED FOR A WINDOW THAT NO LONGER EXISTS
    // IS REACQUIRING, WHATEVER THE LIFECYCLE SAYS. The lifecycle asks whether the
    // pinned family has drawn recently -- and after a resize with no navigation
    // NOTHING draws, so the model generation does not advance either and it
    // reports `Locked` for a camera that cannot be used. Guidance section 5
    // names this transition explicitly.
    if (inj::freshness::Stale ())
        return CameraState::Reacquiring;
    const cen::Stats stats = cen::GetStats ();
    const std::string lifecycle = stats.lifecycle != nullptr ? stats.lifecycle : "Unknown";
    if (lifecycle == "Locked")
        return CameraState::Locked;
    if (lifecycle == "Reacquiring")
        return CameraState::Reacquiring;
    if (lifecycle == "Learning")
        return CameraState::Learning;
    return CameraState::Unavailable;
}

HostState ReadHostState ()
{
    const host::Stats stats = host::GetStats ();
    if (stats.haveSnapshot && stats.publishedTriangles > 0)
        return ExtractionWorker::Get ().IsRunning () ? HostState::Dirty : HostState::Ready;
    if (ExtractionWorker::Get ().IsRunning () || stats.batchBegins > stats.batchEnds)
        return HostState::Extracting;
    return HostState::Idle;
}

// MAIN THREAD. Point the census's scorer at something certainly on screen.
//
// ⚠️ WITHOUT AN ANCHOR THE CAMERA CAN NEVER LOCK, AND THE FIRST
// VERSION OF THIS RETURNED SILENTLY WHEN IT COULD NOT SET ONE. The census does
// not rank draw groups by what they look like: it PROJECTS a world-space triangle
// through each candidate's own bytes and scores where it lands. The anchor
// defaults to the world ORIGIN with a one-metre triangle, so on any model not
// sitting on 0,0,0 every group scores `anchor inside clip: 0%`, nothing clears
// the eligibility gate, `SetAutoSelect` never fires, and the overlay never draws
// while the log looks perfectly healthy. Run sixty-two reached `Learning` and
// stayed there for a whole session on exactly that.
//
// ⚠️ THE EXTRACTED MODEL'S OWN CENTRE IS THE ANCHOR, AND ASKING
// ARCHICAD FOR A CAMERA IS THE FALLBACK RATHER THAN THE OTHER WAY ROUND. The
// building is where the user is looking, by construction, whatever the
// projection; a camera read adds a dependency on projection settings for a number
// the geometry already answers. The camera path stays only for the moments before
// the first extraction publishes.
bool PointAnchorAtView (std::string& how)
{
    const host::Stats stats = host::GetStats ();
    if (stats.boundsValid) {
        const float centre[3] = { (stats.boundsMin[0] + stats.boundsMax[0]) * 0.5f,
                                  (stats.boundsMin[1] + stats.boundsMax[1]) * 0.5f,
                                  (stats.boundsMin[2] + stats.boundsMax[2]) * 0.5f };
        const float dx = stats.boundsMax[0] - stats.boundsMin[0];
        const float dy = stats.boundsMax[1] - stats.boundsMin[1];
        const float dz = stats.boundsMax[2] - stats.boundsMin[2];
        const float diagonal = std::sqrt (dx * dx + dy * dy + dz * dz);
        cen::SetAnchor (centre[0], centre[1], centre[2], ClampAnchorSize (diagonal * 0.1f));
        how = "model centre";
        return true;
    }

    const CameraStart camera = ArchVizPanel::ReadArchicadCamera ();
    if (camera.valid && !camera.orthographic) {
        const float dx = camera.eye[0] - camera.target[0];
        const float dy = camera.eye[1] - camera.target[1];
        const float dz = camera.eye[2] - camera.target[2];
        const float distance = std::sqrt (dx * dx + dy * dy + dz * dz);
        cen::SetAnchor (camera.target[0], camera.target[1], camera.target[2], ClampAnchorSize (distance * 0.05f));
        how = "view target";
        return true;
    }

    how = "no extracted model yet and no readable camera";
    return false;
}

// MAIN THREAD. Is Archicad's 3D model window the one in front?
bool FrontWindowIs3D ()
{
    API_WindowInfo info = {};
    if (ACAPI_Window_GetCurrentWindow (&info) != NoError)
        return false;
    return info.typeID == APIWind_3DModelID;
}

// ⚠️ THE OVERLAY IS ARMED IN ONE PLACE AND `Tick` MAY RUN IT AGAIN.
// Arming is idempotent by construction: every call below either sets a flag that
// is already set or requests a mode that is already current.
StartResult Arm ()
{
    std::string error;
    if (!SetCameraSyncMode (CameraSyncMode::HookDiag, CurrentCameraSyncIntervalMs (), CurrentPredictionScale (),
                            CurrentHideOnNav (), true, error)) {
        // ⚠️ `patchprofile::HasPin`, NOT `ContextHookStats::pinned`.
        // The latter means "a profile was verified AT INSTALL TIME" and is false
        // until the hook has installed -- which is what we are trying to do. Run
        // sixty read it here, before anything had armed, and reported
        // `BuildNotPinned` on a build whose own diagnostic printed
        // `pinned build : 29.0.0.5101` in the same session. The menu then fell
        // back to the portable overlay every time, correctly implementing a
        // conclusion that was wrong.
        StartResult result;
        result.message = error.empty () ? std::string ("the GPU-state hooks would not arm") : error;
        if (!patchprofile::HasPin ()) {
            // Fails closed, and not retryable: the profile fingerprints every
            // hooked slot and refuses an Archicad it was not measured against.
            result.code = StartError::BuildNotPinned;
            result.retryable = false;
        }
        else {
            // ⚠️ AND THE REAL MESSAGE SURVIVES EITHER WAY. The
            // previous version replaced it with a guess about pinning and threw
            // the actual refusal away, so the one line that said WHY never
            // reached a log.
            result.code = StartError::HooksRefused;
            result.retryable = true;
        }
        report::Say ("HOOK", std::string ("REFUSED (") + StartErrorName (result.code) + ") - " + result.message);
        return result;
    }

    // ⚠️ THE CONSTANT-BUFFER SLOTS STAY OFF IN PRODUCTION. Reading mapped upload
    // buffers is the most expensive thing this subsystem does and the overlay
    // needs none of it -- the shader reads Archicad's constants on the GPU. What
    // must be on is the cheap state: viewports, targets, clears and draws.
    dxgi::SetContextSlotEnabled (dxgi::ContextSlot::Map, false);
    dxgi::SetContextSlotEnabled (dxgi::ContextSlot::Unmap, false);
    dxgi::SetContextSlotEnabled (dxgi::ContextSlot::UpdateSubresource, false);

    // ⚠️ THE CENSUS IS ON AND CHOOSES FOR ITSELF. This is the whole difference
    // between the runtime and the diagnostic: no phase A, no orbit instruction,
    // no ranking table, no hand-made selection. See `census::SetAutoSelect`.
    std::string how;
    if (PointAnchorAtView (how))
        report::Say ("CAMERA", "anchor acquired from the " + how);
    else
        report::Say ("CAMERA", "NO ANCHOR YET - " + how + "; the census cannot score until there is one");
    // ⚠️ THE TABLE IS CLEARED BEFORE LEARNING, AS THE DIAGNOSTIC HAS
    // ALWAYS DONE. `ViewerCameraCensus {enabled, reset, clearSelection}` is the
    // working invocation; `Arm` enabled the census and never reset it, so a
    // second activation started against up to 48 groups left by the first --
    // stale COM pointers, a stale viewport, sample counts from another gesture.
    // The table has a hard ceiling, so a full one of those means every CURRENT
    // draw overflows and nothing actually on screen is ever scored.
    //
    // ⚠️ AND IT IS THE THIRD FAULT OF ONE SHAPE HERE: state that
    // outlives the session it describes -- `g_hostRequested` across `Stop`,
    // `g_lastAutoSelectAttempt` across `ResetCounts`, and now the census table
    // across an arm. A diagnostic never sees any of them, because it resets
    // everything on the way in.
    // ⚠️ EVERY COUNTER THIS SESSION WILL BE JUDGED BY, AND
    // THE CAMERA SOURCE WITH THEM. A restart reported `snapshots=466
    // present=2959` one second in, and `BlockedAt` walks the chain in order:
    // a stale `presentInjections` carried it past `Present:NeverInjected` and
    // reported `Compose:NoDrawAndNoReason` for a session in which Present had
    // never injected once. The chain was not wrong about the numbers; the
    // numbers described a session that had ended.
    inj::BeginSession ();
    host::ResetRenderCounters ();
    // Section 8: every `Start` resets what every `Stop` leaves behind. The
    // BACKEND SELECTION IS DELIBERATELY NOT RESET here -- a caller sets it before
    // starting, and clearing it would make the switch impossible to use.
    dxgi::injecteddiligent::ResetCounters ();
    cen::Reset ();
    cen::SetEnabled (true);
    cen::SetAutoSelect (true);

    // ⚠️ THE PRODUCTION STATE IS SET EXPLICITLY, NOT INHERITED. This
    // is the correction for a mistake made three times in this series: the
    // runtime was written to call a SUBSET of what the working diagnostic calls,
    // on the assumption that the rest was diagnostic-only ceremony. It is not.
    // `ViewerInjectTriangle` sets the depth mode and source on every arm, and
    // their defaults (`Mode::Off`) are values only the diagnostic ever moved off
    // -- so the overlay's state depended on a test having run first.
    //
    // ⚠️ `PrivateCopy` IS THE PRODUCTION SHAPE, settled by runs
    // forty-six and forty-eight: Archicad's depth copied into a texture we own,
    // our writes never reaching theirs. `AtPresent` is where this composes.
    depth::SetMode (depth::Mode::PrivateCopy);
    depth::SetSource (depth::Source::AtPresent);

    // ⚠️ ARMED PENDING CAMERA, WHICH DRAWS NOTHING UNTIL A MATCHING
    // DRAW IS SNAPSHOTTED. The render thread promotes it to `Active` on
    // evidence, so enabling here cannot put a primitive on screen with a camera
    // nobody has.
    // ⚠️ THE MENU OVERLAY DRAWS ONE THING: ARCHICAD'S OWN EDGES.
    // That is the whole product default -- a reference layer you look THROUGH,
    // over the model you are working on. It is also exactly enough to validate
    // that the overlay is composing at all, which is what the default is FOR.
    //
    // ⚠️ EVERYTHING ELSE IS PER-COMMAND. The deterministic ghost
    // mesh is a regression fixture with no meaning to a user; the surface
    // heatmap is an ANALYSIS layer and analysis is something a command asks for.
    // A menu item that silently draws both puts two opaque layers over the
    // drawing and hides the thing the overlay exists to show -- run fifty-nine
    // reported exactly that, the ramp lost behind a shaded overlay.
    //
    // The previous build armed the diagnostic's full set deliberately, to remove
    // the last variable while the menu path was still not composing. It composes
    // now, so the fixture comes off.
    // ⚠️ AND THE PROOF PRIMITIVES ARE OFF. The magenta anchor triangle
    // and the coloured probe wedges were drawn unconditionally, so promoting the
    // overlay to a menu item put three instruments over the user model. They are
    // settled questions and they belong to the diagnostic.
    inj::SetProofPrimitives (false);
    ghost::SetEnabled (false);
    ghost::SetStandIns (false);
    ho::SetEnabled (ho::Kind::Wireframe, true);
    ho::SetEnabled (ho::Kind::Heatmap, false);

    inj::SetEnabled (true);
    inj::SetPoint (inj::Point::Present);

    report::Say ("HOOK", "ready");
    StartResult result;
    result.ok = true;
    return result;
}

} // namespace

const char* StartErrorName (StartError code)
{
    switch (code) {
        case StartError::None:
            return "None";
        case StartError::WaitingFor3DContext:
            return "WaitingFor3DContext";
        case StartError::Not3DWindow:
            return "Not3DWindow";
        case StartError::BuildNotPinned:
            return "BuildNotPinned";
        case StartError::HooksRefused:
            return "HooksRefused";
        case StartError::DeviceUnsupported:
            return "DeviceUnsupported";
        case StartError::AlreadyRunning:
            return "AlreadyRunning";
        case StartError::Internal:
            return "Internal";
    }
    return "Internal";
}

const char* CameraStateName (CameraState state)
{
    switch (state) {
        case CameraState::Unavailable:
            return "Unavailable";
        case CameraState::Learning:
            return "Learning";
        case CameraState::Locked:
            return "Locked";
        case CameraState::Reacquiring:
            return "Reacquiring";
    }
    return "Unavailable";
}

const char* HostStateName (HostState state)
{
    switch (state) {
        case HostState::Idle:
            return "Idle";
        case HostState::Extracting:
            return "Extracting";
        case HostState::Ready:
            return "Ready";
        case HostState::Dirty:
            return "Dirty";
    }
    return "Idle";
}

StartResult Start ()
{
    if (g_running) {
        StartResult result;
        result.ok = true;
        result.code = StartError::AlreadyRunning;
        result.message = "the overlay runtime is already running";
        return result;
    }

    // ⚠️ THE WINDOW IS CHECKED BEFORE ANYTHING IS ARMED. See
    // `StartError::Not3DWindow`: a floor plan is a different entity, not a
    // smaller 3D scene, and arming a camera census against one would spend a
    // user's frames looking for a model camera that does not exist.
    if (!FrontWindowIs3D ()) {
        report::Say ("OVERLAY MENU", "refused: the front window is not Archicad's 3D model");
        StartResult result;
        result.code = StartError::Not3DWindow;
        result.message = "the front window is not Archicad's 3D model; the portable overlay "
                         "draws plans and is the renderer there";
        result.retryable = false;
        return result;
    }

    const StartResult armed = Arm ();
    g_running = true;
    g_visible = true;
    // Same fault class as `report::Reset` on the way out: a gap measured across
    // the pause BETWEEN two sessions is not a gap this session suffered.
    g_ticks = 0;
    g_timerTicks = 0;
    g_lastTickMs = 0;
    g_tickGapMaxMs = 0;
    g_tickGapsOverASecond = 0;
    StartHeartbeat ();
    g_armPending = !armed.ok;
    if (!armed.ok) {
        Remember (armed.code, armed.message);
        // ⚠️ A REFUSAL THAT IS NOT RETRYABLE STOPS THE RUNTIME HERE. Leaving it
        // "running" on a build the hooks will never install on would report a
        // state that can never advance, and the caller needs to reach for the
        // fallback instead.
        if (!armed.retryable) {
            g_running = false;
            g_visible = false;
            StopHeartbeat ();
            return armed;
        }
    }
    else {
        Remember (StartError::None, std::string ());
    }

    // ⚠️ THE BUILDING IS ASKED FOR AND NOBODY WAITS. Host extraction and camera
    // synchronisation are independent services: the overlay draws as soon as the
    // camera locks, with self-occlusion only, and semantic host occlusion turns
    // itself on when the snapshot publishes.
    //
    // ⚠️ AND IT IS NOT ASKED FOR TWICE. A snapshot already published -- by an
    // earlier session of this runtime, or by the portable overlay, which feeds
    // the same `SceneCmdQueue` tee -- is the building, and re-requesting would
    // discard it and re-walk the model for nothing.
    const host::Stats hostStats = host::GetStats ();
    if (!g_hostRequested && !(hostStats.haveSnapshot && hostStats.publishedTriangles > 0) &&
        !ExtractionWorker::Get ().IsRunning ()) {
        // One-shot, not live: a live pass arms database observers, which write
        // to the project. See ExtractionThread.hpp.
        ExtractionWorker::Get ().Start (true, 12, 4, 600);
        g_hostRequested = true;

        // ⚠️ AND FOLLOW THE MODEL FROM HERE ON, WHICH THE
        // OVERLAY DID NOT. One extraction per session meant a wall drawn after
        // the overlay started never appeared and an edited one kept its old
        // shape -- the snapshot was taken at activation and never revisited.
        //
        // ⚠️ `modelwatch` IS THE ESTABLISHED MECHANISM AND IT IS
        // NOT AN OBSERVER. `ACAPI_Element_AttachObserver` is a DATABASE WRITE
        // (PLAT-RE68): a viewer that merely watches would dirty the project and
        // make Archicad autosave. This polls Archicad's own difference generator
        // from a low-priority main-thread timer that backs off under load, skips
        // while a pass is in flight, and separates a genuine element edit from
        // the environment moving -- so orbiting does not trigger a rebuild. See
        // ModelWatch.hpp. The portable viewport has used it since PLAT-RE125;
        // the injected runtime simply never armed it.
    }

    // ⚠️ ARMED BECAUSE THE OVERLAY IS RUNNING, NOT BECAUSE AN
    // EXTRACTION WAS REQUESTED. These are two different decisions and tying them
    // together meant the watch was skipped on every activation that found a
    // snapshot already published -- which is every activation after the first,
    // because the building deliberately survives `Stop`. The overlay then
    // followed no edits at all for the rest of the process.
    if (!g_modelWatchStarted) {
        // ⚠️ KEEP-ALIVE BEFORE START, NOT AFTER. The first
        // tick can arrive before the next statement runs, and that tick is
        // exactly the one that used to stop the watch.
        modelwatch::SetKeepAlive (true);
        g_modelWatchStarted = modelwatch::Start (/*floorMs*/ 750);
    }

    StartResult result;
    result.ok = true;
    if (g_armPending) {
        result.code = StartError::WaitingFor3DContext;
        result.message = "waiting for the 3D viewport";
        result.retryable = true;
    }
    return result;
}

void SetOverlayBackend (bool diligent)
{
    namespace dil = dxgi::injecteddiligent;
    const dil::Backend wanted = diligent ? dil::Backend::Diligent : dil::Backend::NativeD3D11;
    if (dil::GetBackend () == wanted)
        return;
    // ⚠️ LET GO OF THE OLD ONE FIRST. Leaving Diligent
    // attached while the native path draws is the "two renderers in one
    // viewport" state section 12 forbids, and it would also keep a reference to
    // a back buffer `ResizeBuffers` needs released.
    dil::Detach ();
    dil::SetBackend (wanted);
    ArchVizLog (std::string ("overlay backend: ") +
                (diligent ? "DILIGENT (attaching on the next composition)" : "native D3D11 (the regression oracle)"));
}

void SetVisible (bool visible)
{
    g_visible = visible;
    if (!g_running)
        return;
    // ⚠️ ONE SWITCH, AND NOTHING ELSE MOVES. The census keeps scoring, the
    // recognizer keeps its fingerprint, the host snapshot stays on the GPU and
    // every shader stays compiled. Hiding an overlay is not a reason to re-learn
    // a camera and re-walk a building.
    inj::SetEnabled (visible);
}

bool Visible ()
{
    return g_running && g_visible;
}

bool Running ()
{
    return g_running;
}

void Tick ()
{
    if (!g_running)
        return;

    // ⚠️ THE GAP IS MEASURED WHOEVER CLOSED IT. The
    // question is not how often the timer fired, it is how long the runtime went
    // unattended -- so a tick the command supplied counts as a tick, and the
    // gap it closed is a gap that did not happen. `timerTicks` beside `ticks` is
    // what separates the two.
    const uint64_t nowMs = ::GetTickCount64 ();
    if (g_lastTickMs != 0) {
        const uint64_t gap = nowMs - g_lastTickMs;
        if (gap > g_tickGapMaxMs)
            g_tickGapMaxMs = uint32_t (gap);
        if (gap >= 1000)
            ++g_tickGapsOverASecond;
    }
    g_lastTickMs = nowMs;
    ++g_ticks;

    // ⚠️ WAITING FOR THE 3D VIEWPORT IS A STATE, NOT A FAILURE, AND THIS IS WHAT
    // ENDS IT. A user who opens the overlay before the 3D window has drawn gets
    // "waiting for the 3D viewport" and an overlay that arms itself the moment
    // the viewport appears -- rather than an error and an instruction to try
    // again, which is a worse version of the same wait.
    if (g_armPending) {
        const StartResult armed = Arm ();
        if (armed.ok) {
            g_armPending = false;
            Remember (StartError::None, std::string ());
            inj::SetEnabled (g_visible);
        }
        else if (!armed.retryable) {
            Remember (armed.code, armed.message);
            Stop ();
            return;
        }
        else {
            Remember (StartError::WaitingFor3DContext, "waiting for the 3D viewport");
        }
    }

    // ⚠️ REACQUISITIONS ARE COUNTED HERE BECAUSE THIS IS THE ONLY PLACE THAT
    // SEES THE TRANSITION. The recognizer knows its own state at any instant; it
    // does not know that the previous instant was different, and "how often did
    // this happen" is the number that says whether a session was healthy.
    // ⚠️ THE ANCHOR FOLLOWS THE VIEW WHILE THERE IS NO LOCK, AND
    // STOPS THE MOMENT THERE IS ONE. Panning moves the orbit target, so an anchor
    // fixed at arm time drifts off screen and the scores stop meaning anything.
    // Moving it AFTER a lock would be worse: the accumulated samples that won the
    // selection were measured against the old one, and mixing the two would make
    // a healthy group look like it had started failing.
    // ⚠️ THE ANCHOR FOLLOWS THE VIEW WHILE THERE IS NO LOCK, AND
    // FREEZES THE MOMENT THERE IS ONE. Panning moves the orbit target, so an
    // anchor fixed at arm time drifts off screen and the scores stop meaning
    // anything. Moving it AFTER a lock would be worse: the samples that won the
    // selection were measured against the old one.
    const CameraState pre = ReadCameraState ();
    if (pre == CameraState::Unavailable || pre == CameraState::Learning) {
        std::string how;
        const bool anchored = PointAnchorAtView (how);
        if (anchored != g_anchored || (anchored && how != g_anchorHow)) {
            report::Say ("CAMERA", anchored ? ("anchor acquired from the " + how)
                                            : ("NO ANCHOR - " + how + "; the census cannot score"));
            g_anchored = anchored;
            g_anchorHow = how;
        }
    }

    const CameraState camera = ReadCameraState ();
    if (camera == CameraState::Reacquiring && g_lastCamera == CameraState::Locked)
        ++g_reacquisitions;
    // ⚠️ EVERY TRANSITION IS LOGGED ONCE, AND THIS IS WHY. A user
    // clicked the menu and reported "did not initialise anything" -- which was
    // true, and which nothing recorded, because the only instrument was a
    // diagnostic command that had itself crashed. A menu path that can fail must
    // narrate itself; a state that never changes costs one comparison.
    if (camera != g_lastCamera) {
        // ⚠️ AND WHEN IT LOCKS, SAY WHAT IT LOCKED ON TO. The
        // gate reports the candidates it REFUSED and then fell silent about the
        // one it accepted, so "it locked and drew nothing visible" had no way to
        // become "it locked on to the wrong group". These are the numbers the
        // decision was made from, in the viewport it was measured in.
        if (camera == CameraState::Locked) {
            const cen::Selection chosen = cen::GetSelection ();
            char detail[220] = {};
            _snprintf_s (detail, sizeof (detail), _TRUNCATE,
                         "Locked g%u occ%u interp%u vp=%.0fx%.0f@%.0f,%.0f samples=%u coverage=%.0f%% "
                         "inside=%.0f%% centre=%.3f",
                         chosen.groupId, chosen.occurrenceIndex, chosen.variant, chosen.viewportWidth,
                         chosen.viewportHeight, chosen.viewportX, chosen.viewportY, chosen.samples,
                         chosen.modelCoverage * 100.0f, chosen.insideClip * 100.0f, chosen.medianCentreError);
            report::Say ("CAMERA", detail);
        }
        else {
            report::Say ("CAMERA", CameraStateName (camera));
        }
        g_lastCamera = camera;
    }
    const HostState hostState = ReadHostState ();
    if (hostState != g_lastHost) {
        report::Say ("HOST", std::string (HostStateName (hostState)) + " (" +
                                 std::to_string (host::GetStats ().publishedTriangles) + " opaque triangles)");
        g_lastHost = hostState;
    }
    // The one number that says whether anything is on screen at all.
    const uint64_t draws = ho::GetStats ().wireframeDraws + ho::GetStats ().heatmapDraws;
    if ((draws > 0) != (g_lastDraws > 0)) {
        // ⚠️ AND SAY HOW MANY LINES, BECAUSE "DRAWING" WAS NOT
        // ENOUGH. A run reported DRAWING while the screen showed nothing but probe
        // triangles: `wireframeDraws` counts passes ISSUED, and a pass that
        // uploaded no feature edges is still a pass. This is the number that
        // separates composing from composing something.
        if (draws > 0)
            report::Say ("OVERLAY", "DRAWING (" + std::to_string (ho::GetStats ().linesDrawn) + " lines)");
        else
            report::Say ("OVERLAY", "stopped drawing");
    }
    g_lastDraws = draws;

    // ⚠️ THE ONE REDRAW THAT ENDS THE STALL. Scaling a
    // window changes the swap chain without Archicad redrawing its model, so
    // the camera stays measured for a window that no longer exists and NOTHING
    // would produce a fresh one -- the overlay waits for the user to orbit,
    // which is precisely what was reported. Who may ask, and how often, is
    // `OverlayRedrawBudget`.
    redrawbudget::Consider (cen::GetStats ().modelFramesSeen);

    // ⚠️ EVERY TICK, NOT ONCE AT ARM. Stamping this in
    // `Start` read `geometryEdits` before any edit had happened, so the whole
    // revision chain reported `model=0 published=0 gpu=0` for the life of the
    // session while the watch counted edits beside it.
    const uint32_t revision = modelwatch::Get ().geometryEdits;
    host::SetModelRevision (revision);
    cen::NoteModelRevision (revision);

    const Health live = GetHealth ();
    report::Live (live);
    // ⚠️ THE CHAIN, WHICH 5cf72fa TOOK AWAY WHILE
    // SAYING IT CHANGED NOTHING -- and GATE is reached only through it, so
    // "no eligible candidate" could not say whether the gate refused or had
    // never seen enough samples. Silent while drawing, and only on a CHANGE.
    report::Chain (live);
    report::Watch (live);
    report::Backend (live);
    report::Pulse (live);
    report::Sync ();
}

// ⚠️ THE FIRST STAGE THAT IS NOT SATISFIED, IN ORDER, AND NOTHING
// DOWNSTREAM OF IT IS WORTH READING. Each of the last six runs stopped at a
// different one of these and every time the visible symptom was identical --
// "nothing on screen" -- so the symptom carried no information at all. This is
// the ordering the chain actually has, and the first gap is the diagnosis.
// ⚠️ THE CLOSEST CANDIDATE'S ACTUAL MEASUREMENTS, BESIDE THE
// THRESHOLDS THEY FAILED. A gate that says only "no" cannot be distinguished
// from a gate that is wrong.
std::string BlockedAt (const Health& health)
{
    if (!health.running)
        return "NotStarted";
    if (health.waitingForContext)
        return "Hook";
    if (health.modelFramesSeen == 0)
        return "NoModelFrames";
    // ⚠️ A DRAW THAT NEVER REACHED THE TABLE CANNOT FAIL A GATE.
    // `drawsQualified == 0` means no draw bound both camera windows as 256-byte
    // ranges -- a hook question, not a scoring one. A full table with overflow
    // means current draws were refused a slot by stale ones.
    if (health.drawsQualified == 0)
        return health.drawsSeen == 0 ? "Census:NoDrawsSeen" : "Census:NoDrawsQualified";
    if (health.groupsUsed == 0)
        return "Census:NoGroups";
    if (!health.selectionValid) {
        // ⚠️ "NEVER ATTEMPTED" AND "ATTEMPTED AND REFUSED" LOOKED
        // IDENTICAL, AND THE FIRST IS WHAT ACTUALLY HAPPENED. `eligible = 0` was
        // reported truthfully by a gate that had not been run.
        if (health.selectionAttempts == 0)
            return "Selection:NeverAttempted";
        return health.eligibleCandidates == 0 ? "Selection:NoEligibleCandidate" : "Selection:CommitRefused";
    }
    // ⚠️ A SELECTION WITH THE WRONG SOURCE IS THE STATE THAT MUST NO
    // LONGER EXIST. `SelectCandidate` now sets both as one transaction; if this
    // ever fires again, the transaction has been split somewhere.
    if (health.cameraSource != "CensusSelectedGroup")
        return "Selection:SourceNotCommitted";
    if (health.authoritativeSnapshots == 0)
        return health.logicalMatches == 0 ? "Snapshot:NoMatchingDraw" : "Snapshot:MatchedButNotTaken";
    if (health.armState != "Active")
        return "Camera:NotPromotedToActive";
    if (health.presentInjections == 0)
        return "Present:NeverInjected";
    if (health.overlayDraws == 0) {
        if (health.hostNoDepthTarget > 0)
            return "Compose:NoDepthTarget";
        if (health.hostNoGeometry > 0)
            return "Compose:NoHostGeometry";
        if (health.overlayNoEdges > 0)
            return "Compose:NoFeatureEdges";
        if (health.overlayNoCamera > 0)
            return "Compose:NoCameraSnapshot";
        return "Compose:NoDrawAndNoReason";
    }
    return "";
}

Health GetHealth ()
{
    Health health;
    health.running = g_running;
    health.visible = g_running && g_visible;
    health.waitingForContext = g_armPending;
    health.camera = g_running ? ReadCameraState () : CameraState::Unavailable;
    health.host = ReadHostState ();
    health.reacquisitions = g_reacquisitions;
    health.lastError = g_lastError;
    health.lastMessage = g_lastMessage;

    const cen::Stats census = cen::GetStats ();
    health.autoSelections = census.autoSelections;

    const host::Stats hostStats = host::GetStats ();
    health.hostOpaqueTriangles = hostStats.publishedTriangles;

    const ho::Stats overlayStats = ho::GetStats ();
    health.overlayDraws = overlayStats.heatmapDraws + overlayStats.wireframeDraws;

    // ⚠️ WHERE THE COMPOSITION STOPPED, NOT JUST THAT IT DID. "draws
    // 0" was true at three different depths of the call stack across three runs
    // and meant something different each time: Present never injected; the
    // occluder had no depth target to match; the wireframe had no edges. One
    // number each, so the next run does not need another round of inference.
    const inj::InjectionStats injectionStats = inj::GetInjectionStats ();
    health.presentInjections = injectionStats.injectedPresent;
    health.presentsSeen = injectionStats.injectedPresent + injectionStats.skippedNoCamera;
    health.hostNoDepthTarget = hostStats.skippedNoDepthTarget;
    health.hostNoGeometry = hostStats.skippedNoGeometry;
    health.overlayNoEdges = overlayStats.skippedNoEdges;
    health.overlayNoCamera = overlayStats.skippedNoCamera;
    health.overlayCulled = overlayStats.culledPasses;
    health.linesDrawn = overlayStats.linesDrawn;
    health.silhouetteEdges = overlayStats.silhouetteCandidates;
    health.skippedStaleCamera = injectionStats.skippedStaleCamera;
    health.acceptedViewportWidth = injectionStats.acceptedViewportWidth;
    health.acceptedViewportHeight = injectionStats.acceptedViewportHeight;
    health.sceneViewportWidth = injectionStats.liveSceneViewportWidth;
    health.sceneViewportHeight = injectionStats.liveSceneViewportHeight;
    health.resizeRebinds = cen::GetBindingStats ().resizeRebinds;
    health.lastMissMask = cen::GetBindingStats ().lastMissMask;
    health.sceneNew = injectionStats.newScene;
    health.sceneRepeat = injectionStats.repeatScene;
    health.sceneLate = injectionStats.invalidGenerationAdvanced;
    const inj::freshness::Report fresh = inj::freshness::Snapshot ();
    health.age0 = fresh.age0;
    health.age1 = fresh.age1;
    health.age2 = fresh.age2;
    health.age3plus = fresh.age3plus;
    health.cameraAgeMax = fresh.ageMax;
    health.cameraAgeSamples = fresh.samples;
    health.suppressedStaleViewport = fresh.suppressed;
    health.repeatHeld = fresh.repeatHeld;
    health.repeatPassMoved = fresh.repeatPassMoved;
    health.repeatWindowMoved = fresh.repeatWindowMoved;
    health.redrawRequests = redrawbudget::Requests ();
    health.redrawWaitMaxMs = fresh.redrawWaitMaxMs;
    health.redrawsTaken = fresh.redrawsTaken;
    health.ticks = g_ticks;
    health.timerTicks = g_timerTicks;
    health.tickGapMaxMs = g_tickGapMaxMs;
    health.tickGapsOverASecond = g_tickGapsOverASecond;
    const dxgi::injecteddiligent::Stats dil = dxgi::injecteddiligent::Snapshot ();
    health.overlayBackend =
        dxgi::injecteddiligent::GetBackend () == dxgi::injecteddiligent::Backend::Diligent ? "diligent" : "native";
    health.diligentAttached = dil.attached;
    health.diligentAttachMs = dil.attachMs;
    health.diligentAttachAttempts = dil.attachAttempts;
    health.diligentAttachFailures = dil.attachFailures;
    health.diligentWraps = dil.wraps;
    health.diligentWrapHits = dil.wrapHits;
    health.diligentWrapFailures = dil.wrapFailures;
    health.diligentDistinctBackBuffers = dil.distinctBackBuffers;
    health.diligentWrapDropsOnResize = dil.wrapDropsOnResize;
    health.diligentError = dil.lastError;
    health.modelRevision = hostStats.modelRevision;
    health.publishedRevision = hostStats.publishedRevision;
    health.gpuRevision = hostStats.gpuRevision;
    const modelwatch::Stats watch = modelwatch::Get ();
    health.watchRunning = watch.running;
    health.watchPolls = watch.polls;
    health.watchEdits = watch.geometryEdits;
    health.watchRefreshes = watch.refreshes;
    health.watchEnvironmentOnly = watch.environmentOnly;
    health.watchSkippedBusy = watch.skippedBusy;
    health.watchIntervalMs = watch.intervalMs;
    health.watchError = watch.lastError;
    health.modelEditRebinds = census.modelEditRebinds;
    health.modelEditReselects = census.modelEditReselects;
    const cen::BindingStats binding = cen::GetBindingStats ();
    health.rebinds = binding.rebinds;
    health.rebindsRefused = binding.rebindsRefused;
    health.selectionMatches = binding.selectionMatches;
    health.pinMissMask = binding.pinMissMask;
    const composer::Stats composeStats = composer::GetStats ();
    health.targetWidth = composeStats.targetWidth;
    health.targetHeight = composeStats.targetHeight;
    health.composeDepthWidth = composeStats.depthWidth;
    health.composeDepthHeight = composeStats.depthHeight;
    health.composeSizeMismatches = composeStats.sizeMismatches;

    const cen::Selection selection = cen::GetSelection ();
    health.modelFramesSeen = census.modelFramesSeen;
    health.drawsSeen = census.drawsSeen;
    health.drawsQualified = census.drawsQualified;
    health.groupsUsed = census.groupsUsed;
    health.groupsOverflowed = census.groupsOverflowed;
    health.eligibleCandidates = census.eligibleCandidates;
    health.selectionAttempts = census.autoSelectAttempts;
    health.selectionValid = selection.valid;
    health.selectedGroup = selection.groupId;
    health.selectedOccurrence = selection.occurrenceIndex;
    health.occurrenceLocked = injectionStats.occurrenceLocked;
    const inj::CameraSource source = inj::GetCameraSource ();
    health.cameraSource = source == inj::CameraSource::CensusSelectedGroup ? "CensusSelectedGroup"
                          : source == inj::CameraSource::Learner           ? "Learner"
                                                                           : "None";
    health.armState = inj::GetArmState () == inj::ArmState::Active               ? "Active"
                      : inj::GetArmState () == inj::ArmState::ArmedPendingCamera ? "ArmedPendingCamera"
                                                                                 : "Disabled";
    health.logicalMatches = census.logicalMatches;
    health.authoritativeSnapshots = injectionStats.authoritativeSnapshots;
    health.blockedAt = BlockedAt (health);
    return health;
}

void Stop ()
{
    if (!g_running)
        return;
    // ⚠️ ONLY IF WE ARMED IT, AND NOT WHILE THE PORTABLE
    // VIEWPORT IS UP. `modelwatch::Start` is idempotent and shared; stopping a
    // watch somebody else depends on would leave THEIR scene frozen instead.
    if (g_modelWatchStarted) {
        modelwatch::SetKeepAlive (false);
        // Still not stopped while the portable viewport is up: that is its watch
        // too, and stopping it would leave THEIR scene frozen instead.
        if (!DiligentViewport::Get ().IsRunning ())
            modelwatch::Stop ();
    }
    g_modelWatchStarted = false;
    redrawbudget::Reset ();
    // ⚠️ AND STOP POINTING AT A SELECTION THAT IS ABOUT TO NOT
    // EXIST. Guidance section 4: selection and camera source move together, and
    // `selection=none source=CensusSelectedGroup` is a state that must never be
    // observable. It was, on every restart.
    // ⚠️ AND LET GO OF ARCHICAD'S BACK BUFFER BEFORE THE
    // HOOKS COME OUT. `ResizeBuffers` fails while a swap-chain view is alive
    // (section 11), so a wrapper outliving the session would break the NEXT one
    // in a way that looks nothing like its cause.
    // ⚠️ RESET AT STOP, COUNTERS ONLY AT START. The
    // choice ends with the session that made it, or the next menu click inherits
    // it; but the caller sets it just BEFORE starting, so clearing it there
    // would wipe it microseconds after it was made.
    dxgi::injecteddiligent::Reset ();
    inj::SetCameraSource (inj::CameraSource::None);
    inj::SetEnabled (false);
    cen::SetAutoSelect (false);
    cen::SetEnabled (false);

    // ⚠️ BACK TO THE MODE THE HOOKS CAME FROM, WHICH ALSO RELEASES THEM. The
    // context detours repair themselves every frame, so an uninstall that loses
    // a race can leave them forwarding through nulled originals -- Archicad keeps
    // running and silently stops drawing. `SetCameraSyncMode` owns that sequence.
    std::string error;
    SetCameraSyncMode (CameraSyncMode::Legacy, CurrentCameraSyncIntervalMs (), CurrentPredictionScale (),
                       CurrentHideOnNav (), false, error);

    StopHeartbeat ();
    g_running = false;
    g_visible = false;
    g_armPending = false;
    g_lastCamera = CameraState::Unavailable;
    g_lastHost = HostState::Idle;
    g_lastDraws = 0;
    // Same fault class as `g_hostRequested` and `g_lastAutoSelectAttempt`: a mark
    // that outlives the session it describes makes the first tick of the next one
    // report a delta it did not measure.
    report::Reset ();
    // ⚠️ THE REQUEST FLAG IS A PROPERTY OF ONE SESSION AND MUST NOT
    // OUTLIVE IT. `g_hostRequested` guards against asking for the same extraction
    // twice inside a session; left set across `Stop`, it meant the SECOND
    // `Start` never requested one at all. Run sixty-four reported `host Idle` for
    // its whole window with a perfectly working extractor, because an earlier
    // session in the same Archicad had already used up the one request.
    g_hostRequested = false;
    report::Say ("OVERLAY", "stopped, hooks released");
    // ⚠️ THE HOST SNAPSHOT IS DELIBERATELY KEPT. It is the model, not overlay
    // state, it costs nothing while nothing draws, and the next `Start` then has
    // a building immediately instead of re-walking one.
}

} // namespace overlayruntime
} // namespace archviz
} // namespace geomsrv
