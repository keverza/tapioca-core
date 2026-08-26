#ifndef EVP_NODEGRAPH_EVALUATOR_HPP
#define EVP_NODEGRAPH_EVALUATOR_HPP

#include "NodeGraph/Graph.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace evp::nodegraph {

using ValueMap = std::map<std::string, Value>;

struct NodeResult {
    ValueMap outputs;
    double durationMilliseconds = 0.0;
};

enum class NodeExecutionState { Dirty, Complete, Failed, Blocked };

struct NodeStatus {
    NodeExecutionState state = NodeExecutionState::Dirty;
    std::string message;
};

using NodeExecutor = std::function<bool (const Node&, const ValueMap&, ValueMap&, std::string&)>;

class NodeRegistry;

class Evaluator {
  public:
    void Invalidate (const GraphDocument& document, const std::vector<NodeId>& roots);
    bool Evaluate (const GraphDocument& document, const NodeRegistry& registry, const NodeExecutor& executor,
                   std::string& error);
    std::shared_ptr<const NodeResult> Result (const NodeId& nodeId) const;
    NodeStatus Status (const NodeId& nodeId) const;
    bool IsDirty (const NodeId& nodeId) const;

  private:
    struct CacheEntry {
        bool dirty = true;
        size_t inputHash = 0;
        std::shared_ptr<const NodeResult> result;
        NodeStatus status;
    };

    std::map<NodeId, CacheEntry> cache_;
};

} // namespace evp::nodegraph

#endif
