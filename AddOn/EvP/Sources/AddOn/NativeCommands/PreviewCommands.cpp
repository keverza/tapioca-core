#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/PreviewCommands.hpp"

#include "NativeCommands/CommandRegistration.hpp"
#include "NativeCommands/SchemaValidator.hpp"
#include "Preview/PreviewRuntimeState.hpp"
#include "Preview/RetainedPreviewStore.hpp"

#include "ObjectStateJSONConversion.hpp"

#include <cmath>
#include <string>
#include <utility>

namespace geomsrv {
namespace {

namespace retained = evp::preview;

constexpr std::size_t kMaxWatchNodes = 64;
constexpr std::size_t kMaxFramesPerNode = 512;
constexpr std::size_t kMaxWatchPoints = 20000;
constexpr std::size_t kMaxPreviewMeshes = 64;
constexpr std::size_t kMaxPreviewTriangles = 20000;
constexpr std::size_t kMaxPreviewVertices = 30000;
constexpr std::size_t kMaxPreviewLinePoints = 20000;

constexpr const char kPreviewInputSchema[] =
    R"json({"type":"object","properties":{"kind":{"type":"string","enum":["3d","plan2d"]},"meshes":{"type":"array","maxItems":64,"items":{"type":"string"}},"lines":{"type":"array","items":{"type":"string"}},"notes":{"type":"array","items":{"type":"string"}},"boundsMin":{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number"}},"boundsMax":{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number"}}},"additionalProperties":false,"required":["kind","meshes","lines","notes"]})json";
constexpr const char kPreviewResponseSchema[] =
    R"json({"type":"object","properties":{"generation":{"type":"integer","minimum":1},"meshes":{"type":"integer","minimum":0},"lines":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["generation","meshes","lines"]})json";
constexpr const char kWatchInputSchema[] =
    R"json({"type":"object","properties":{"version":{"type":"integer","const":1},"nodes":{"type":"array","maxItems":64,"items":{"type":"string"}}},"additionalProperties":false,"required":["version","nodes"]})json";
constexpr const char kWatchResponseSchema[] =
    R"json({"type":"object","properties":{"generation":{"type":"integer","minimum":1},"nodes":{"type":"integer","minimum":0},"frames":{"type":"integer","minimum":0},"points":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["generation","nodes","frames","points"]})json";

constexpr const char kMeshSchema[] =
    R"json({"type":"object","properties":{"role":{"type":"string","enum":["add","remove","modify","context","guide"]},"label":{"type":"string"},"vertices":{"type":"array","minItems":3,"maxItems":90000,"items":{"type":"number"}},"normals":{"type":"array","minItems":3,"maxItems":90000,"items":{"type":"number"}},"triangles":{"type":"array","minItems":3,"maxItems":60000,"items":{"type":"integer","minimum":0}}},"additionalProperties":false,"required":["role","label","vertices","normals","triangles"]})json";
constexpr const char kLineSchema[] =
    R"json({"type":"object","properties":{"role":{"type":"string","enum":["add","remove","modify","context","guide"]},"label":{"type":"string"},"closed":{"type":"boolean"},"points":{"type":"array","minItems":6,"maxItems":60000,"items":{"type":"number"}}},"additionalProperties":false,"required":["role","label","closed","points"]})json";
constexpr const char kNodeSchema[] =
    R"json({"type":"object","properties":{"name":{"type":"string","minLength":1},"frames":{"type":"array","maxItems":512,"items":{"type":"string"}}},"additionalProperties":false,"required":["name","frames"]})json";
constexpr const char kFrameSchema[] =
    R"json({"type":"object","properties":{"index":{"type":"integer","minimum":0},"primitives":{"type":"array","items":{"type":"object"}}},"additionalProperties":false,"required":["index","primitives"]})json";
constexpr const char kPointSchema[] =
    R"json({"type":"object","properties":{"kind":{"type":"string","const":"point"},"points":{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number"}},"text":{"type":"string"},"role":{"type":"string"},"closed":{"type":"boolean"},"direction":{"type":"boolean"},"offset":{"type":"number"}},"additionalProperties":false,"required":["kind","points"]})json";
