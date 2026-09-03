#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/NodeGraphValueEncoding.hpp"

#include "NodeGraph/ValueText.hpp"

#include <string>

namespace geomsrv {
namespace {

void AddPoints (GS::ObjectState& state, const std::vector<graph::Point3>& points)
{
    GS::Array<double> numbers;
    for (const graph::Point3& point : points) {
        numbers.Push (point.x);
        numbers.Push (point.y);
        numbers.Push (point.z);
    }
    state.Add ("numbers", numbers);
    state.Add ("itemCount", static_cast<GS::Int64> (points.size ()));
}

} // namespace

GS::ObjectState EncodeValue (const graph::Value& value, bool expand)
{
    GS::ObjectState state;
    state.Add ("valueType", GraphValueTypeName (value.Type ()));
    switch (value.Type ()) {
        case graph::ValueType::Absent:
            break;
        case graph::ValueType::Bool:
            state.Add ("bool", std::get<bool> (value.DataValue ()));
            break;
        case graph::ValueType::Integer:
            state.Add ("number", static_cast<double> (std::get<int64_t> (value.DataValue ())));
            break;
        case graph::ValueType::Double:
            state.Add ("number", std::get<double> (value.DataValue ()));
            break;
        case graph::ValueType::String:
            state.Add ("text", GraphText (std::get<std::string> (value.DataValue ())));
            break;
        case graph::ValueType::Point3: {
            const graph::Point3& point = std::get<graph::Point3> (value.DataValue ());
            GS::Array<double> numbers;
            numbers.Push (point.x);
            numbers.Push (point.y);
            numbers.Push (point.z);
            state.Add ("numbers", numbers);
            break;
        }
        case graph::ValueType::Polyline:
            AddPoints (state, std::get<graph::Polyline> (value.DataValue ()).points);
            break;
        case graph::ValueType::Polygon:
            AddPoints (state, std::get<graph::Polygon> (value.DataValue ()).points);
            break;
        case graph::ValueType::Mesh: {
            const auto& mesh = std::get<graph::Value::ImmutableMesh> (value.DataValue ());
            const size_t vertexCount = mesh ? mesh->VertexCount () : 0;
            state.Add ("itemCount", static_cast<GS::Int64> (vertexCount));
            // A list MEMBER stays a count, as every other type does there: a list
            // of two hundred meshes is a summary, not a scene.
            if (!expand || mesh == nullptr || vertexCount == 0 || vertexCount > kMaxEncodedMeshVertices) {
                state.Add ("truncated", true);
                break;
            }
            GS::Array<double> numbers;
            numbers.SetCapacity (static_cast<USize> (mesh->vertices.size ()));
            for (const double coordinate : mesh->vertices)
                numbers.Push (coordinate);
            state.Add ("numbers", numbers);
            // ⚠️ NORMALS ARE NOT SENT. The viewer computes flat face normals from
            // the triangles it is given, which is what it draws anyway; sending
            // them would double the payload to reproduce something derivable, and
            // a normal array that disagreed with the positions after a truncation
            // would shade the mesh wrongly with nothing to say why.
            GS::Array<GS::Int32> indices;
            indices.SetCapacity (static_cast<USize> (mesh->triangles.size ()));
            for (const uint32_t index : mesh->triangles)
                indices.Push (static_cast<GS::Int32> (index));
            state.Add ("indices", indices);
            break;
        }
        case graph::ValueType::ArchicadElementRef:
            state.Add ("text", GraphText (std::get<graph::ArchicadElementRef> (value.DataValue ()).guid));
            break;
        case graph::ValueType::List: {
            const graph::Value::List& list = std::get<graph::Value::List> (value.DataValue ());
            state.Add ("itemCount", static_cast<GS::Int64> (list.size ()));
            if (!expand || list.size () > kMaxEncodedListItems) {
                state.Add ("truncated", true);
                break;
            }
            GS::Array<GS::ObjectState> items;
            for (const graph::Value& item : list)
                items.Push (EncodeValue (item, false));
            state.Add ("items", items);
            break;
        }
    }
    return state;
}

GS::ObjectState EncodeProjectedOutput (const std::string& portId, const graph::data::TreeValue& tree)
{
    // Bounded by the runtime, not by this command: see ProjectOutput.
    const graph::ProjectedOutput projected = graph::ProjectOutput (tree, kMaxEncodedListItems, kMaxEncodedBranches);

    GS::ObjectState output;
    output.Add ("portId", GraphText (portId));
    output.Add ("value", EncodeValue (projected.value, true));
    // The same renderer the Panel node uses, on EVERY output, so a client can
    // show what a node produced without the user having to wire an inspector to
    // it first.
    output.Add ("text", GraphText (graph::FormatValue (projected.value)));
    output.Add ("summary", GraphText (graph::DescribeValue (projected.value)));

    // The declared item type of the tree, which is NOT derivable from the
    // projected value: an empty tree of meshes and an empty tree of numbers both
    // project to an empty list, and a client showing port types needs them apart.
    output.Add ("itemType", GraphText (graph::data::ItemTypeName (projected.itemType)));
    output.Add ("branchCount", static_cast<GS::Int64> (projected.branchCount));
    output.Add ("branchesTruncated", projected.branchesTruncated);

    GS::Array<GS::ObjectState> branches;
    for (const graph::ProjectedBranch& branch : projected.branches) {
        GS::ObjectState encoded;
        // Both spellings of the path: the text is what a person reads in a row
        // label, the segments are what a client sorts and groups by without
        // having to parse the text back.
        encoded.Add ("path", GraphText (branch.path.ToString ()));
        GS::Array<GS::Int32> segments;
        for (const graph::data::DataPath::Segment segment : branch.path.Segments ())
            segments.Push (static_cast<GS::Int32> (segment));
        encoded.Add ("segments", segments);
        encoded.Add ("itemCount", static_cast<GS::Int64> (branch.itemCount));
        encoded.Add ("truncated", branch.truncated);
        encoded.Add ("value", EncodeValue (branch.value, true));
        branches.Push (std::move (encoded));
    }
    output.Add ("branches", branches);
    return output;
}

} // namespace geomsrv
