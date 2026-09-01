#ifndef EVP_NODEGRAPH_PREVIEWPROJECTION_HPP
#define EVP_NODEGRAPH_PREVIEWPROJECTION_HPP

// A graph's results -> what the viewport should draw.
//
// ⚠️ PREVIEW IS A PROJECTION OF THE RESULTS, NOT A SIDE EFFECT OF A NODE BODY,
// and that is the whole design decision in this file. The obvious alternative --
// a Preview node whose body pushes into the preview store -- was rejected twice
// over:
//
//   1. It would make the node EFFECTFUL. EffectKind::HostUiWrite is deferred to
//      a second phase and REFUSED unless the request set allowSideEffects, which
//      only a deliberate Run does. A preview that only appears when you press Run
//      is not a preview; the entire point is that it follows a slider drag.
//   2. It would make the node body impure, and a body that reaches a process-wide
//      store is a body that cannot be tested by calling it.
//
// Projecting instead means the node stays Pure, the preview updates on every
// evaluation including a preview run, and the whole conversion is a function from
// a document plus a result lookup to a vector of primitives - which is exactly
// what an offline test can pin.
//
// ⚠️ IT READS THE RESULTS SNAPSHOT, NOT THE RUN. An incremental evaluation
// executes a few nodes; the preview must show ALL of them, including the ones
// served from cache. Projecting the run's touched nodes would make a graph's
// preview flicker down to whatever was last recomputed.
//
// Pure: no GPU, no ACAPI, no Win32. It names evp::preview only for the primitive
// struct, which is a plain aggregate.

#include "NodeGraph/Evaluator.hpp"
#include "NodeGraph/Graph.hpp"
#include "NodeGraph/NodeRegistry.hpp"
#include "Preview/GhPreviewCache.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace evp::nodegraph {

// The one node type this file looks for. Named here rather than in BuiltinNodes
// because the projection is the thing that gives it meaning: a preview node
// whose id nothing projects is an inert node with a nice icon.
extern const char* const kPreviewNodeType;

// The parameters that node carries. Spelled once, so the descriptor, the body
// and the projection cannot drift apart.
extern const char* const kPreviewEnabledParameter;
extern const char* const kPreviewColorParameter;
extern const char* const kPreviewXRayParameter;

// Where the node draws: "node", "archicad" or "both". Absent means both, which
// is what a Preview node placed before this parameter existed did.
extern const char* const kPreviewTargetParameter;

// True when this target includes the Archicad viewport. The client asks the same
// question of the other half; both spellings live here so they cannot drift.
bool PreviewTargetDrawsInArchicad (const std::string& target);

// The input the geometry arrives on.
//
// ⚠️ THE PROJECTION FOLLOWS THE EDGE INTO THIS PORT AND READS THE UPSTREAM
// NODE'S OUTPUT. Only outputs are cached, so the obvious shape was a Preview
// node that passed its input straight back out and a projection that read that -
// but it put two ports on a TERMINAL that nobody would ever wire, and made
// "List of 1" appear beside a node whose whole job is to show a shape. Walking
// the edge costs one lookup and leaves the node with the shape it should have.
extern const char* const kPreviewGeometryInput;

struct PreviewProjectionLimits {
    // A ceiling on primitives, not on triangles: the drawables builder already
    // has the vertex ceilings, and this one guards the map and the id hashing
    // that happen before it. A graph emitting a million points would otherwise
    // pay a shared_ptr allocation each.
    std::size_t maxPrimitives = 50000;

    // How deep into nested lists the walk goes. Same reason as
    // EvaluationLimits::maxValueDepth: a value from a node body is untrusted, and
    // this walk is recursive.
    std::size_t maxDepth = 64;
};

struct PreviewProjection {
    std::vector<std::shared_ptr<const evp::preview::GhPreviewPrimitive>> primitives;

    // How many preview nodes the document has, and how many of those were
    // switched on. Reported so a client can say "3 preview nodes, none enabled"
    // rather than showing an empty viewport that looks broken.
    std::size_t previewNodes = 0;
    std::size_t enabledNodes = 0;

    // Values of a type that has no geometry - a number, a string, an element
    // reference. COUNTED, not silently dropped: "I wired a number into Preview"
    // and "my geometry did not arrive" look identical in an empty viewport.
    std::size_t nonGeometricValues = 0;

    // True when maxPrimitives cut the walk short.
    bool truncated = false;
};

// What the projection needs from the evaluator: this node's last published
// result, or null. A function rather than the evaluator itself so that a test can
// answer it from a table and this file needs no cache.
using PreviewResultLookup = std::function<std::shared_ptr<const NodeResult> (const NodeId&)>;

PreviewProjection ProjectGraphPreview (const std::string& graphId, const GraphDocument& document,
                                       const NodeRegistry& registry, const PreviewResultLookup& lookup,
                                       const PreviewProjectionLimits& limits = {});

// "#RRGGBB" or "#RRGGBBAA" -> 0xRRGGBBAA. False when the text is not one of
// those, and the caller uses the viewport's ordinary preview colour instead: a
// mistyped colour should show the geometry, not hide it.
bool ParsePreviewColour (const std::string& text, uint32_t& rgba);

} // namespace evp::nodegraph

#endif
