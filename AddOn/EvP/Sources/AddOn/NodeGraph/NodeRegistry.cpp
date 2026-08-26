#include "NodeGraph/NodeRegistry.hpp"

#include <set>

namespace evp::nodegraph {
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