constexpr const char kPolylineSchema[] =
    R"json({"type":"object","properties":{"kind":{"type":"string","const":"polyline"},"points":{"type":"array","minItems":6,"maxItems":60000,"items":{"type":"number"}},"text":{"type":"string"},"role":{"type":"string"},"closed":{"type":"boolean"},"direction":{"type":"boolean"},"offset":{"type":"number"}},"additionalProperties":false,"required":["kind","points"]})json";
constexpr const char kArrowSchema[] =
    R"json({"type":"object","properties":{"kind":{"type":"string","const":"arrow"},"points":{"type":"array","minItems":6,"maxItems":6,"items":{"type":"number"}},"text":{"type":"string"},"role":{"type":"string"},"closed":{"type":"boolean"},"direction":{"type":"boolean"},"offset":{"type":"number"}},"additionalProperties":false,"required":["kind","points"]})json";
constexpr const char kDimensionSchema[] =
    R"json({"type":"object","properties":{"kind":{"type":"string","const":"dimension"},"points":{"type":"array","minItems":6,"maxItems":6,"items":{"type":"number"}},"text":{"type":"string"},"role":{"type":"string"},"closed":{"type":"boolean"},"direction":{"type":"boolean"},"offset":{"type":"number"}},"additionalProperties":false,"required":["kind","points"]})json";
constexpr const char kAngleSchema[] =
    R"json({"type":"object","properties":{"kind":{"type":"string","const":"angle"},"points":{"type":"array","minItems":9,"maxItems":9,"items":{"type":"number"}},"text":{"type":"string"},"role":{"type":"string"},"closed":{"type":"boolean"},"direction":{"type":"boolean"},"offset":{"type":"number"}},"additionalProperties":false,"required":["kind","points"]})json";
constexpr const char kLabelSchema[] =
    R"json({"type":"object","properties":{"kind":{"type":"string","const":"label"},"points":{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number"}},"text":{"type":"string"},"role":{"type":"string"},"closed":{"type":"boolean"},"direction":{"type":"boolean"},"offset":{"type":"number"}},"additionalProperties":false,"required":["kind","points"]})json";
constexpr const char kElementSchema[] =
    R"json({"type":"object","properties":{"kind":{"type":"string","const":"element"},"guid":{"type":"string","minLength":1},"text":{"type":"string"},"role":{"type":"string"},"closed":{"type":"boolean"},"direction":{"type":"boolean"},"offset":{"type":"number"}},"additionalProperties":false,"required":["kind","guid"]})json";

std::string Utf8 (const GS::UniString& value)
{
    return value.ToCStr (0, GS::MaxUSize, CC_UTF8).Get ();
}

bool ValidateDecoded (const GS::ObjectState& value, const char* schema, const GS::UniString& context,
                      GS::UniString& error)
{
    GS::UniString detail;
    if (ValidateObjectStateSchema (value, GS::UniString (schema), detail))
        return true;
    error = context + " is invalid: " + detail;
    return false;
}

bool DecodeObject (const GS::UniString& json, const GS::UniString& context, GS::ObjectState& value,
                   GS::UniString& error)
{
    if (JSON::ConvertToObjectState (json, value) == NoError)
        return true;
    error = context + " is malformed JSON";
    return false;
}

bool ReadFiniteValues (const GS::ObjectState& value, const char* key, std::vector<double>& out,
                       const GS::UniString& context, GS::UniString& error)
{
    GS::Array<double> values;
    if (!value.Get (key, values)) {
        error = context + "." + key + " must be an array of numbers";
        return false;
    }
    out.reserve (values.GetSize ());
    for (double component : values) {
        if (!std::isfinite (component)) {
            error = context + "." + key + " contains a non-finite number";
            return false;
        }
        out.push_back (component);
    }
    return true;
}

