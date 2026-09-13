// ArchViz/Dxgi/DeviceIdentity -- see the header. Every rule about why the IIDs
// are written out and why `is11On12` matters is there; this is the mechanism.

#include "ArchViz/Dxgi/DeviceIdentity.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace deviceidentity {

namespace {

// D3D11On12's published interface identifier.
const GUID kIID_ID3D11On12Device =
    {0x85611e73, 0x70a9, 0x490e, {0x96, 0x14, 0xa9, 0xe3, 0x02, 0x77, 0x79, 0x04}};

// ID3D11Device1 .. ID3D11Device5, in order, so the highest one that answers says
// which era of the API this device was built by.
const GUID kDeviceIids[] = {
    {0xa04bfb29, 0x08ef, 0x43d6, {0xa4, 0x9c, 0xa9, 0xbd, 0xbd, 0xcb, 0xe6, 0x86}},
    {0x9d06dffa, 0xd1e5, 0x4d07, {0x83, 0xa8, 0x1b, 0xb1, 0x23, 0xf2, 0xf8, 0x41}},
    {0xa05c8c37, 0xd2c6, 0x4732, {0xb3, 0xa0, 0x9c, 0xe0, 0xb0, 0xdc, 0x9a, 0xe6}},
    {0x8992ab71, 0x02e6, 0x4b8d, {0xba, 0x48, 0xb0, 0x56, 0xdc, 0xda, 0x42, 0xc4}},
    {0x8ffde202, 0xa0e7, 0x45df, {0x9e, 0x01, 0xe8, 0x37, 0x80, 0x1b, 0x5e, 0xa0}},
};

// ⚠️ EVERY SUCCESSFUL QueryInterface IS RELEASED. This runs on a live Archicad
// device; a leaked reference per call to a diagnostic is a device that never
// tears down, and the symptom would surface long after as Archicad failing to
// close cleanly.
bool Answers (ID3D11Device* device, const GUID& iid)
{
    IUnknown* probe = nullptr;
    if (FAILED (device->QueryInterface (iid, (void**) &probe)) || probe == nullptr)
        return false;
    probe->Release ();
    return true;
}

}   // namespace

DeviceInfo Describe (ID3D11Device* device)
{
    DeviceInfo info;
    if (device == nullptr)
        return info;

    info.found = true;
    info.device = uint64_t (uintptr_t (device));
    info.creationFlags = uint32_t (device->GetCreationFlags ());
    info.featureLevel = uint32_t (device->GetFeatureLevel ());
    info.debugLayer = (info.creationFlags & D3D11_CREATE_DEVICE_DEBUG) != 0;
    info.singleThreaded = (info.creationFlags & D3D11_CREATE_DEVICE_SINGLETHREADED) != 0;
    info.bgraSupport = (info.creationFlags & D3D11_CREATE_DEVICE_BGRA_SUPPORT) != 0;

    info.is11On12 = Answers (device, kIID_ID3D11On12Device);

    for (uint32_t i = 0; i < 5; ++i) {
        if (Answers (device, kDeviceIids[i]))
            info.highestDeviceInterface = i + 1;
    }
    return info;
}

RenderStack DescribeRenderStack (uint64_t targetWindow)
{
    RenderStack stack;

    // ⚠️ `GetModuleHandleW`, NOT `LoadLibrary`. This must answer "is Archicad
    // already using this" and nothing else; loading a graphics runtime to ask
    // whether it is loaded would make the answer true by asking it.
    stack.openglLoaded  = GetModuleHandleW (L"opengl32.dll") != nullptr;
    stack.d3d12Loaded   = GetModuleHandleW (L"d3d12.dll") != nullptr;
    stack.vulkanLoaded  = GetModuleHandleW (L"vulkan-1.dll") != nullptr;
    stack.d2dLoaded     = GetModuleHandleW (L"d2d1.dll") != nullptr;
    stack.dcompLoaded   = GetModuleHandleW (L"dcomp.dll") != nullptr;

    // The vendor ICDs, which is what actually distinguishes "opengl32.dll is
    // mapped because something linked it" from "OpenGL is being driven": the
    // Microsoft stub loads the vendor driver only when a real context is made.
    static const wchar_t* const kIcds[] = {
        L"nvoglv64.dll",    // NVIDIA
        L"atio6axx.dll",    // AMD
        L"ig9icd64.dll",    // Intel, recent
        L"ig75icd64.dll",   // Intel, older
    };
    for (const wchar_t* icd : kIcds) {
        if (GetModuleHandleW (icd) != nullptr)
            stack.openglIcdLoaded = true;
    }

    stack.targetWindow = targetWindow;
    if (targetWindow == 0)
        return stack;

    // ⚠️ THE DC IS RELEASED ON EVERY PATH. A leaked window DC inside Archicad is
    // a GDI handle that never comes back, and the symptom -- Archicad slowly
    // failing to draw anything at all -- would arrive hours later with nothing
    // pointing at a diagnostic that ran once.
    HWND window = (HWND) (uintptr_t) targetWindow;
    HDC dc = GetDC (window);
    if (dc == nullptr)
        return stack;

    const int format = GetPixelFormat (dc);
    if (format > 0) {
        stack.targetHasPixelFormat = true;
        stack.targetPixelFormat = format;
        PIXELFORMATDESCRIPTOR descriptor = {};
        if (DescribePixelFormat (dc, format, sizeof (descriptor), &descriptor) != 0) {
            stack.targetSupportsOpenGL = (descriptor.dwFlags & PFD_SUPPORT_OPENGL) != 0;
            stack.targetSupportsGdi = (descriptor.dwFlags & PFD_SUPPORT_GDI) != 0;
            stack.targetDoubleBuffered = (descriptor.dwFlags & PFD_DOUBLEBUFFER) != 0;
        }
    }
    ReleaseDC (window, dc);
    return stack;
}

}   // namespace deviceidentity
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv
