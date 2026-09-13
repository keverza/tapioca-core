#ifndef EVP_ARCHVIZ_DXGI_DEVICEIDENTITY_HPP
#define EVP_ARCHVIZ_DXGI_DEVICEIDENTITY_HPP

// What KIND of D3D11 device is this (PLAT-RE153,
// docs/architecture/api/HANDOFF-OverlayPatch.md stage 1).
//
// WHY IT IS ITS OWN FILE. `ContextHook` answers "can I record what this context
// is told"; this answers the prior question nobody thought to ask -- "is this
// context where anything happens at all". The 2026-09-13 third run made that
// distinction expensive: the hook installed on Archicad's own immediate context,
// provably and exclusively, and then recorded about two viewport sets a second
// while the user orbited at 100 fps. Everything worked and nothing was there.
//
// ⚠️ `is11On12` IS THE FACT THE WHOLE RUNG TURNS ON. A D3D11On12 device means
// Archicad's real renderer is D3D12: the D3D11 device exists to own a swap chain
// and interoperate, the immediate context is a presentation shim, and the view
// matrix is going out through `ID3D12GraphicsCommandList::SetGraphicsRoot*`
// where no `ID3D11DeviceContext` hook can ever see it. That is not a bug to fix
// in the hook -- it is a different hook, against a different API, and knowing it
// costs one `QueryInterface` rather than a week with a disassembler.
//
// ⚠️ THE IIDs ARE SPELLED OUT RATHER THAN INCLUDED. Pulling in <d3d11on12.h> and
// the newer <d3d11_4.h> would put those headers on everything that includes this,
// to ask a handful of yes/no questions. The identifiers are stable published
// constants -- they are part of the ABI in exactly the way the vtable order is --
// and writing them here keeps the dependency at one translation unit.
//
// THREAD SAFETY: MAIN THREAD. `QueryInterface` is safe on any thread, but the
// only caller is a bus command and there is no reason to widen that.

#include <cstdint>

struct ID3D11Device;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace deviceidentity {

struct DeviceInfo {
    bool     found = false;
    uint64_t device = 0;
    uint32_t creationFlags = 0;
    uint32_t featureLevel = 0;
    bool     is11On12 = false;
    bool     debugLayer = false;
    bool     singleThreaded = false;
    bool     bgraSupport = false;
    uint32_t highestDeviceInterface = 0;   // 5 for ID3D11Device5, 0 if only the base
};

// `device` may be null, which reports `found == false` rather than failing: the
// caller is usually asking before the present detour has discovered one.
DeviceInfo Describe (ID3D11Device* device);

// ---- which graphics stack is Archicad's 3D window actually using? ----------
//
// ⚠️ THIS IS THE QUESTION THE FIFTH RUN LEFT. By then the D3D11 immediate
// context behind Archicad's nominated swap chain had been shown to draw four
// times in a whole orbit, play back no command lists and blit once, on a device
// that is not D3D11On12 -- and there were only two swap chains in the process.
// Nothing DXGI-shaped is rendering that model.
//
// The device's creation flags are the clue that reframes it: `BGRA_SUPPORT` and
// nothing else is the DIRECT2D INTEROP flag. A device created with only that,
// presenting continuously while drawing nothing, is a Direct2D or
// DirectComposition UI surface -- Archicad's window chrome and palettes. And the
// chain nomination accepts any window sharing a ROOT with the overlay's target,
// so the main window's compositor chain passes it.
//
// If the 3D view is not presented through DXGI at all, it is presented through
// something else, and for Archicad the obvious candidate is OPENGL -- which uses
// `SwapBuffers` on an HDC and would never appear as a swap chain. That is
// checkable without hooking anything: an OpenGL window has a pixel format whose
// descriptor says `PFD_SUPPORT_OPENGL`, and a process using OpenGL has the
// vendor's ICD loaded.
struct RenderStack {
    // Loaded graphics runtimes, by presence. Asked with `GetModuleHandleW`, which
    // takes no reference and cannot load anything -- a plain "is this already in
    // the process" question.
    bool openglLoaded = false;      // opengl32.dll
    bool openglIcdLoaded = false;   // a vendor ICD: nvoglv64 / atio6axx / ig*icd64
    bool d3d12Loaded = false;
    bool vulkanLoaded = false;
    bool d2dLoaded = false;
    bool dcompLoaded = false;

    // The window the overlay is covering -- Archicad's 3D canvas.
    uint64_t targetWindow = 0;
    bool     targetHasPixelFormat = false;
    int32_t  targetPixelFormat = 0;
    bool     targetSupportsOpenGL = false;   // PFD_SUPPORT_OPENGL
    bool     targetSupportsGdi = false;      // PFD_SUPPORT_GDI
    bool     targetDoubleBuffered = false;
};

// MAIN THREAD (it touches a DC). `targetWindow` is the overlay's own target, or
// 0 to skip the per-window half.
RenderStack DescribeRenderStack (uint64_t targetWindow);

}   // namespace deviceidentity
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv

#endif