bool DecodePrimitive (const GS::ObjectState& value, const GS::UniString& context, retained::WatchPrimitive& out,
                      std::size_t& pointCount, GS::UniString& error)
{
    GS::UniString kind;
    if (!value.Get ("kind", kind)) {
        error = context + ".kind is required";
        return false;
    }

    const char* schema = nullptr;
    if (kind == "point")
        schema = kPointSchema;
    else if (kind == "polyline")
        schema = kPolylineSchema;
    else if (kind == "arrow")
        schema = kArrowSchema;
    else if (kind == "dimension")
        schema = kDimensionSchema;
    else if (kind == "angle")
        schema = kAngleSchema;
    else if (kind == "label")
        schema = kLabelSchema;
    else if (kind == "element")
        schema = kElementSchema;
    else {
        error = context + ".kind has unknown primitive kind '" + kind + "'";
        return false;
    }
    if (!ValidateDecoded (value, schema, context, error))
        return false;

    GS::UniString stringValue;
    if (kind == "point") {
        out.kind = retained::WatchPrimitiveKind::Point;
    }
    else if (kind == "polyline") {
        out.kind = retained::WatchPrimitiveKind::Polyline;
    }
    else if (kind == "arrow") {
        out.kind = retained::WatchPrimitiveKind::Arrow;
    }
    else if (kind == "dimension") {
        out.kind = retained::WatchPrimitiveKind::Dimension;
    }
    else if (kind == "angle") {
        out.kind = retained::WatchPrimitiveKind::Angle;
    }
    else if (kind == "label") {
        out.kind = retained::WatchPrimitiveKind::Label;
    }
    else {
        out.kind = retained::WatchPrimitiveKind::Element;
        value.Get ("guid", stringValue);
        out.guid = Utf8 (stringValue);
    }

    if (kind != "element") {
        if (!ReadFiniteValues (value, "points", out.points, context, error) || out.points.size () % 3 != 0)
            return false;
        pointCount += out.points.size () / 3;
    }
    if (value.Get ("text", stringValue))
        out.text = Utf8 (stringValue);
    if (value.Get ("role", stringValue))
        out.role = Utf8 (stringValue);
    bool boolValue = false;
    if (value.Get ("closed", boolValue))
        out.closed = boolValue;
    if (value.Get ("direction", boolValue))
        out.direction = boolValue;
    double offset = 0.0;
    if (value.Get ("offset", offset))
        out.offset = offset;
    return true;
}

