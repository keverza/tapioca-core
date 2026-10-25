// TapiocaDxrWorker.exe -- the out-of-process DXR ray worker (G2).
//
//   TapiocaDxrWorker --pipe <name> --segments <prefix>   serve the add-on
//   TapiocaDxrWorker --describe                          print the adapter check
//
// The add-on launches it lazily (RayWorkerSupervisor) inside a kill-on-close
// job; it owns the only D3D12 device used for analysis. --describe is the G0
// capability record: adapter, DXR tier and driver, or why there is none.

#include "DxrWorker/DxrBackend.hpp"
#include "RayWorker/RayWorkerWin32.hpp"

#include <cstdio>
#include <cwchar>

int wmain (int argc, wchar_t** argv)
{
    using namespace evp::rayworker;
    if (argc == 2 && std::wcscmp (argv[1], L"--describe") == 0) {
        const protocol::HelloPayload hello = evp::dxrworker::MakeDxrBackend ()->Describe ();
        std::printf ("reason=%s device=\"%s\" tier=%u driver=%u memoryBytes=%llu\n", GpuReasonCode (hello.reason),
                     hello.deviceName.c_str (), hello.runtimeVersion, hello.driverVersion,
                     static_cast<unsigned long long> (hello.deviceMemoryBytes));
        return hello.reason == GpuReason::None ? 0 : 1;
    }
    std::wstring pipe;
    std::string segments;
    if (!win32::ParseWorkerArgs (argc, argv, pipe, segments)) {
        std::fprintf (stderr, "usage: TapiocaDxrWorker --pipe <name> --segments <prefix> | --describe\n");
        return 64;
    }
    return win32::RunWorker (pipe, segments, evp::dxrworker::MakeDxrBackend ());
}
