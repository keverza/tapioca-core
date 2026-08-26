#ifndef EVP_NODEGRAPH_NODETYPE_HPP
#define EVP_NODEGRAPH_NODETYPE_HPP

#include "NodeGraph/Value.hpp"

#include <optional>
#include <string>
#include <vector>

namespace evp::nodegraph {

enum class ExecutionDomain {
    Worker,
    ArchicadMainThread,
    RenderThread,
};

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
    std::vector<PortSchema> inputs;
    std::vector<PortSchema> outputs;
    std::vector<ParameterSchema> parameters;
};

} // namespace evp::nodegraph

#endif