bool DecodePreview (const GS::ObjectState& params, retained::PreviewScene& scene, GS::UniString& error)
{
    GS::UniString kind;
    GS::Array<GS::UniString> meshJsons, lineJsons, notes;
    params.Get ("kind", kind);
    params.Get ("meshes", meshJsons);
    params.Get ("lines", lineJsons);
    params.Get ("notes", notes);
    scene.kind = Utf8 (kind);
    for (const GS::UniString& note : notes)
        scene.notes.push_back (Utf8 (note));

    std::size_t triangleCount = 0, vertexCount = 0;
    for (UIndex index = 0; index < meshJsons.GetSize (); ++index) {
        const GS::UniString context = GS::UniString::Printf ("meshes[%u]", (unsigned) index);
        GS::ObjectState value;
        if (!DecodeObject (meshJsons[index], context, value, error) ||
            !ValidateDecoded (value, kMeshSchema, context, error))
            return false;
        retained::PreviewMesh mesh;
        GS::UniString role, label;
        value.Get ("role", role);
        value.Get ("label", label);
        mesh.role = Utf8 (role);
        mesh.label = Utf8 (label);
        if (!ReadFiniteValues (value, "vertices", mesh.vertices, context, error) ||
            !ReadFiniteValues (value, "normals", mesh.normals, context, error))
            return false;
        GS::Array<GS::Int32> triangles;
        value.Get ("triangles", triangles);
        if (mesh.vertices.size () % 3 != 0 || mesh.normals.size () != mesh.vertices.size () ||
            triangles.GetSize () % 3 != 0) {
            error = context + " has inconsistent vertices, normals, or triangles";
            return false;
        }
        const std::size_t vertices = mesh.vertices.size () / 3;
        for (GS::Int32 triangleIndex : triangles) {
            if (triangleIndex < 0 || (USize) triangleIndex >= vertices) {
                error = context + ".triangles contains an out-of-range vertex index";
                return false;
            }
            mesh.triangles.push_back ((uint32_t) triangleIndex);
        }
        vertexCount += vertices;
        triangleCount += triangles.GetSize () / 3;
        if (vertexCount > kMaxPreviewVertices || triangleCount > kMaxPreviewTriangles ||
            scene.meshes.size () >= kMaxPreviewMeshes) {
            error = "preview scene exceeds the mesh, vertex, or triangle budget";
            return false;
        }
        scene.meshes.push_back (std::move (mesh));
    }

    std::size_t linePointCount = 0;
    for (UIndex index = 0; index < lineJsons.GetSize (); ++index) {
        const GS::UniString context = GS::UniString::Printf ("lines[%u]", (unsigned) index);
        GS::ObjectState value;
        if (!DecodeObject (lineJsons[index], context, value, error) ||
            !ValidateDecoded (value, kLineSchema, context, error))
            return false;
        retained::PreviewLine line;
        GS::UniString role, label;
        value.Get ("role", role);
        value.Get ("label", label);
        value.Get ("closed", line.closed);
        line.role = Utf8 (role);
        line.label = Utf8 (label);
        if (!ReadFiniteValues (value, "points", line.points, context, error) || line.points.size () % 3 != 0)
            return false;
        linePointCount += line.points.size () / 3;
        if (linePointCount > kMaxPreviewLinePoints) {
            error = "preview scene exceeds the 20000 polyline-point budget";
            return false;
        }
        scene.lines.push_back (std::move (line));
    }

    GS::Array<double> boundsMin, boundsMax;
    const bool hasMin = params.Get ("boundsMin", boundsMin), hasMax = params.Get ("boundsMax", boundsMax);
    if (hasMin != hasMax) {
        error = "boundsMin and boundsMax must be provided together";
        return false;
    }
    if (hasMin) {
        scene.hasBounds = true;
        for (UIndex axis = 0; axis < 3; ++axis) {
            if (!std::isfinite (boundsMin[axis]) || !std::isfinite (boundsMax[axis]) ||
                boundsMin[axis] > boundsMax[axis]) {
                error = "preview bounds must be finite and ordered";
                return false;
            }
            scene.boundsMin[axis] = boundsMin[axis];
            scene.boundsMax[axis] = boundsMax[axis];
        }
    }
    return true;
}

