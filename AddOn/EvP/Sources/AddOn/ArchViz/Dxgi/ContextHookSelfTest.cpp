// ArchViz/Dxgi/ContextHookSelfTest -- see the header. Every rule about why the
// throwaway must match Archicad's device, and why the verdict lives in the
// caller, is there; this is the mechanism.

#include "ArchViz/Dxgi/ContextHookSelfTest.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>

#include <algorithm>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace selftest {

void Throwaway::Release ()
{
    if (colourShaderView != nullptr)
        colourShaderView->Release ();
    if (depthView != nullptr)
        depthView->Release ();
    if (colourView != nullptr)
        colourView->Release ();
    if (depth != nullptr)
        depth->Release ();
    if (shaderTexture != nullptr)
        shaderTexture->Release ();
    if (colour != nullptr)
        colour->Release ();
    if (defaultBuffer != nullptr)
        defaultBuffer->Release ();
    if (dynamicBuffer != nullptr)
        dynamicBuffer->Release ();
    if (context != nullptr)
        context->Release ();
    if (device != nullptr)
        device->Release ();
    *this = Throwaway {};
}

std::string OwningModuleOf (const void* function)
{
    if (function == nullptr)
        return std::string ();
    HMODULE owner = nullptr;
    if (GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR> (function), &owner) == 0 ||
        owner == nullptr)
        return std::string ();

    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW (owner, path, MAX_PATH) == 0)
        return std::string ();
    std::wstring wide (path);
    std::transform (wide.begin (), wide.end (), wide.begin (), ::towlower);
    const size_t slash = wide.find_last_of (L"\\/");
    const std::wstring leaf = (slash == std::wstring::npos) ? wide : wide.substr (slash + 1);

    const int needed =
        WideCharToMultiByte (CP_UTF8, 0, leaf.c_str (), int (leaf.size ()), nullptr, 0, nullptr, nullptr);
    std::string narrow (size_t (needed), '\0');
    WideCharToMultiByte (CP_UTF8, 0, leaf.c_str (), int (leaf.size ()), narrow.data (), needed, nullptr, nullptr);
    return narrow;
}

bool ValidateTable (void** vtable, const SlotDescriptor* slots, size_t count, std::string& error)
{
    if (vtable == nullptr || slots == nullptr || count == 0) {
        error = "no vtable to validate";
        return false;
    }

    // ⚠️ THE WHOLE RANGE MUST BE COMMITTED BEFORE A SINGLE SLOT IS READ, and this
    // became load-bearing when the set grew into ID3D11DeviceContext1 territory
    // at index 123. Archicad's table is INLINE IN THE CONTEXT OBJECT -- its
    // address is the object's own address plus eight -- not in d3d11.dll's
    // read-only data. So an index past the end of that table is a read, and then
    // a WRITE, into whatever follows the object on the heap. VirtualQuery turns
    // "probably still mapped" into a checked fact.
    size_t highest = 0;
    for (size_t i = 0; i < count; ++i)
        highest = (slots[i].index > highest) ? slots[i].index : highest;
    const size_t bytes = (highest + 1) * sizeof (void*);
    MEMORY_BASIC_INFORMATION region = {};
    if (VirtualQuery (vtable, &region, sizeof (region)) == 0 || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        error = "the vtable is not committed readable memory; refusing to touch it";
        return false;
    }
    const unsigned char* regionEnd = static_cast<const unsigned char*> (region.BaseAddress) + region.RegionSize;
    if (reinterpret_cast<const unsigned char*> (vtable) + bytes > regionEnd) {
        error = "the vtable is shorter than the highest slot the hook wants (" + std::to_string (highest) +
                "); refusing to read or write past it";
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        void* const entry = vtable[slots[i].index];
        const std::string owner = OwningModuleOf (entry);
        if (owner != "d3d11.dll") {
            error = std::string ("vtable slot ") + std::to_string (slots[i].index) + " (" + slots[i].name +
                    ") does not point into d3d11.dll but into '" +
                    (owner.empty () ? std::string ("no loaded module") : owner) + "'; refusing to patch it";
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (vtable[slots[j].index] == entry) {
                error = std::string ("vtable slots ") + slots[j].name + " and " + slots[i].name +
                        " hold the same function; the indices cannot all "
                        "be right, so nothing is patched";
                return false;
            }
        }
    }
    return true;
}

