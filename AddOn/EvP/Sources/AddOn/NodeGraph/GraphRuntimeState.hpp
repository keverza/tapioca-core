#ifndef EVP_NODEGRAPH_GRAPHRUNTIMESTATE_HPP
#define EVP_NODEGRAPH_GRAPHRUNTIMESTATE_HPP

#include "NodeGraph/Evaluator.hpp"
#include "NodeGraph/GraphEdit.hpp"
#include "NodeGraph/NodeRegistry.hpp"

#include <mutex>

namespace evp::nodegraph {

struct EvaluationSummary {
    bool succeeded = false;
    std::string error;
    uint64_t revision = 0;
    size_t nodeCount = 0;
};

struct RuntimeNodeResult {
    NodeId nodeId;
    NodeStatus status;
    std::shared_ptr<const NodeResult> result;
};

struct ResultsSnapshot {
    uint64_t revision = 0;
    std::vector<RuntimeNodeResult> nodes;
};

class GraphRuntimeState final {
  public:
    static GraphRuntimeState& Get ();

    NodeRegistry Catalog () const;
    GraphDocument Document () const;
    EditResult Apply (const GraphEdit& edit);
    EvaluationSummary Evaluate ();
    ResultsSnapshot Results () const;

  private:
    GraphRuntimeState ();

    mutable std::mutex mutex_;
    NodeRegistry registry_;
    GraphDocument document_;
    Evaluator evaluator_;
};

} // namespace evp::nodegraph

#endif