bool DecodeWatchTrace (const GS::ObjectState& params, retained::WatchTrace& trace, std::size_t& frameCount,
                       std::size_t& pointCount, GS::UniString& error)
{
    GS::Int32 version = 0;
    GS::Array<GS::UniString> nodeJsons;
    params.Get ("version", version);
    params.Get ("nodes", nodeJsons);
    trace.version = (uint32_t) version;
    if (nodeJsons.GetSize () > kMaxWatchNodes) {
        error = "watch trace exceeds the 64-node budget";
        return false;
    }
    for (UIndex nodeIndex = 0; nodeIndex < nodeJsons.GetSize (); ++nodeIndex) {
        const GS::UniString nodeContext = GS::UniString::Printf ("nodes[%u]", (unsigned) nodeIndex);
        GS::ObjectState nodeValue;
        if (!DecodeObject (nodeJsons[nodeIndex], nodeContext, nodeValue, error) ||
            !ValidateDecoded (nodeValue, kNodeSchema, nodeContext, error))
            return false;
        retained::WatchNode node;
        GS::UniString name;
        GS::Array<GS::UniString> frameJsons;
        nodeValue.Get ("name", name);
        nodeValue.Get ("frames", frameJsons);
        node.name = Utf8 (name);
        if (frameJsons.GetSize () > kMaxFramesPerNode) {
            error = nodeContext + " exceeds the 512-frame budget";
            return false;
        }
        for (UIndex frameIndex = 0; frameIndex < frameJsons.GetSize (); ++frameIndex) {
            const GS::UniString frameContext =
                nodeContext + GS::UniString::Printf (".frames[%u]", (unsigned) frameIndex);
            GS::ObjectState frameValue;
            if (!DecodeObject (frameJsons[frameIndex], frameContext, frameValue, error) ||
                !ValidateDecoded (frameValue, kFrameSchema, frameContext, error))
                return false;
            retained::WatchFrame frame;
            GS::Int32 index = 0;
            GS::Array<GS::ObjectState> primitives;
            frameValue.Get ("index", index);
            frameValue.Get ("primitives", primitives);
            frame.index = (uint32_t) index;
            for (UIndex primitiveIndex = 0; primitiveIndex < primitives.GetSize (); ++primitiveIndex) {
                retained::WatchPrimitive primitive;
                if (!DecodePrimitive (primitives[primitiveIndex],
                                      frameContext +
                                          GS::UniString::Printf (".primitives[%u]", (unsigned) primitiveIndex),
                                      primitive, pointCount, error))
                    return false;
                if (pointCount > kMaxWatchPoints) {
                    error = "watch trace exceeds the 20000-point budget";
                    return false;
                }
                frame.primitives.push_back (std::move (primitive));
            }
            node.frames.push_back (std::move (frame));
            ++frameCount;
        }
        trace.nodes.push_back (std::move (node));
    }
    return true;
}

class SetPreviewSceneCommand : public MainThreadCommand {
  public:
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        if (!evp::preview::PreviewRuntimeState::Get ().IsEnabled ())
            return NativeCommandResult::Failure ("previews are disabled");
        retained::PreviewScene scene;
        GS::UniString error;
        if (!DecodePreview (params, scene, error))
            return NativeCommandResult::Failure (error);
        const GS::Int32 meshes = (GS::Int32) scene.meshes.size (), lines = (GS::Int32) scene.lines.size ();
        const uint64_t generation = retained::RetainedPreviewStore::Get ().PublishPreviewScene (std::move (scene));
        GS::ObjectState response;
        response.Add ("generation", (GS::Int64) generation);
        response.Add ("meshes", meshes);
        response.Add ("lines", lines);
        return response;
    }
};

class SetWatchTraceCommand : public MainThreadCommand {
  public:
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        if (!evp::preview::PreviewRuntimeState::Get ().IsEnabled ())
            return NativeCommandResult::Failure ("previews are disabled");
        retained::WatchTrace trace;
        std::size_t frames = 0, points = 0;
        GS::UniString error;
        if (!DecodeWatchTrace (params, trace, frames, points, error))
            return NativeCommandResult::Failure (error);
        const GS::Int32 nodes = (GS::Int32) trace.nodes.size ();
        const uint64_t generation = retained::RetainedPreviewStore::Get ().PublishWatchTrace (std::move (trace));
        GS::ObjectState response;
        response.Add ("generation", (GS::Int64) generation);
        response.Add ("nodes", nodes);
        response.Add ("frames", (GS::Int32) frames);
        response.Add ("points", (GS::Int32) points);
        return response;
    }
};

const NativeCommandRegistration registrations[] = {
    { "SetPreviewScene", &MakeRegisteredNativeCommand<SetPreviewSceneCommand>, false, kPreviewInputSchema,
      kPreviewResponseSchema },
    { "SetWatchTrace", &MakeRegisteredNativeCommand<SetWatchTraceCommand>, false, kWatchInputSchema,
      kWatchResponseSchema }
};

} // namespace

NativeCommandRegistrations GetPreviewCommandRegistrations ()
{
    return MakeRegistrationView (registrations);
}

} // namespace geomsrv
