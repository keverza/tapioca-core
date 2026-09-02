#include "NodeGraph/ScriptRuntime.hpp"

#include <atomic>

namespace evp::nodegraph {
namespace {

// One slot per language, atomic for the reason ArchicadHost's pointer is: read
// on every script evaluation and written twice in the process lifetime.
std::atomic<IScriptRuntime*> gRuntimes[2] { { nullptr }, { nullptr } };

size_t SlotOf (ScriptLanguage language)
{
    return language == ScriptLanguage::Python ? 1 : 0;
}

} // namespace

IScriptRuntime* ActiveScriptRuntime (ScriptLanguage language)
{
    return gRuntimes[SlotOf (language)].load (std::memory_order_acquire);
}

void SetActiveScriptRuntime (ScriptLanguage language, IScriptRuntime* runtime)
{
    gRuntimes[SlotOf (language)].store (runtime, std::memory_order_release);
}

} // namespace evp::nodegraph
