#include "ArchViz/DiligentProbe.hpp"

#include "ArchViz/ArchVizLog.hpp"

#include <windows.h>
#include <d3d11.h> // Must precede Diligent's D3D11 interop header (Probe 1a).
#include <EngineFactoryD3D11.h>
#include <RefCntAutoPtr.hpp>

namespace geomsrv::archviz {

DiligentProbe& DiligentProbe::Get ()
{
    static DiligentProbe probe;
    return probe;
}

DiligentProbe::~DiligentProbe () { Stop (); }

bool DiligentProbe::Start (void* hwnd)
{
    if (hwnd == nullptr || ::IsWindow (static_cast<HWND> (hwnd)) == FALSE) {
        std::lock_guard<std::mutex> lock (mutex_);
        stats_.error = "DG child HWND was invalid; Diligent was not called.";
        ArchVizLog ("Diligent 1c: refused before init: invalid DG child HWND");
        return false;
    }
    if (attempted_.exchange (true)) {
        std::lock_guard<std::mutex> lock (mutex_);
        stats_.running = false;
        stats_.succeeded = false;
        stats_.error = "Probe already attempted in this Archicad session; restart before retrying.";
        ArchVizLog ("Diligent 1c: refused retry in this Archicad session");
        return false;
    }

    Stop ();
    {
        std::lock_guard<std::mutex> lock (mutex_);
        stats_ = {};
        stats_.attempted = true;
        stats_.running = true;
    }
    running_.store (true);
    worker_ = std::thread ([this, hwnd] { Run (hwnd); });
    return true;
}

void DiligentProbe::Stop ()
{
    if (worker_.joinable ())
        worker_.join ();
}

DiligentProbeStats DiligentProbe::Stats () const
{
    std::lock_guard<std::mutex> lock (mutex_);
    return stats_;
}

void DiligentProbe::Run (void*)
{
    ArchVizLog ("Diligent 1c: render thread entered; DG child HWND is valid. Creating D3D11 device only (no swap chain).");
    Diligent::EngineD3D11CreateInfo createInfo;
    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device;
    // The API takes a raw IDeviceContext**. A scalar RefCntAutoPtr exposes the
    // required out-parameter helper; an array of wrappers does not.
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> context;
    Diligent::GetEngineFactoryD3D11 ()->CreateDeviceAndContextsD3D11 (createInfo, &device, &context);

    const bool succeeded = device != nullptr && context != nullptr;
    std::string error;
    if (!succeeded)
        error = "CreateDeviceAndContextsD3D11 returned no render device or immediate context.";

    {
        std::lock_guard<std::mutex> lock (mutex_);
        stats_.running = false;
        stats_.succeeded = succeeded;
        stats_.error = error;
    }
    running_.store (false);
    ArchVizLog (succeeded ? "Diligent 1c: PASS — IRenderDevice and immediate context created and released."
                          : "Diligent 1c: FAIL — device creation returned incomplete objects. Restart Archicad before any retry.");
}

} // namespace geomsrv::archviz
