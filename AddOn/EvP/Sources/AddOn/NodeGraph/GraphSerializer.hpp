#ifndef EVP_NODEGRAPH_GRAPHSERIALIZER_HPP
#define EVP_NODEGRAPH_GRAPHSERIALIZER_HPP

// A graph document as a portable file.
//
// WHAT IS PERSISTED, and the list is short on purpose (architecture doc §34):
// nodes, their types and parameters, edges, editor layout metadata, and a
// format version. NOT persisted: execution state, cached results, output
// revisions, run history, run ids. A graph is a PROGRAM; a run is not part of
// it, and a file that carried one would reload a stale answer as though it were
// current.
//
// ⚠️ LOADING GOES THROUGH ApplyEdit, NODE BY NODE AND EDGE BY EDGE. It would be
// faster to assign the maps directly, and that is exactly the shortcut that lets
// a hand-edited or downgraded file introduce a cycle, a dangling edge or an
// unknown node type into a document the evaluator assumes is valid. A loaded
// graph satisfies every invariant an edited one does, because it is built out of
// the same validated edits. A file that cannot satisfy them is REJECTED naming
// the node - never silently repaired, per §10.
//
// ⚠️ A MESH CANNOT BE A PERSISTED PARAMETER. The value vocabulary has one, but a
// mesh is something a node PRODUCES, and writing one into a graph file would put
// a cached result inside the program that computes it. Serialising a mesh
// parameter fails and says so.

#include "NodeGraph/Graph.hpp"

#include <cstdint>
#include <map>
#include <string>

namespace evp::nodegraph {

class NodeRegistry;

// Bumped only when the on-disk shape changes in a way an older reader cannot
// understand. Adding an OPTIONAL member is not such a change: a reader treats an
// absent member as its default, which is what lets node schema versions arrive
// later without invalidating every file written before them.
constexpr uint32_t kGraphFormatVersion = 1;

constexpr const char* kGraphFormatName = "tapioca-nodegraph";

// Everything about a stored graph that is not the graph. Editor layout lives
// here rather than on Node, so the evaluator cannot come to depend on it.
struct GraphMetadata {
    std::string label;
    std::string description;

    // Opaque to the runtime, round-tripped for the client that wrote it. Node
    // positions live here. The runtime never reads a member of it, which is the
    // point: an editor can add a field without a format change and without the
    // evaluator acquiring an opinion about layout.
    std::map<NodeId, std::map<std::string, std::string>> nodeLayout;
};

struct SerializedGraph {
    GraphDocument document;
    GraphMetadata metadata;
    uint32_t formatVersion = kGraphFormatVersion;
};

struct SerializeResult {
    bool ok = false;
    std::string error;
    std::string text;
};

struct DeserializeResult {
    bool ok = false;

    // Names the node or edge that could not be loaded, so a broken file is
    // repairable rather than merely refused.
    std::string error;

    SerializedGraph graph;
};

// `indent` 0 writes one line; the default pretty-prints, because a workflow file
// belongs in a diff.
SerializeResult SerializeGraph (const GraphDocument& document, const GraphMetadata& metadata, size_t indent = 2);

// Rebuilds the document through validated edits against `registry`. Fails on an
// unknown format, a newer format version, an unknown node type, a bad parameter,
// a dangling edge or a cycle.
DeserializeResult DeserializeGraph (const std::string& text, const NodeRegistry& registry);

} // namespace evp::nodegraph

#endif
