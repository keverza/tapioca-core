#ifndef EVP_NATIVECOMMANDS_NODEGRAPHVALUEENCODING_HPP
#define EVP_NATIVECOMMANDS_NODEGRAPHVALUEENCODING_HPP

// The OUTBOUND wire encoding of a graph value, and of one published output.
//
// ⚠️ ONE ENCODER, FOR THE SAME REASON GraphValueTypeName IS SHARED. A second
// copy of this is a second contract: a client cannot tell which spelling of a
// mesh, or which list cap, is the truth. It lived in NodeGraphCommands.cpp until
// the branch view needed it too; it is here now so that no verb has to choose.
//
// Bounded by construction. A mesh crosses as its counts past a cap, a long list
// crosses as its size with `truncated` set, and a list MEMBER never expands - so
// the encoding is two levels deep and the response schemas need no recursive
// $ref. Model geometry does not cross this bridge at all; it reaches the preview
// hosts through RetainedPreviewStore.

#include "NativeCommands/NodeGraphCommandSupport.hpp"
#include "NodeGraph/NodeLifting.hpp"

namespace geomsrv {

// How many items of a list are spelled out before the encoding reports a count
// instead. A client that needs more asks the node for its preview.
constexpr size_t kMaxEncodedListItems = 256;

// How many BRANCHES of a tree are spelled out.
//
// A tree's branch count is its shape, and shape is small in every graph anyone
// can read: a hundred branches is already a diagram nobody follows. The item
// budget is what a big result actually costs, and that stays kMaxEncodedListItems
// spent across the branches rather than per branch.
constexpr size_t kMaxEncodedBranches = 128;

// How much of a mesh crosses the bridge.
//
// ⚠️ MESH DATA USED NOT TO CROSS AT ALL, AND THE NODE VIEWER DREW SUBSTITUTE
// BOXES - one per item - out of the count. That is the failure this cap exists
// to end: a viewer showing three green cubes for a sphere is not an abstraction,
// it is a picture of something the graph never produced, and a user comparing it
// against their model has no way to tell.
//
// So the vertices and the triangles cross, up to here. The cap is on the mesh
// rather than on the response because one node's result is what a viewer draws:
// past it the mesh is reported as truncated and the viewer says so instead of
// drawing half a solid. 20000 vertices is a couple of hundred kilobytes of JSON
// and comfortably more than any primitive in this catalog produces.
constexpr size_t kMaxEncodedMeshVertices = 20000;

// `expand` is false for list members, which is what keeps the encoding two
// levels deep and the response schemas non-recursive.
GS::ObjectState EncodeValue (const graph::Value& value, bool expand);

// One published output port, flat value AND branch structure.
//
// ⚠️ BOTH VIEWS, BECAUSE ONE OF THEM CANNOT ANSWER THE QUESTION. `value` is the
// tree flattened in canonical order, which is what most clients want and what
// every client used to get. `branches` is the shape: a flat twelve walls and
// four walls on each of three storeys are the same `value` and different trees,
// and telling them apart is the entire purpose of the tree layer (HANDOFF §7.3).
GS::ObjectState EncodeProjectedOutput (const std::string& portId, const graph::data::TreeValue& tree);

} // namespace geomsrv

#endif
