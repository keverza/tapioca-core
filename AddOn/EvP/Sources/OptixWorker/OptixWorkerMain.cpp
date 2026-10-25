// TapiocaOptixWorker.exe -- the out-of-process OptiX ray worker (G3).
//
//   TapiocaOptixWorker --pipe <name> --segments <prefix>   serve the add-on
//   TapiocaOptixWorker --describe                          print the device check
//
// The add-on launches it lazily inside a kill-on-close job; it owns the only
// CUDA context used for analysis. --describe is the G0 capability record:
// device, compute capability, driver CUDA level and OptiX version, or why not.

#include "OptixWorker/OptixBackend.hpp"
#include "RayWorker/RayWorkerWin32.hpp"

#include <cstdio>
#include <cwchar>

int wmain (int argc, wchar_t** argv)
{
    using namespace evp::rayworker;
    if (argc == 2 && std::wcscmp (argv[1], L"--describe") == 0) {
        const protocol::HelloPayload hello = evp::optixworker::MakeOptixBackend ()->Describe ();
        std::printf ("reason=%s device=\"%s\" compute=%u driverCuda=%u optix=%u memoryBytes=%llu\n",
                     GpuReasonCode (hello.reason), hello.deviceName.c_str (), hello.computeCapability,
                     hello.driverVersion, hello.runtimeVersion,
                     static_cast<unsigned long long> (hello.deviceMemoryBytes));
        return hello.reason == GpuReason::None ? 0 : 1;
    }
    std::wstring pipe;
    std::string segments;
    if (!win32::ParseWorkerArgs (argc, argv, pipe, segments)) {
        std::fprintf (stderr, "usage: TapiocaOptixWorker --pipe <name> --segments <prefix> | --describe\n");
        return 64;
    }
    return win32::RunWorker (pipe, segments, evp::optixworker::MakeOptixBackend ());
}
