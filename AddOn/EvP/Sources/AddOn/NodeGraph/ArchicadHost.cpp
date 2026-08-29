#include "NodeGraph/ArchicadHost.hpp"

#include <atomic>

namespace evp::nodegraph {
namespace {

// Atomic rather than mutex-guarded: this is read on every plan and written twice
// in the process lifetime, and a reader must never block a writer that is
// detaching during teardown.
std::atomic<IArchicadHost*> gHost { nullptr };

} // namespace

IArchicadHost* ActiveArchicadHost ()
{
    return gHost.load (std::memory_order_acquire);
}

void SetActiveArchicadHost (IArchicadHost* host)
{
    gHost.store (host, std::memory_order_release);
}

} // namespace evp::nodegraph
