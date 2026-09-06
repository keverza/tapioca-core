#include "NodeGraph/CaptureService.hpp"

#include <atomic>
#include <mutex>
#include <string>

namespace evp::nodegraph {
namespace {

// Atomic rather than mutex-guarded, for the same reason ArchicadHost.cpp's is:
// read whenever a capture node runs, written twice in the process lifetime, and
// a reader must never block the writer detaching during teardown.
std::atomic<ICaptureService*> gCaptureService { nullptr };

} // namespace

ICaptureService* ActiveCaptureService ()
{
    return gCaptureService.load (std::memory_order_acquire);
}

void SetActiveCaptureService (ICaptureService* service)
{
    gCaptureService.store (service, std::memory_order_release);
}

namespace {

// A mutex rather than an atomic, because a std::string is not one. Held for the
// length of a copy and nothing else.
std::mutex gProgressMutex;
std::string gProgress;

} // namespace

void ReportCaptureProgress (const std::string& progress)
{
    std::lock_guard<std::mutex> lock (gProgressMutex);
    gProgress = progress;
}

std::string CaptureProgress ()
{
    std::lock_guard<std::mutex> lock (gProgressMutex);
    return gProgress;
}

} // namespace evp::nodegraph
