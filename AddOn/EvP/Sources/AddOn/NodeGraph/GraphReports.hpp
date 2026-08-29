#ifndef EVP_NODEGRAPH_GRAPHREPORTS_HPP
#define EVP_NODEGRAPH_GRAPHREPORTS_HPP

// Can this graph load, and can it run?
//
// Both questions are answered by walking the document once, resolving everything
// it names, and asking different things of the same answers. The handoff
// specified GraphDependencyReport and CompatibilityReport as separate
// subsystems; they differ only in WHEN they run and which findings they care
// about, and two walkers over one truth drift. So: one pass, two projections.

#include "NodeGraph/Graph.hpp"
#include "NodeGraph/NodeRegistry.hpp"
#include "NodeGraph/ReferenceResolver.hpp"

#include <string>
#include <vector>

namespace evp::nodegraph {

class IArchicadHost;

enum class FindingSeverity {
    // The graph runs, but something is worth saying.
    Warning,

    // The graph cannot run as it stands.
    Error,
};

const char* FindingSeverityName (FindingSeverity severity);

struct GraphFinding {
    FindingSeverity severity = FindingSeverity::Warning;

    // Empty when the finding is about the document rather than one node.
    NodeId nodeId;

    // A short machine-readable kind, so a client can group or filter without
    // parsing prose: "missingNodeType", "unresolvedReference", "hostUnavailable",
    // "unsampleableGeneration", "effectRequiresExplicitRun".
    std::string kind;

    // For a reference finding, the status that produced it.
    std::string detail;
};

// Every resource the document names, and what it resolves to now. This is the
// single pass; both reports below are computed from it.
struct ReferenceSite {
    NodeId nodeId;
    std::string parameterId;
    Reference reference;
    ReferenceResolution resolution;
};

struct GraphResolution {
    std::vector<ReferenceSite> sites;
    std::vector<GraphFinding> findings;

    // True when nothing in `findings` is an Error.
    bool Usable () const;
};

// Walks the document once. `host` may be nullptr, in which case Archicad-domain
// nodes and every reference are reported as unavailable rather than assumed fine.
GraphResolution ResolveGraph (const GraphDocument& document, const NodeRegistry& registry, IArchicadHost* host);

// --- Projection one: can it run now? ---------------------------------------

struct GraphDependencyReport {
    bool canEvaluate = false;
    std::vector<GraphFinding> findings;

    // Counts, so a client can summarize without walking the list.
    size_t resolvedReferences = 0;
    size_t unresolvedReferences = 0;
    size_t nodesNeedingArchicad = 0;
    size_t effectNodes = 0;
};

GraphDependencyReport MakeDependencyReport (const GraphDocument& document, const NodeRegistry& registry,
                                            const GraphResolution& resolution);

// --- Projection two: can it load at all? -----------------------------------

enum class CompatibilityStatus {
    Compatible,
    NeedsMigration,
    UnsupportedFormat,
    MissingNodeType,
    MissingCapability,
};

const char* CompatibilityStatusName (CompatibilityStatus status);

struct CompatibilityReport {
    CompatibilityStatus status = CompatibilityStatus::Compatible;
    std::vector<GraphFinding> findings;

    // Node types the document uses that the registry does not have. These stay
    // visible in the editor so the workflow can be repaired rather than lost.
    std::vector<std::string> missingNodeTypes;
};

// A graph's format version is checked against what this runtime understands.
// `formatVersion` of 0 means "not stated", which is how an in-memory document
// arrives and is treated as current.
CompatibilityReport MakeCompatibilityReport (const GraphDocument& document, const NodeRegistry& registry,
                                             const GraphResolution& resolution, uint32_t formatVersion);

// The graph format this runtime writes and reads.
constexpr uint32_t kGraphFormatVersion = 1;

} // namespace evp::nodegraph

#endif
