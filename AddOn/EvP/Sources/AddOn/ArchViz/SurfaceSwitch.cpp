// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. It starts and stops the overlays through the controller's own
// entry points, never around them (§8, §9).
// ArchViz/SurfaceSwitch -- see the header.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/SurfaceSwitch.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/HudConsole.hpp" // the Debug tab's console: what the user checks when something fails
#include "ArchViz/ArchVizPanel.hpp"
#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/OverlayController.hpp"

#include <windows.h>

#include <atomic>
#include <string>

namespace geomsrv {
namespace archviz {
namespace surfaceswitch {

namespace {

constexpr UINT kSwitchMessage = WM_APP + 0x51;
constexpr wchar_t kClassName[] = L"TapiocaSurfaceSwitch";

std::atomic<HWND> g_window { nullptr };
bool g_classRegistered = false;
// A switch performed starts and stops renderers; one asked meanwhile waits for the next message.
bool g_performing = false;
std::atomic<uint64_t> g_requested { 0 }, g_performed { 0 }, g_dropped { 0 };

HINSTANCE Module ()
{
    HMODULE module = nullptr;
    ::GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR> (&Module), &module);
    return module;
}

void Perform (Surface to)
{
    if (g_performing)
        return;
    g_performing = true;
    g_performed.fetch_add (1, std::memory_order_relaxed);
    namespace control = overlaycontrol;
    if (to == Surface::Viewer) {
        // From the floor plan, the viewer opens in its place: top-down, cut at the storey.
        const bool plan = control::CurrentView () == control::ViewKind::FloorPlan;
        ArchVizLog (std::string ("SURFACE      the HUD switches to the viewer") + (plan ? ", in plan mode" : "") +
                    ": the overlays go off");
        hudconsole::Note ("Switch", plan ? "to the viewer, in plan mode" : "to the viewer");
        // The overlays are turned off inside (BeforeViewerOpens).
        ArchVizPanel::OpenViewer (plan);
    }
    else {
        const control::ViewKind front = control::CurrentView ();
        const control::Overlay which =
            front == control::ViewKind::FloorPlan ? control::Overlay::TwoD : control::Overlay::ThreeD;
        ArchVizLog (std::string ("SURFACE      the viewer's HUD switches to the ") + control::OverlayName (which) +
                    ": the viewer closes");
        // The viewer is closed inside (BeforeOverlayStarts); "hud" opens the overlay's HUD.
        control::SetWanted (which, true, "hud");
    }
    g_performing = false;
}

LRESULT CALLBACK WindowProc (HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == kSwitchMessage) {
        Perform (Surface (wParam));
        return 0;
    }
    return ::DefWindowProcW (window, message, wParam, lParam);
}

} // namespace

void Arm ()
{
    if (g_window.load (std::memory_order_acquire) != nullptr)
        return;
    const HINSTANCE instance = Module ();
    if (!g_classRegistered) {
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof (windowClass);
        windowClass.lpfnWndProc = &WindowProc;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = kClassName;
        g_classRegistered = ::RegisterClassExW (&windowClass) != 0 || ::GetLastError () == ERROR_CLASS_ALREADY_EXISTS;
    }
    const HWND window = g_classRegistered ? ::CreateWindowExW (0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                                               instance, nullptr)
                                          : nullptr;
    if (window == nullptr) {
        ArchVizLog ("SURFACE      the switch's window could not be made: the HUD's circles will not switch");
        hudconsole::Error ("Switch", "the dock's circles will not switch: the switch's window could not be made");
    }
    g_window.store (window, std::memory_order_release);
}

void Shutdown ()
{
    const HWND window = g_window.exchange (nullptr, std::memory_order_acq_rel);
    if (window != nullptr)
        ::DestroyWindow (window);
    if (g_classRegistered) {
        ::UnregisterClassW (kClassName, Module ());
        g_classRegistered = false;
    }
}

void Request (Surface to)
{
    g_requested.fetch_add (1, std::memory_order_relaxed);
    const HWND window = g_window.load (std::memory_order_acquire);
    if (window == nullptr || !::PostMessageW (window, kSwitchMessage, WPARAM (to), 0))
        g_dropped.fetch_add (1, std::memory_order_relaxed);
}

bool ViewerOpen ()
{
    const DiligentViewport& viewport = DiligentViewport::Get ();
    return viewport.IsRunning () && viewport.Mode () == SurfaceMode::PaletteChild;
}

void BeforeOverlayStarts ()
{
    if (!ViewerOpen ())
        return;
    ArchVizLog ("SURFACE      an overlay starts: the viewer closes");
    ArchVizPanel::CloseViewer ();
}

void BeforeViewerOpens ()
{
    namespace control = overlaycontrol;
    for (const control::Overlay which : { control::Overlay::ThreeD, control::Overlay::TwoD })
        if (control::Describe (which).wanted)
            control::SetWanted (which, false, "viewer");
}

Stats GetStats ()
{
    Stats stats;
    stats.requested = g_requested.load (std::memory_order_relaxed);
    stats.performed = g_performed.load (std::memory_order_relaxed);
    stats.dropped = g_dropped.load (std::memory_order_relaxed);
    return stats;
}

} // namespace surfaceswitch
} // namespace archviz
} // namespace geomsrv
