#include "NodeGraph/PreviewProjection.hpp"

#include "Preview/GraphPreviewStore.hpp"

#include <cctype>
#include <utility>

namespace evp::nodegraph {

using evp::preview::GhPreviewPrimitive;
using evp::preview::PreviewKind;
using evp::preview::PreviewSurface;

const char* const kPreviewNodeType = "preview";
const char* const kPreviewEnabledParameter = "enabled";
const char* const kPreviewColorParameter = "color";
const char* const kPreviewXRayParameter = "xray";
const char* const kPreviewTargetParameter = "target";
const char* const kPreviewGeometryInput = "geometry";

namespace {

bool BoolParameter (const Node& node, const char* id, bool fallback)
{
    const auto found = node.parameters.find (id);
    if (found == node.parameters.end () || found->second.Type () != ValueType::Bool)
        return fallback;
    return std::get<bool> (found->second.DataValue ());
}

std::string TextParameter (const Node& node, const char* id)
{
    const auto found = node.parameters.find (id);
    if (found == node.parameters.end () || found->second.Type () != ValueType::String)
        return {};
    return std::get<std::string> (found->second.DataValue ());
}

void PushPoint (std::vector<float>& positions, const Point3& point)
{
    positions.push_back (static_cast<float> (point.x));
    positions.push_back (static_cast<float> (point.y));
    positions.push_back (static_cast<float> (point.z));
}

// One node's worth of state, so the recursive walk carries four things rather
// than six arguments.
struct Walk {
    const std::string* graphId = nullptr;
    const NodeId* nodeId = nullptr;
    bool hasColour = false;
    uint32_t rgba = 0;
    uint8_t flags = 0;
    uint32_t item = 0;
    PreviewProjection* out = nullptr;
    const PreviewProjectionLimits* limits = nullptr;
};

// Fills in everything every primitive from one preview node shares.
std::shared_ptr<GhPreviewPrimitive> NewPrimitive (Walk& walk, PreviewKind kind)
{
    auto primitive = std::make_shared<GhPreviewPrimitive> ();
    primitive->id = evp::preview::GraphPreviewPrimitiveId (*walk.graphId, *walk.nodeId, walk.item++);
    primitive->kind = kind;
    primitive->flags = walk.flags;
    // Model3D, not Both. The plan overlay does not read preview yet, and a
    // primitive claiming a surface nothing draws would be an unkept promise
    // rather than a feature - see PreviewSurface's note on guessing.
    primitive->surface = PreviewSurface::Model3D;
    primitive->hasOwnColour = walk.hasColour;
    primitive->rgba = walk.rgba;
    return primitive;
}

// The one recursive step. Returns false when a ceiling stopped it, so the caller
// can stop walking the rest rather than filling `truncated` a million times.
bool AppendValue (const Value& value, std::size_t depth, Walk& walk)
{
    if (walk.out->primitives.size () >= walk.limits->maxPrimitives) {
        walk.out->truncated = true;
        return false;
    }
    if (depth > walk.limits->maxDepth) {
        walk.out->truncated = true;
        return false;
    }

    switch (value.Type ()) {
        case ValueType::Point3: {
            auto primitive = NewPrimitive (walk, PreviewKind::PointMarker);
            PushPoint (primitive->positions, std::get<Point3> (value.DataValue ()));
            walk.out->primitives.push_back (std::move (primitive));
            return true;
        }

        case ValueType::Polyline: {
            const Polyline& polyline = std::get<Polyline> (value.DataValue ());
            if (polyline.points.size () < 2)
                return true; // a one-point polyline is not a drawing failure
            auto primitive = NewPrimitive (walk, PreviewKind::Polyline3D);
            for (const Point3& point : polyline.points)
                PushPoint (primitive->positions, point);
            walk.out->primitives.push_back (std::move (primitive));
            return true;
        }

        case ValueType::Polygon: {
            const Polygon& polygon = std::get<Polygon> (value.DataValue ());
            if (polygon.points.size () < 3)
                return true;
            auto primitive = NewPrimitive (walk, PreviewKind::Polyline3D);
            // ⚠️ CLOSED, AND SAID SO RATHER THAN REPEATING THE FIRST POINT. The
            // drawables builder draws the closing segment from this flag; a
            // duplicated last point would draw a zero-length segment as well,
            // which is a degenerate quad in the ribbon and shows up as a speck.
            primitive->closed = true;
            for (const Point3& point : polygon.points)
                PushPoint (primitive->positions, point);
            walk.out->primitives.push_back (std::move (primitive));
            return true;
        }

        case ValueType::Mesh: {
            const auto& mesh = std::get<Value::ImmutableMesh> (value.DataValue ());
            if (mesh == nullptr || mesh->triangles.empty ())
                return true;
            auto primitive = NewPrimitive (walk, PreviewKind::TriangleMesh);
            primitive->positions.reserve (mesh->vertices.size ());
            for (const double coordinate : mesh->vertices)
                primitive->positions.push_back (static_cast<float> (coordinate));
            // ⚠️ NORMALS ARE COPIED ONLY WHEN THERE IS ONE PER VERTEX. A short
            // normal array is worse than none: the mesh appender indexes it
            // positionally, so a mismatched length reads whatever follows it.
            if (mesh->normals.size () == mesh->vertices.size ())
                primitive->normals = mesh->normals;
            primitive->indices = mesh->triangles;
            walk.out->primitives.push_back (std::move (primitive));
            return true;
        }

        case ValueType::List: {
            for (const Value& item : std::get<Value::List> (value.DataValue ())) {
                if (!AppendValue (item, depth + 1, walk))
                    return false;
            }
            return true;
        }

        case ValueType::Absent:
            // Nothing was wired in. Not a miswiring, so not counted as one.
            return true;

        default:
            // A number, a bool, a string, an element reference. Counted; see the
            // field's note.
            ++walk.out->nonGeometricValues;
            return true;
    }
}

} // namespace

bool PreviewTargetDrawsInArchicad (const std::string& target)
{
    // ⚠️ AN UNRECOGNISED TARGET DRAWS. A graph saved by a later build naming a
    // target this one does not know should show its geometry rather than hide
    // it: a preview that silently stops appearing is indistinguishable from a
    // definition that stopped producing anything, and the user would debug the
    // wrong half. Only the one spelling that means "not in Archicad" suppresses.
    return target != "node";
}

bool ParsePreviewColour (const std::string& text, uint32_t& rgba)
{
    if (text.size () != 7 && text.size () != 9)
        return false;
    if (text[0] != '#')
        return false;

    uint32_t value = 0;
    for (std::size_t index = 1; index < text.size (); ++index) {
        const unsigned char character = static_cast<unsigned char> (text[index]);
        int digit = 0;
        if (character >= '0' && character <= '9')
            digit = character - '0';
        else if (character >= 'a' && character <= 'f')
            digit = character - 'a' + 10;
        else if (character >= 'A' && character <= 'F')
            digit = character - 'A' + 10;
        else
            return false;
        value = (value << 4) | static_cast<uint32_t> (digit);
    }

    // "#RRGGBB" is opaque. Alpha is accepted because the struct carries it, not
    // because anything asks for a translucent preview yet.
    rgba = text.size () == 7 ? ((value << 8) | 0xFFu) : value;
    return true;
}

PreviewProjection ProjectGraphPreview (const std::string& graphId, const GraphDocument& document,
                                       const NodeRegistry& registry, const PreviewResultLookup& lookup,
                                       const PreviewProjectionLimits& limits)
{
    PreviewProjection projection;

    for (const auto& entry : document.Nodes ()) {
        const Node& node = entry.second;
        if (node.nodeType != kPreviewNodeType)
            continue;
        ++projection.previewNodes;

        // ⚠️ THE NODE'S EXECUTION MODE OUTRANKS ITS OWN SWITCH. A node the user
        // disabled on the canvas did not run, so its cached result belongs to a
        // graph that no longer exists; drawing it would show geometry the graph
        // is not producing. Bypassed is the same story with a different cause.
        if (node.executionMode != ExecutionMode::Enabled)
            continue;
        if (!BoolParameter (node, kPreviewEnabledParameter, true))
            continue;
        // Counted as a preview node either way - it IS previewing, just not
        // here. Only the Archicad half is this function's business.
        if (!PreviewTargetDrawsInArchicad (TextParameter (node, kPreviewTargetParameter)))
            continue;

        // ⚠️ THE NODE HAS TO HAVE RUN, even though it produces nothing. Its own
        // result is what says it was REACHED by the last evaluation; a preview
        // downstream of a disabled or failed branch would otherwise keep drawing
        // the geometry from before the branch was cut.
        if (lookup (node.id) == nullptr)
            continue;

        // Follow the wire. The geometry is the upstream node's output, and the
        // internalised parameter is the fallback for a port with nothing wired -
        // the same precedence the evaluator itself applies.
        const Value* geometry = nullptr;
        std::shared_ptr<const NodeResult> upstream;
        for (const Edge& edge : document.Edges ()) {
            if (edge.targetNode != node.id || edge.targetPort != kPreviewGeometryInput)
                continue;
            upstream = lookup (edge.sourceNode);
            if (upstream == nullptr)
                break;
            const auto output = upstream->outputs.find (edge.sourcePort);
            if (output != upstream->outputs.end ())
                geometry = &output->second;
            break;
        }
        if (geometry == nullptr) {
            const auto stored = node.parameters.find (kPreviewGeometryInput);
            if (stored != node.parameters.end ())
                geometry = &stored->second;
        }
        if (geometry == nullptr)
            continue; // nothing is wired in, and nothing was typed in

        ++projection.enabledNodes;

        Walk walk;
        walk.graphId = &graphId;
        walk.nodeId = &node.id;
        walk.out = &projection;
        walk.limits = &limits;
        walk.hasColour = ParsePreviewColour (TextParameter (node, kPreviewColorParameter), walk.rgba);
        // Visible, and nothing else asserted. Selected and highlighted are
        // Grasshopper's canvas states and mean nothing here; DepthTest is not
        // read by the builder, which decides depth from the x-ray flag alone,
        // and setting a flag nothing reads is how the two drift apart.
        walk.flags = evp::grasshopper::protocol::PreviewFlagVisible;
        if (BoolParameter (node, kPreviewXRayParameter, false))
            walk.flags |= evp::grasshopper::protocol::PreviewFlagXRay;

        if (!AppendValue (*geometry, 0, walk))
            break; // a ceiling was hit; the rest would only repeat it
    }

    (void) registry; // the node type is enough; kept for a future per-type rule
    return projection;
}

} // namespace evp::nodegraph
