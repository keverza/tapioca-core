#include "ArchViz/MassingCalculation.hpp"
#include "NodeGraph/Json.hpp"

#include <cmath>
#include <algorithm>

namespace geomsrv::archviz::massingcalculation {
namespace {
namespace js = evp::nodegraph::json;
using Value = js::JsonValue;
Value Numbers (const std::vector<double>& values)
{
    js::JsonArray out;
    for (double value : values)
        out.push_back (Value::Double (value));
    return Value::Array (std::move (out));
}
bool Number (const Value& object, const char* key, double& value)
{
    const auto* member = object.Find (key);
    return member != nullptr && member->AsDouble (value) && std::isfinite (value);
}
bool Buffer (const Value& value, std::vector<double>& out, size_t limit)
{
    const auto* array = value.AsArray ();
    if (array == nullptr || array->size () % 3 != 0 || array->size () > limit)
        return false;
    for (const auto& member : *array) {
        double number = 0;
        if (!member.AsDouble (number) || !std::isfinite (number))
            return false;
        out.push_back (number);
    }
    return true;
}
bool Fail (std::string& error, const char* message)
{
    error = message;
    return false;
}
} // namespace

bool SameRequest (const Request& a, const Request& b)
{
    if (a.action != b.action || a.before.guid != b.before.guid || a.before.known != b.before.known ||
        a.before.hasStored != b.before.hasStored || a.before.stored != b.before.stored ||
        a.before.edges.size () != b.before.edges.size () || a.assignments.size () != b.assignments.size () ||
        a.regulated != b.regulated || a.endpoints != b.endpoints || a.landscape != b.landscape ||
        a.baseHeight != b.baseHeight || a.runPerRise != b.runPerRise || a.capZ != b.capZ ||
        a.baseDepth != b.baseDepth || a.capped != b.capped || a.parcels.size () != b.parcels.size ())
        return false;
    for (size_t i = 0; i < a.before.edges.size (); ++i) {
        const auto& x = a.before.edges[i];
        const auto& y = b.before.edges[i];
        if (x.ax != y.ax || x.ay != y.ay || x.bx != y.bx || x.by != y.by || x.arcAngle != y.arcAngle)
            return false;
    }
    for (size_t i = 0; i < a.assignments.size (); ++i)
        if (a.assignments[i].mode != b.assignments[i].mode || a.assignments[i].distance != b.assignments[i].distance ||
            a.assignments[i].review != b.assignments[i].review)
            return false;
    if (!a.parcels.empty ()) {
        const auto left = Expand (a), right = Expand (b);
        for (size_t i = 0; i < left.size (); ++i)
            if (!SameRequest (left[i], right[i]))
                return false;
    }
    return true;
}

overlaylayers::Layer OffsetDimensions (const Preview& preview)
{
    overlaylayers::Layer layer;
    layer.name = kDimensions;
    layer.occlusion = overlaylayers::Behind::Fade;
    if (!preview.parcels.empty ()) {
        for (const auto& parcel : preview.parcels) {
            const auto part = OffsetDimensions ({ parcel.inputs, parcel.result });
            layer.dimensions.insert (layer.dimensions.end (), part.dimensions.begin (), part.dimensions.end ());
        }
        return layer;
    }
    const auto& edges = preview.inputs.before.edges;
    const auto& xy = preview.result.offsetXY;
    if (xy.size () < 6 || preview.inputs.assignments.size () != edges.size ())
        return layer;
    double winding = 0;
    for (const auto& edge : edges)
        winding += edge.ax * edge.by - edge.bx * edge.ay;
    for (size_t i = 0; i < edges.size (); ++i) {
        const auto& edge = edges[i];
        const double distance = preview.inputs.assignments[i].distance;
        const double dx = edge.bx - edge.ax, dy = edge.by - edge.ay, length = std::hypot (dx, dy);
        if (length < 1e-6 || distance <= 1e-6 || std::abs (edge.arcAngle) > 1e-8)
            continue;
        const double sign = winding > 0 ? 1 : -1;
        const double nx = -dy / length * sign, ny = dx / length * sign;
        const double x = (edge.ax + edge.bx) * 0.5, y = (edge.ay + edge.by) * 0.5;
        const double tx = x + nx * distance, ty = y + ny * distance;
        // Only dimension a surviving inset edge, not a collapsed/trimmed midpoint.
        bool onInset = false;
        for (size_t j = 0; j < xy.size (); j += 2) {
            const size_t k = (j + 2) % xy.size ();
            const double ex = xy[k] - xy[j], ey = xy[k + 1] - xy[j + 1];
            const double squared = ex * ex + ey * ey;
            if (squared < 1e-12)
                continue;
            const double t = (std::clamp) (((tx - xy[j]) * ex + (ty - xy[j + 1]) * ey) / squared, 0.0, 1.0);
            if (std::hypot (tx - xy[j] - t * ex, ty - xy[j + 1] - t * ey) < 1e-4)
                onInset = true;
        }
        if (!onInset)
            continue;
        overlaylayers::Dimension dimension;
        dimension.from[0] = x;
        dimension.from[1] = y;
        dimension.to[0] = tx;
        dimension.to[1] = ty;
        dimension.from[2] = dimension.to[2] = preview.result.hasMeanZ ? preview.result.meanZ : 0;
        dimension.offsetMetres = 0;
        dimension.rgba = 0xA66226FF;
        dimension.showUnit = true;
        dimension.textSizePixels = 32; // glyph resolution, not its model height
        dimension.textSizeMetres = 0.20;
        dimension.textMinProjectedPixels = 9;
        layer.dimensions.push_back (std::move (dimension));
    }
    return layer;
}

bool PreviewQueue::Follow (Request request, uint64_t now)
{
    if (desired && SameRequest (*desired, request))
        return false;
    desired = std::move (request);
    Refresh (now);
    return true;
}

void PreviewQueue::Refresh (uint64_t now)
{
    ++revision;
    pending = desired && desired->action == Action::Calculate;
    due = now + 150;
}

void PreviewQueue::Reset ()
{
    desired.reset ();
    ++revision;
    pending = false;
}

std::optional<Request> PreviewQueue::TakeReady (uint64_t now, bool busy)
{
    if (!pending || busy || now < due)
        return {};
    pending = false;
    return desired;
}

bool Encode (const Request& request, const geomsrv::Mesh& terrain, bool hasAltitude, double altitude, std::string& json,
             std::string& error)
{
    error.clear ();
    metadata::Property property;
    if (!request.parcels.empty ())
        return Fail (error, "Expand the site request before encoding each parcel.");
    if (!request.before.known)
        return Fail (error, "Define a readable property-line Polyline before calculating.");
    if (!massingrules::Encode (request.before.edges, request.assignments, property, error))
        return false;
    const size_t count = request.before.edges.size ();
    if (request.regulated.size () != count || request.endpoints.size () != count || request.landscape < 0 ||
        request.landscape > 1 || !std::isfinite (request.baseHeight) || request.baseHeight < 0 ||
        request.baseHeight > 1000 || !std::isfinite (request.runPerRise) || request.runPerRise < 0.01 ||
        request.runPerRise > 1000 || !std::isfinite (request.capZ) || (request.capped && request.capZ <= 0) ||
        !std::isfinite (request.baseDepth) || request.baseDepth < 0.01 || request.baseDepth > 100 ||
        (hasAltitude && !std::isfinite (altitude)))
        return Fail (error, "Invalid native massing calculation parameters.");
    const bool noTerrain = terrain.vertices.empty () && terrain.triangles.empty ();
    if (!noTerrain &&
        (terrain.vertices.empty () || terrain.vertices.size () % 3 || terrain.vertices.size () > 600000 ||
         terrain.triangles.empty () || terrain.triangles.size () % 3 || terrain.triangles.size () > 600000))
        return Fail (error, "Empty/invalid terrain or snapshot budget exceeded.");
    for (double value : terrain.vertices)
        if (!std::isfinite (value))
            return Fail (error, "Non-finite terrain snapshot.");
    js::JsonArray indices, edges;
    for (uint32_t index : terrain.triangles) {
        if (index >= terrain.VertexCount ())
            return Fail (error, "Invalid terrain triangle index.");
        indices.push_back (Value::Integer (index));
    }
    for (size_t i = 0; i < count; ++i) {
        const auto& edge = request.before.edges[i];
        edges.push_back (
            Value::Object ({ { "a", Numbers ({ edge.ax, edge.ay }) },
                             { "b", Numbers ({ edge.bx, edge.by }) },
                             { "arc", Value::Double (edge.arcAngle) },
                             { "distance", Value::Double (request.assignments[i].distance) },
                             { "vertical", Value::Bool (request.assignments[i].mode == massingrules::Mode::None) },
                             { "review", Value::Bool (request.assignments[i].review) },
                             { "regulated", Value::Bool (request.regulated[i]) },
                             { "reference", Value::Bool (request.endpoints[i]) } }));
    }
    const Value input = Value::Object (
        { { "version", Value::Integer (1) },
          { "edges", Value::Array (std::move (edges)) },
          { "terrain", noTerrain ? Value {}
                                 : Value::Object ({ { "vertices", Numbers (terrain.vertices) },
                                                    { "triangles", Value::Array (std::move (indices)) } }) },
          { "baseHeight", Value::Double (request.baseHeight) },
          { "runPerRise", Value::Double (request.runPerRise) },
          { "capZ", Value::Double (request.capZ) },
          { "capped", Value::Bool (request.capped) },
          { "baseDepth", Value::Double (request.baseDepth) },
          { "originAltitude", hasAltitude ? Value::Double (altitude) : Value {} } });
    std::string encoded = js::Write (Value::Object ({ { "request", input } }), 0);
    if (encoded.size () > 16 * 1024 * 1024)
        return Fail (error, "Terrain snapshot exceeds the calculation transport budget.");
    json = std::move (encoded);
    return true;
}

bool Decode (const std::string& bridgeJson, Result& result, std::string& error)
{
    error.clear ();
    if (bridgeJson.size () > 8 * 1024 * 1024)
        return Fail (error, "Python result exceeded the native geometry budget.");
    const auto parsed = js::Parse (bridgeJson);
    bool ok = false;
    const auto* success = parsed.value.Find ("ok");
    if (!parsed.ok || success == nullptr || !success->AsBool (ok) || !ok) {
        const auto* message = parsed.value.Find ("error");
        if (message != nullptr)
            message->AsString (error);
        if (error.empty ())
            error = "Python calculation failed or returned an invalid bridge response.";
        return false;
    }
    const auto* outputs = parsed.value.Find ("outputs");
    const auto* payload = outputs == nullptr ? nullptr : outputs->Find ("payload");
    double version = 0;
    if (payload == nullptr || !Number (*payload, "version", version) || version != 1)
        return Fail (error, "Unsupported Python massing result.");
    Result out;
    out.layer.name = "tapioca.massing.envelope";
    out.layer.occlusion = overlaylayers::Behind::Dash;
    out.site.name = "tapioca.massing.lines";
    out.site.occlusion = overlaylayers::Behind::Dash;
    const auto *hasEnvelope = payload->Find ("hasEnvelope"), *note = payload->Find ("note");
    const auto* outline = payload->Find ("offsetXY");
    const auto* xy = outline == nullptr ? nullptr : outline->AsArray ();
    if (!hasEnvelope || !hasEnvelope->AsBool (out.hasEnvelope) || !note || !note->AsString (out.note) ||
        out.note.size () > 4096 || !xy || xy->size () % 2 || xy->size () > 2048 || (!xy->empty () && xy->size () < 6))
        return Fail (error, "Invalid Python inset preview.");
    for (const auto& value : *xy) {
        double coordinate = 0;
        if (!value.AsDouble (coordinate) || !std::isfinite (coordinate))
            return Fail (error, "Invalid Python inset coordinate.");
        out.offsetXY.push_back (coordinate);
    }
    overlaylayers::Mesh mesh;
    const auto *vertices = payload->Find ("vertices"), *normals = payload->Find ("normals"),
               *triangles = payload->Find ("triangles");
    if (vertices == nullptr || normals == nullptr || triangles == nullptr || !Buffer (*vertices, mesh.points, 60000) ||
        (out.hasEnvelope && mesh.points.empty ()) || !Buffer (*normals, mesh.normals, 60000) ||
        mesh.normals.size () != mesh.points.size ())
        return Fail (error, "Invalid Python envelope vertex/normal buffers.");
    const auto* indices = triangles->AsArray ();
    if (indices == nullptr || (out.hasEnvelope && indices->empty ()) || indices->size () % 3 ||
        indices->size () > 60000 || (!out.hasEnvelope && (!mesh.points.empty () || !indices->empty ())))
        return Fail (error, "Invalid Python envelope triangle buffer.");
    for (const auto& value : *indices) {
        double index = 0;
        if (!value.AsDouble (index) || !std::isfinite (index) || std::floor (index) != index || index < 0 ||
            index >= mesh.points.size () / 3)
            return Fail (error, "Invalid Python envelope triangle index.");
        mesh.indices.push_back (uint32_t (index));
    }
    mesh.rgba = 0xDCF3FAFF;
    mesh.styled = true;
    mesh.style.shading = overlaylayers::Shading::Lit;
    mesh.style.opacity = 0.25f;
    if (out.hasEnvelope)
        out.layer.meshes.push_back (std::move (mesh));
    for (const char* key : { "boundary", "offsets", "wires" }) {
        const auto* member = payload->Find (key);
        const auto* lines = member == nullptr ? nullptr : member->AsArray ();
        if (lines == nullptr || lines->size () > 4096)
            return Fail (error, "Invalid Python boundary/feature lines.");
        for (const auto& line : *lines) {
            overlaylayers::Polyline polyline;
            if (!Buffer (line, polyline.points, 60000) || polyline.points.size () < 6)
                return Fail (error, "Invalid Python polyline buffer.");
            polyline.rgba = std::string (key) == "boundary" ? 0xAA4465FF : 0xA66226FF;
            polyline.widthPixels = 2;
            (std::string (key) == "wires" ? out.layer : out.site).polylines.push_back (std::move (polyline));
        }
    }
    const auto* references = payload->Find ("references");
    overlaylayers::PointSet points;
    if (references == nullptr || !Buffer (*references, points.points, 768))
        return Fail (error, "Invalid Python reference points.");
    points.rgba = 0x2F6FEBFF;
    if (!points.points.empty ())
        out.site.points.push_back (std::move (points));
    double faces = 0;
    if (!Number (*payload, "parcelArea", out.parcelArea) || out.parcelArea <= 0 ||
        !Number (*payload, "allowedArea", out.allowedArea) || out.allowedArea < 0 ||
        (out.hasEnvelope && (out.allowedArea <= 0 || out.offsetXY.empty ())) ||
        out.allowedArea > out.parcelArea + 1e-5 || !Number (*payload, "faces", faces) || faces < 0 || faces > 256 ||
        (out.hasEnvelope ? faces < 1 : faces != 0) || std::floor (faces) != faces)
        return Fail (error, "Invalid Python massing figures.");
    out.faces = uint32_t (faces);
    for (const char* key : { "meanZ", "meanASL" }) {
        const auto* value = payload->Find (key);
        if (value == nullptr)
            return Fail (error, "Missing mean elevation result.");
        double mean = 0;
        if (!value->IsNull ()) {
            if (!Number (*payload, key, mean))
                return Fail (error, "Invalid mean elevation result.");
            if (std::string (key) == "meanZ") {
                out.meanZ = mean;
                out.hasMeanZ = true;
            }
            else {
                out.meanASL = mean;
                out.hasMeanASL = true;
            }
        }
    }
    error = overlaylayers::Validate (out.layer);
    if (error.empty ())
        error = overlaylayers::Validate (out.site);
    if (!error.empty ())
        return false;
    result = std::move (out);
    return true;
}
} // namespace geomsrv::archviz::massingcalculation
