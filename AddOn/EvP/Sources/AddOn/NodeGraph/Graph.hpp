#ifndef EVP_NODEGRAPH_GRAPH_HPP
#define EVP_NODEGRAPH_GRAPH_HPP

#include "NodeGraph/NodeType.hpp"
#include "NodeGraph/Value.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace evp::nodegraph {

using NodeId = std::string;
using PortId = std::string;
using GraphId = std::string;

// Run identity lives with the other identity types so the event stream can name
// a run without depending on the execution context that creates one.
using RunId = uint64_t;

constexpr RunId kNoRun = 0;

// Stage F: what the user has told the graph to do with this node, as opposed to
// what happened to it on the last run.
//
// ⚠️ A MODE IS DOCUMENT STATE AND A STATUS IS RUN STATE. They are separate types
// because they have separate lifetimes: a mode is authored, persists with the
// graph and survives a restart; a status is produced by an evaluation and is
// session-only. Collapsing them - "the node is disabled" as a status - is what
// makes a reload silently re-enable everything the user switched off.
enum class ExecutionMode {
    // Normal evaluation and caching.
    Enabled,

    // Does not execute and publishes no output. Consumers become Blocked with a
    // dependency reason rather than Error: nothing is broken.
    Disabled,

    // Does not execute; the type's declared input-to-output mappings forward
    // compatible values, so consumers still run. Legal ONLY for a type that
    // declares unambiguous mappings - see NodeType::bypassMappings.
    Bypassed,

    // Data Dam. The node executes, but its result is STAGED rather than
    // published; consumers keep seeing the last released value until an explicit
    // release promotes the staged one. Legal only for a hold-capable type.
    Holding,
};

const char* ExecutionModeName (ExecutionMode mode);

// Parses the wire spelling. False for anything else, so an unknown mode from a
// client is a rejection rather than a silent fall back to Enabled.
bool ParseExecutionMode (const std::string& name, ExecutionMode& mode);

// What one input port does to what arrives, before the node sees it.
//
// ⚠️ A MODIFIER IS NOT A HIDDEN NODE. It exists because the alternative - a
// Flatten node on every one of six inputs - buries the graph's actual subject
// under plumbing, and everyone who has used a node editor for an afternoon
// reaches for it. What keeps it honest is that it is DECLARED on the port, saved
// with the document, and drawn on the node: it changes what a port receives, and
// a reader can see that it does.
//
// The reshaping three are exactly `tree.flatten`, `tree.graft` and
// `tree.simplify` - the same operations, applied at the port instead of on the
// canvas - so there is one implementation and a modifier can never drift from
// the node that shares its name.
enum class PortModifier {
    None,
    Flatten,
    Graft,
    Simplify,

    // Every branch's items in the opposite order. Shape is untouched - the same
    // branches hold the same number of items - so this is a reshaping modifier
    // like the three above it, not a conversion.
    Reverse,

    // ⚠️ THE ONE THAT CHANGES A TYPE, AND THEREFORE THE ONE THAT CHANGES WHAT
    // MAY BE CONNECTED. A Double cannot reach an Integer port, because 2.5 is
    // two or three depending on an answer only the author has. This modifier IS
    // that answer, given once on the port: nearest. The other three roundings
    // stay on `math.toInteger`, where each has a name a reader can see - a
    // modifier is a small badge on a port and cannot carry four meanings.
    Round,

    // ⚠️ THIS IS WHAT "REPARAMETERIZE" WOULD HAVE BEEN, UNDER A NAME THAT IS
    // TRUE HERE. Grasshopper's Reparameterize maps a curve's parameter DOMAIN
    // onto 0..1, and nothing in this catalog has one: a Polyline is a list of
    // points with no parameterisation to remap. Borrowing the word would have
    // promised an operation the geometry cannot perform.
    //
    // What IS well defined is the numeric version of the same idea: the numbers
    // in a branch remapped onto 0..1, smallest to largest. That is the useful
    // half - driving a colour or a radius from measurements in any range - and
    // it is named for what it does.
    //
    // Like Round, it acts on Doubles and leaves any other item type alone; by
    // the time a modifier runs, a port that declared Double has already had an
    // Integer input widened for it.
    Normalise,
};

const char* PortModifierName (PortModifier modifier);

// Parses the wire spelling. False for anything else, so an unknown modifier from
// a client is a rejection rather than a silent "none" that quietly changes what
// a saved graph computes.
bool ParsePortModifier (const std::string& name, PortModifier& modifier);

struct Node {
    NodeId id;
    std::string nodeType;
    std::map<std::string, Argument> parameters;

    // Per-input-port modifiers, keyed by port id. Absent means None, so a graph
    // that uses no modifiers carries no trace of them.
    std::map<std::string, PortModifier> inputModifiers;

    // Persisted with the graph. See ExecutionMode.
    ExecutionMode executionMode = ExecutionMode::Enabled;

    // Ports declared by this INSTANCE rather than by its type, and read only
    // when the type sets NodeType::instancePorts.
    //
    // â ï¸ EVERY OTHER TYPE IN THE CATALOG STILL OWNS ITS PORTS, AND MUST.
    // A type's port list is a contract a graph file, a library component and a
    // client all rely on; making it per-node everywhere would mean no client
    // could draw a node it had not first fetched an instance of. These exist for
    // the one family whose interface is genuinely authored elsewhere - a script
    // file the user edits in VSCode - where the type cannot know the ports
    // because the type is not where they are written down.
    //
    // Resolution goes through ResolvedInputs/ResolvedOutputs in NodeRegistry.hpp,
    // never by reading either list directly, so a type that does not opt in can
    // never be given ports by a hand-edited document.
    std::vector<PortSchema> dynamicInputs;
    std::vector<PortSchema> dynamicOutputs;
};

struct Edge {
    NodeId sourceNode;
    PortId sourcePort;
    NodeId targetNode;
    PortId targetPort;
};

class NodeRegistry;
struct GraphEdit;
struct EditResult;

class GraphDocument {
  public:
    const std::map<NodeId, Node>& Nodes () const
    {
        return nodes_;
    }
    const std::vector<Edge>& Edges () const
    {
        return edges_;
    }
    const Node* FindNode (const NodeId& nodeId) const;
    uint64_t Revision () const
    {
        return revision_;
    }

    // Take another document's CONTENT, under a NEW revision.
    //
    // ⚠️ THE REVISION MUST NOT GO BACKWARDS, WHICH IS WHY THIS IS NOT `*this =
    // other`. A plain assignment would restore the old counter too, and the
    // revision is the client's change TOKEN, not a version label: an editor that
    // saw revision 9, undid to 7 and then saw 8 again would conclude nothing had
    // changed since it last read, and would keep showing the graph it had. So
    // the content rewinds and the counter keeps climbing - undo is a new state
    // of the document, not a return to an old one.
    //
    // Used by the batch transaction to roll back a refused edit, and by undo and
    // redo. All three want exactly this: content from a snapshot, identity that
    // still moves forward.
    void RestoreContent (const GraphDocument& other);

  private:
    friend EditResult ApplyEdit (GraphDocument&, const NodeRegistry&, const GraphEdit&);

    std::map<NodeId, Node> nodes_;
    std::vector<Edge> edges_;
    uint64_t revision_ = 0;
};

} // namespace evp::nodegraph

#endif
