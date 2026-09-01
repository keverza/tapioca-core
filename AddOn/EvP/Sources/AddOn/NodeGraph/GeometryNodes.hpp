#ifndef EVP_NODEGRAPH_GEOMETRYNODES_HPP
#define EVP_NODEGRAPH_GEOMETRYNODES_HPP

// The geometry and transform node family.
//
// Split out of BuiltinNodes for the reason ArchicadNodes is: a node FAMILY is
// the unit of ownership here, and one file carrying the inputs, the inspection
// nodes, the flow control AND every solid, curve and transform had grown past
// the point where anyone could find the one they came for.
//
// ⚠️ EVERYTHING HERE IS PURE AND RUNS ON A WORKER. No node in this family reads
// the model or writes to it - a Box is a Box whether or not a project is open -
// which is what lets the whole family be exercised offline by calling it.
//
// The modelling itself lives in Geometry/: Primitives builds the solids,
// Curves the arcs and the sampling, Transforms the matrices. This file is the
// catalog entry and the argument coercion, and deliberately nothing else.

#include "NodeGraph/Evaluator.hpp"
#include "NodeGraph/NodeRegistry.hpp"

namespace evp::nodegraph {

void RegisterGeometryNodes (NodeRegistry& registry);

// Executes a geometry-family node. Returns false with `error` set when the node
// is this family's and it failed, so the dispatcher can tell that from "not
// mine" - which `IsGeometryNodeType` answers first.
bool ExecuteGeometryNode (const Node& node, const ValueMap& inputs, const NodeExecutionContext& context,
                          ValueMap& outputs, std::string& error);

bool IsGeometryNodeType (const std::string& nodeTypeId);

} // namespace evp::nodegraph

#endif
