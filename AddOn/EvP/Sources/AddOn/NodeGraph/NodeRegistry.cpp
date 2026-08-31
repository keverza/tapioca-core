#include "NodeGraph/NodeRegistry.hpp"

#include <set>

namespace evp::nodegraph {

const char* ExecutionDomainName (ExecutionDomain domain)
{
    switch (domain) {
        case ExecutionDomain::Worker:
            return "worker";
        case ExecutionDomain::ArchicadMainThread:
            return "archicadMainThread";
        case ExecutionDomain::RenderThread:
            return "renderThread";
    }
    return "worker";
}

const char* NodeDisplayName (NodeDisplay display)
{
    switch (display) {
        case NodeDisplay::Ports:
            return "ports";
        case NodeDisplay::Text:
            return "text";
        case NodeDisplay::Preview:
            return "preview";
        case NodeDisplay::SelectionSet:
            return "selectionSet";
    }
    return "ports";
}

const char* EffectKindName (EffectKind effect)
{
    switch (effect) {
        case EffectKind::Pure:
            return "pure";
        case EffectKind::ReadModel:
            return "readModel";
        case EffectKind::HostUiWrite:
            return "hostUiWrite";
    }
    return "pure";
}
namespace {

template <typename Schema> bool ValidateIds (const std::vector<Schema>& schemas, const char* kind, std::string& error)
{
    std::set<std::string> ids;
    for (const Schema& schema : schemas) {
        if (schema.id.empty () || !ids.insert (schema.id).second) {
            error = std::string ("invalid or duplicate ") + kind + " id: " + schema.id;
            return false;
        }
    }
    return true;
}

} // namespace

bool NodeRegistry::Register (NodeType nodeType, std::string& error)
{
    if (nodeType.id.empty ()) {
        error = "node type id is empty";
        return false;
    }
    if (types_.contains (nodeType.id)) {
        error = "node type already registered: " + nodeType.id;
        return false;
    }
    if (!ValidateIds (nodeType.inputs, "input", error) || !ValidateIds (nodeType.outputs, "output", error) ||
        !ValidateIds (nodeType.parameters, "parameter", error))
        return false;

    for (const ParameterSchema& parameter : nodeType.parameters) {
        if (parameter.defaultValue && parameter.defaultValue->Type () != parameter.valueType) {
            error = "default value type mismatch for parameter: " + parameter.id;
            return false;
        }
    }

    // Stage F3. Checked HERE, once, rather than on every bypassed evaluation:
    // an ambiguous or ill-typed table is a defect in the node type, and a defect
    // in a node type should stop it entering the catalog rather than surface as
    // a puzzling run-time result later.
    std::set<std::string> mappedInputs;
    std::set<std::string> mappedOutputs;
    for (const BypassMapping& mapping : nodeType.bypassMappings) {
        const PortSchema* input = FindInput (nodeType, mapping.inputId);
        const PortSchema* output = FindOutput (nodeType, mapping.outputId);
        if (input == nullptr || output == nullptr) {
            error =
                "bypass mapping names a port this type does not have: " + mapping.inputId + " -> " + mapping.outputId;
            return false;
        }
        if (input->valueType != output->valueType) {
            error = "bypass mapping is not type-compatible: " + mapping.inputId + " -> " + mapping.outputId;
            return false;
        }
        // One output may be fed by exactly one input and vice versa. Two
        // mappings onto one output is the ambiguity the declaration exists to
        // remove.
        if (!mappedInputs.insert (mapping.inputId).second || !mappedOutputs.insert (mapping.outputId).second) {
            error =
                "bypass mapping is ambiguous; a port is mapped twice: " + mapping.inputId + " -> " + mapping.outputId;
            return false;
        }
    }
    // A partial table would leave a required output absent while bypassed, which
    // reads to a consumer as a broken node rather than a bypassed one.
    if (!nodeType.bypassMappings.empty ()) {
        for (const PortSchema& output : nodeType.outputs) {
            if (output.required && !mappedOutputs.contains (output.id)) {
                error = "bypass mapping leaves required output '" + output.id + "' unfed";
                return false;
            }
        }
    }

    types_.emplace (nodeType.id, std::move (nodeType));
    error.clear ();
    return true;
}

const NodeType* NodeRegistry::Find (const std::string& nodeTypeId) const
{
    const auto iterator = types_.find (nodeTypeId);
    return iterator == types_.end () ? nullptr : &iterator->second;
}

const PortSchema* FindInput (const NodeType& nodeType, const std::string& portId)
{
    for (const PortSchema& port : nodeType.inputs)
        if (port.id == portId)
            return &port;
    return nullptr;
}

const PortSchema* FindOutput (const NodeType& nodeType, const std::string& portId)
{
    for (const PortSchema& port : nodeType.outputs)
        if (port.id == portId)
            return &port;
    return nullptr;
}

const ParameterSchema* FindParameter (const NodeType& nodeType, const std::string& parameterId)
{
    for (const ParameterSchema& parameter : nodeType.parameters)
        if (parameter.id == parameterId)
            return &parameter;
    return nullptr;
}

} // namespace evp::nodegraph
