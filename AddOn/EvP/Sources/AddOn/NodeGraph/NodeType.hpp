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