bool Create (unsigned int creationFlags, int featureLevel, Throwaway& out, std::string& error)
{
    // ⚠️ THE LEVEL IS REQUESTED AS AN EXACT ARRAY OF ONE, not as a minimum. D3D11
    // walks the array and takes the first level it can create, so passing the
    // usual descending list would happily hand back a HIGHER level than
    // Archicad's -- and a different level is one of the things that changes which
    // context implementation the runtime hands out, which is the whole failure
    // this argument exists to avoid.
    const D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL (featureLevel);
    D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL (0);

    HRESULT hr = D3D11CreateDevice (nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, creationFlags, &requested, 1,
                                    D3D11_SDK_VERSION, &out.device, &obtained, &out.context);
    if (FAILED (hr) || out.device == nullptr || out.context == nullptr) {
        out.Release ();
        error = "D3D11CreateDevice with Archicad's own creation flags (0x" + std::to_string ((unsigned) creationFlags) +
                ") and feature level 0x" + std::to_string ((unsigned) featureLevel) + " failed (0x" +
                std::to_string ((unsigned) hr) + ")";
        return false;
    }
    out.vtable = *reinterpret_cast<void***> (out.context);

    // Everything below exists only so `Exercise` has something legal to pass to
    // each patched method. All of it is 1x1 or 256 bytes, on our own device.
    D3D11_BUFFER_DESC dynamicDesc = {};
    dynamicDesc.ByteWidth = 256;
    dynamicDesc.Usage = D3D11_USAGE_DYNAMIC;
    dynamicDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dynamicDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    HRESULT step = out.device->CreateBuffer (&dynamicDesc, nullptr, &out.dynamicBuffer);

    D3D11_BUFFER_DESC defaultDesc = {};
    defaultDesc.ByteWidth = 256;
    defaultDesc.Usage = D3D11_USAGE_DEFAULT;
    defaultDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED (step))
        step = out.device->CreateBuffer (&defaultDesc, nullptr, &out.defaultBuffer);

    D3D11_TEXTURE2D_DESC colourDesc = {};
    colourDesc.Width = 1;
    colourDesc.Height = 1;
    colourDesc.MipLevels = 1;
    colourDesc.ArraySize = 1;
    colourDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    colourDesc.SampleDesc.Count = 1;
    colourDesc.Usage = D3D11_USAGE_DEFAULT;
    colourDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (SUCCEEDED (step))
        step = out.device->CreateTexture2D (&colourDesc, nullptr, &out.colour);
    if (SUCCEEDED (step))
        step = out.device->CreateRenderTargetView (out.colour, nullptr, &out.colourView);

    D3D11_TEXTURE2D_DESC shaderDesc = colourDesc;
    shaderDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED (step))
        step = out.device->CreateTexture2D (&shaderDesc, nullptr, &out.shaderTexture);
    if (SUCCEEDED (step))
        step = out.device->CreateShaderResourceView (out.shaderTexture, nullptr, &out.colourShaderView);

    D3D11_TEXTURE2D_DESC depthDesc = colourDesc;
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (SUCCEEDED (step))
        step = out.device->CreateTexture2D (&depthDesc, nullptr, &out.depth);
    if (SUCCEEDED (step))
        step = out.device->CreateDepthStencilView (out.depth, nullptr, &out.depthView);

    if (FAILED (step)) {
        out.Release ();
        error = "the throwaway resources the vtable self-test needs could not be created (0x" +
                std::to_string ((unsigned) step) + ")";
        return false;
    }
    return true;
}

void Exercise (const Throwaway& throwaway)
{
    ID3D11DeviceContext* context = throwaway.context;
    if (context == nullptr)
        return;

    D3D11_VIEWPORT viewport = {};
    viewport.Width = 1.0f;
    viewport.Height = 1.0f;
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports (1, &viewport);

    D3D11_RECT scissor = { 0, 0, 1, 1 };
    context->RSSetScissorRects (1, &scissor);

    context->OMSetRenderTargets (1, &throwaway.colourView, throwaway.depthView);
    context->PSSetShaderResources (0, 1, &throwaway.colourShaderView);

    ID3D11Buffer* constants[1] = { throwaway.defaultBuffer };
    context->VSSetConstantBuffers (0, 1, constants);
    context->PSSetConstantBuffers (0, 1, constants);
    context->GSSetConstantBuffers (0, 1, constants);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED (context->Map (throwaway.dynamicBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        if (mapped.pData != nullptr)
            std::memset (mapped.pData, 0, 256);
        context->Unmap (throwaway.dynamicBuffer, 0);
    }

    unsigned char bytes[256] = {};
    context->UpdateSubresource (throwaway.defaultBuffer, 0, nullptr, bytes, 0, 0);

    const FLOAT clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->ClearRenderTargetView (throwaway.colourView, clear);
    context->ClearDepthStencilView (throwaway.depthView, D3D11_CLEAR_DEPTH, 1.0f, 0);

    // ⚠️ UNBOUND BEFORE THE CALLER RELEASES THE TARGETS. Leaving a view bound to
    // a context whose resource is about to go away is the kind of thing that
    // survives a debug run and reports as a driver fault on somebody else's
    // machine.
    context->OMSetRenderTargets (0, nullptr, nullptr);
    ID3D11ShaderResourceView* noShaderResource = nullptr;
    context->PSSetShaderResources (0, 1, &noShaderResource);
}

} // namespace selftest
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
