#ifndef EVP_NODEGRAPH_INPUTGATHERING_HPP
#define EVP_NODEGRAPH_INPUTGATHERING_HPP

// What each of a node's input ports actually receives, before anything runs.
//
// Separated from the evaluator because it answers a different question. The
// evaluator decides WHETHER a node runs - is it dirty, is it cached, is it
// blocked, which thread. This decides WHAT it would receive if it did: which
// edges feed a port, what happens when several do, what a port with no edge
// falls back to, and which of those situations is a failure.
//
// Pulling it out of Evaluator::PrepareNode also makes it reachable on its own.
// The rules here - an edge beats a typed-in value, several edges merge under
// the port's declared contract, an unwired optional port receives the EMPTY
// tree rather than no tree - are ones a test should be able to state directly,
// rather than by building a graph and running it to see what a body was handed.

#include "NodeGraph/Evaluator.hpp"
#include "NodeGraph/Graph.hpp"
#include "NodeGraph/NodeType.hpp"

#include <functional>
#include <string>

namespace evp::nodegraph {

// One upstream node's last published result, or null when it has none. A
// callback rather than the evaluator's cache, so gathering does not need to
// know how results are stored or reach into a private member to find out.
using UpstreamResultLookup = std::function<std::shared_ptr<const NodeResult> (const NodeId&)>;

// Fills `inputs` with one tree per declared input port, and folds each
// contributing edge into `inputHash` so that re-running with a changed upstream
// misses the cache.
//
// Returns false with `error` set when an upstream result is missing, an
// internalised value cannot become a tree of the port's type, a required port
// is unconnected, or a fan-in refused. Every one of those is the caller's to
// report - they are all the same kind of failure and the caller owns the code.
// One port's modifier applied to what arrived. Exposed because it is the whole
// meaning of a modifier and deserves to be stated by a test directly, rather
// than inferred from a graph that ran.
//
// Fails only when a reshaping operation refuses - the modifier vocabulary is
// closed, so an unknown one cannot reach here.
bool ApplyPortModifier (PortModifier modifier, const data::TreeValue& input, data::TreeValue& result,
                        std::string& error);

bool GatherNodeInputs (const GraphDocument& document, const NodeId& nodeId, const Node& node, const NodeType& nodeType,
                       const UpstreamResultLookup& upstream, TreeMap& inputs, size_t& inputHash, std::string& error);

} // namespace evp::nodegraph

#endif
