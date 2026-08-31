#ifndef EVP_NODEGRAPH_NODETYPE_HPP
#define EVP_NODEGRAPH_NODETYPE_HPP

#include "NodeGraph/ProjectGenerations.hpp"
#include "NodeGraph/Value.hpp"

#include <optional>
#include <string>
#include <vector>

namespace evp::nodegraph {

// WHERE a node runs.
enum class ExecutionDomain {
    Worker,
    ArchicadMainThread,
    RenderThread,
};

const char* ExecutionDomainName (ExecutionDomain domain);

// WHAT a node does to the world outside the graph. Deliberately separate from
// ExecutionDomain: the executor picks a thread from the domain, and the
// evaluator decides scheduling and permission from the effect.
enum class EffectKind {
    // Computes from its inputs and nothing else.
    Pure,

    // Reads host state. Safe on any evaluation, including a preview - reading
    // cannot surprise the user.
    ReadModel,

    // Changes what the user sees, without changing the model. Selection is the
    // only member today. These are DEFERRED and permissioned: see
    // EvaluationRequest::allowSideEffects.
    HostUiWrite,
};

const char* EffectKindName (EffectKind effect);

// Deliberately no WriteModel. ADR-007 excludes model writes from this track, and
// an unused enum value is an invitation. The write pipeline is designed for in
// the architecture document sections 26-27 and arrives with the value.

// How a client should draw the node's body.
//
// A hint, not a command: a client is free to ignore it. It exists because the
// editor was branching on node id - `nodeType === 'watch'` - which means every
// new inspectable node needs a frontend change, and a renamed node silently
// loses its rendering. The runtime already owns the schema; it should own this
// too.
enum class NodeDisplay {
    // Ports and status only. The default.
    Ports,

    // The node has something to READ. A client should show the node's `text`
    // output in the body, scrollable. This is the Grasshopper-panel shape.
    Text,

    // The node has something to LOOK at; a client may host a viewport.
    Preview,

    // The node OWNS a set of Archicad elements and is operated on directly:
    // Update, Add, Remove, Reselect, Clear, exactly as the command palette's
    // selection rows. A client should draw those five actions and the set's
    // size. The actions are native verbs, not evaluations - see
    // NodeGraphSelectionCommands.cpp.
    SelectionSet,
};

const char* NodeDisplayName (NodeDisplay display);

struct PortSchema {
    std::string id;
    std::string label;
    ValueType valueType = ValueType::Absent;
    bool required = true;
    bool acceptsMultiple = false;
};

struct ParameterSchema {
    std::string id;
    std::string label;
    ValueType valueType = ValueType::Absent;
    bool required = false;
    std::optional<Value> defaultValue;
};

struct NodeType {
    std::string id;
    std::string label;
    std::string category;
    std::string description;
    ExecutionDomain executionDomain = ExecutionDomain::Worker;
    EffectKind effect = EffectKind::Pure;
    NodeDisplay display = NodeDisplay::Ports;

    // Host state this node reads that its ports do not express. Folded into the
    // node's cache key, so declaring a domain is what makes the node re-run when
    // that domain changes - and failing to declare one is what makes it serve a
    // stale answer.
    GenerationSet generations;

    std::vector<PortSchema> inputs;
    std::vector<PortSchema> outputs;
    std::vector<ParameterSchema> parameters;
};

} // namespace evp::nodegraph

#endif
