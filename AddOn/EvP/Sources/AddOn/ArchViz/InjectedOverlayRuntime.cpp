// ArchViz/InjectedOverlayRuntime -- see the header. Every rule about this file is
// in that header's comments; this is the mechanism.

#include "ArchViz/InjectedOverlayRuntime.hpp"

#include "ArchViz/CameraSyncMode.hpp"
#include "ArchViz/Dxgi/CameraCensus.hpp"
#include "ArchViz/Dxgi/ContextHook.hpp"
#include "ArchViz/Dxgi/HostOccluders.hpp"
#include "ArchViz/Dxgi/HostOverlay.hpp"
#include "ArchViz/Dxgi/InjectionRenderer.hpp"
#include "ArchViz/ExtractionThread.hpp"

#include <windows.h>

namespace geomsrv {
namespace archviz {
namespace overlayruntime {

namespace {

namespace cen = dxgi::census;
namespace inj = dxgi::injection;
namespace host = dxgi::hostocclusion;
namespace ho = dxgi::hostoverlay;

bool g_running = false;
bool g_visible = false;
// ⚠️ THE ARMING IS NOT FINISHED WHEN `Start` RETURNS, and pretending otherwise is
// what would make this a command rather than a service. The context hook cannot
// install until the Present detour has identified Archicad's swap chain, which
// takes about sixty of Archicad's frames -- and if the 3D window has never been
// drawn there is nothing to identify at all. `Tick` finishes the job.
bool g_armPending = false;
bool g_hostRequested = false;
uint64_t g_reacquisitions = 0;
CameraState g_lastCamera = CameraState::Unavailable;
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

void CALLBACK TickProc (HWND, UINT, UINT_PTR, DWORD)
{
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

void Remember (StartError code, const std::string& message)
{
    g_lastError = code;
    g_lastMessage = message;
}

CameraState ReadCameraState ()
{
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

// ⚠️ THE OVERLAY IS ARMED IN ONE PLACE AND `Tick` MAY RUN IT AGAIN. Arming is
// idempotent by construction: every call below either sets a flag that is already
// set or requests a mode that is already current.
StartResult Arm ()
{
    const auto context = dxgi::GetContextHookStats ();

    std::string error;
    if (!SetCameraSyncMode (CameraSyncMode::HookDiag, CurrentCameraSyncIntervalMs (), CurrentPredictionScale (),
                            CurrentHideOnNav (), true, error)) {
        // ⚠️ AN UNPINNED BUILD FAILS CLOSED AND SAYS SO IN ONE WORD. It is not a
        // fault and it is not retryable: the patch profile fingerprints every
        // hooked slot and refuses an Archicad it was not measured against, which
        // is the trade this whole rung was accepted on. The portable overlay is
        // the answer, and only the caller can choose it.
        if (!context.pinned) {
            StartResult result;
            result.code = StartError::BuildNotPinned;
            result.message = "this Archicad build is not pinned, so the GPU-state hooks refuse to install; "
                             "the portable overlay is the fallback";
            result.retryable = false;
            return result;
        }
        StartResult result;
        result.code = StartError::HooksRefused;
        result.message = error.empty () ? std::string ("the GPU-state hooks would not arm") : error;
        result.retryable = true;
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
    cen::SetEnabled (true);
    cen::SetAutoSelect (true);

    // ⚠️ ARMED PENDING CAMERA, WHICH DRAWS NOTHING UNTIL A MATCHING DRAW IS
    // SNAPSHOTTED. The render thread promotes it to `Active` on evidence, so
    // enabling here cannot put a primitive on screen with a camera nobody has.
    inj::SetEnabled (true);
    inj::SetPoint (inj::Point::Present);

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

    const StartResult armed = Arm ();
    g_running = true;
    g_visible = true;
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
    const CameraState camera = ReadCameraState ();
    if (camera == CameraState::Reacquiring && g_lastCamera == CameraState::Locked)
        ++g_reacquisitions;
    g_lastCamera = camera;
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
    return health;
}

void Stop ()
{
    if (!g_running)
        return;
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
    // ⚠️ THE HOST SNAPSHOT IS DELIBERATELY KEPT. It is the model, not overlay
    // state, it costs nothing while nothing draws, and the next `Start` then has
    // a building immediately instead of re-walking one.
}

} // namespace overlayruntime
} // namespace archviz
} // namespace geomsrv
