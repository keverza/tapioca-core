#include "NodeGraph/Graph.hpp"

namespace evp::nodegraph {

const Node* GraphDocument::FindNode (const NodeId& nodeId) const
{
    const auto iterator = nodes_.find (nodeId);
    return iterator == nodes_.end () ? nullptr : &iterator->second;
}

// ⚠️ THESE SPELLINGS ARE THE PERSISTED FORMAT AND THE WIRE FORMAT AT ONCE.
// A saved graph carries them, so renaming one silently changes every stored
// workflow's meaning. Add a name; never repurpose one.
const char* ExecutionModeName (ExecutionMode mode)
{
    switch (mode) {
        case ExecutionMode::Enabled:
            return "enabled";
        case ExecutionMode::Disabled:
            return "disabled";
        case ExecutionMode::Bypassed:
            return "bypassed";
        case ExecutionMode::Holding:
            return "holding";
    }
    return "enabled";
}

bool ParseExecutionMode (const std::string& name, ExecutionMode& mode)
{
    if (name == "enabled") {
        mode = ExecutionMode::Enabled;
        return true;
    }
    if (name == "disabled") {
        mode = ExecutionMode::Disabled;
        return true;
    }
    if (name == "bypassed") {
        mode = ExecutionMode::Bypassed;
        return true;
    }
    if (name == "holding") {
        mode = ExecutionMode::Holding;
        return true;
    }
    return false;
}

} // namespace evp::nodegraph
