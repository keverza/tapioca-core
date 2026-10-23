#include "Edit/EditLandscapeJsonInternal.hpp"
#include <algorithm>
#include <charconv>
#include <limits>
#include <set>

namespace tapioca::geometry::landscape_json {
namespace edit = geomsrv::edit;
namespace {
template <class F> void Each (const Reader& reader, F function)
{
    size_t i = 0;
    for (const auto& value : reader.Items ())
        function (Reader (value, reader.path + "[" + std::to_string (i++) + "]"));
}
template <class T> T Enum (const Reader& reader, T last)
{
    const int value = reader.Integer ();
    if (value < 0 || value > int (last))
        Fail (reader.path, "unknown enum value");
    return static_cast<T> (value);
}
std::string Guid (const Reader& reader)
{
    auto text = reader.Text ();
    if (!text.empty () && !CanonicalLandscapeGuid (text, text))
        Fail (reader.path, "malformed/nil GUID");
    return text;
}
std::array<uint8_t, 16> Uuid (const Reader& reader)
{
    const auto text = Guid (reader);
    if (text.empty ())
        Fail (reader.path, "non-nil session UUID required");
    std::array<uint8_t, 16> uuid {};
    size_t byte = 0;
    for (size_t i = 0; i < text.size ();) {
        if (text[i] == '-') {
            ++i;
            continue;
        }
        unsigned int value = 0;
        const auto parsed = std::from_chars (text.data () + i, text.data () + i + 2, value, 16);
        if (parsed.ec != std::errc {})
            Fail (reader.path, "invalid UUID byte");
        uuid[byte++] = static_cast<uint8_t> (value);
        i += 2;
    }
    return uuid;
}
void Entities (const Reader& root, edit::Document& doc, LandscapeStorage mode)
{
    Each (root.Field ("entities"), [&] (const Reader& item) {
        item.Fields ({ "id", "kind", "closed", "height", "role", "vertices", "segments" });
        edit::Entity e;
        e.id = item.Field ("id").Identifier ();
        e.kind = Enum (item.Field ("kind"), edit::Kind::Extrusion);
        e.closed = item.Field ("closed").Flag ();
        e.height = item.Field ("height").Number ();
        e.role = item.Field ("role").Text ();
        Each (item.Field ("vertices"), [&] (const Reader& vertex) {
            if (mode == LandscapeStorage::Virtual)
                vertex.Fields ({ "id", "point", "lockXY", "lockZ", "radius", "in", "out", "smooth" });
            else
                vertex.Fields ({ "id", "lockXY", "lockZ", "radius", "in", "out", "smooth" });
            edit::Vertex v;
            v.id = vertex.Field ("id").Identifier ();
            if (mode == LandscapeStorage::Virtual)
                v.p = vertex.Field ("point").Point ();
            else
                v.p = { std::numeric_limits<double>::quiet_NaN (), 0, 0 };
            v.lockXY = vertex.Field ("lockXY").Flag ();
            v.lockZ = vertex.Field ("lockZ").Flag ();
            v.radius = vertex.Field ("radius").Number ();
            v.in = vertex.Field ("in").Point ();
            v.out = vertex.Field ("out").Point ();
            v.smooth = vertex.Field ("smooth").Flag ();
            if (v.radius < 0)
                Fail (vertex.path + ".radius", "non-negative radius required");
            e.vertices.push_back (v);
        });
        Each (item.Field ("segments"), [&] (const Reader& segment) {
            segment.Fields ({ "id", "width", "tag" });
            const auto tag = segment.Field ("tag").Identifier ();
            if (tag > std::numeric_limits<uint32_t>::max ())
                Fail (segment.path + ".tag", "32-bit tag overflow");
            e.segments.push_back (
                { segment.Field ("id").Identifier (), segment.Field ("width").Number (), static_cast<uint32_t> (tag) });
        });
        doc.entities.push_back (std::move (e));
    });
    Each (root.Field ("paths"), [&] (const Reader& item) {
        item.Fields ({ "id", "role", "edgeRadius", "points", "profile", "sections", "kerbs" });
        edit::Path path;
        path.id = item.Field ("id").Identifier ();
        path.role = item.Field ("role").Text ();
        path.edgeRadius = item.Field ("edgeRadius").Number ();
        Each (item.Field ("points"), [&] (const Reader& point) {
            if (mode == LandscapeStorage::Virtual)
                point.Fields ({ "id", "point", "radius", "lockXY" });
            else
                point.Fields ({ "id", "radius", "lockXY" });
            edit::ControlPoint p;
            p.id = point.Field ("id").Identifier ();
            if (mode == LandscapeStorage::Virtual)
                p.p = point.Field ("point").Point ();
            else
                p.p = { std::numeric_limits<double>::quiet_NaN (), 0, 0 };
            p.radius = point.Field ("radius").Number ();
            p.lockXY = point.Field ("lockXY").Flag ();
            path.points.push_back (p);
        });
        Each (item.Field ("profile"), [&] (const Reader& point) {
            point.Fields ({ "id", "station", "z", "curve", "fixed", "anchor", "binding", "crossing" });
            edit::GradeBreak p;
            p.id = point.Field ("id").Identifier ();
            p.station = point.Field ("station").Number ();
            p.z = point.Field ("z").Number ();
            p.curve = point.Field ("curve").Number ();
            p.fixed = point.Field ("fixed").Flag ();
            p.anchor = point.Field ("anchor").Identifier ();
            p.binding = point.Field ("binding").Text ();
            p.crossing = point.Field ("crossing").Identifier ();
            path.profile.push_back (p);
        });
        Each (item.Field ("sections"), [&] (const Reader& section) {
            section.Fields ({ "id", "station", "widthLeft", "widthRight", "crossfall", "fall", "shoulder", "cutSlope",
                              "fillSlope" });
            edit::Section s;
            s.id = section.Field ("id").Identifier ();
            s.station = section.Field ("station").Number ();
            s.widthLeft = section.Field ("widthLeft").Number ();
            s.widthRight = section.Field ("widthRight").Number ();
            s.crossfall = section.Field ("crossfall").Number ();
            s.fall = Enum (section.Field ("fall"), edit::Fall::Right);
            s.shoulder = section.Field ("shoulder").Number ();
            s.cutSlope = section.Field ("cutSlope").Number ();
            s.fillSlope = section.Field ("fillSlope").Number ();
            path.sections.push_back (s);
        });
        Each (item.Field ("kerbs"), [&] (const Reader& kerb) {
            kerb.Fields ({ "point", "other", "side", "otherSide", "radius" });
            path.kerbs.push_back ({ kerb.Field ("point").Identifier (), kerb.Field ("other").Identifier (),
                                    Enum (kerb.Field ("side"), edit::KerbSide::End),
                                    Enum (kerb.Field ("otherSide"), edit::KerbSide::End),
                                    kerb.Field ("radius").Number () });
        });
        doc.paths.push_back (std::move (path));
    });
}
} // namespace

void ReadDocument (const Reader& root, edit::Document& doc, LandscapeStorage mode)
{
    root.Fields ({ "version", "mode", "sessionUuid", "nextId", "revision", "entities", "paths", "landscape" });
    doc.nextId = root.Field ("nextId").Identifier ();
    doc.revision = root.Field ("revision").Identifier ();
    if (doc.nextId == 0)
        Fail (root.path + ".nextId", "allocator high-water must be nonzero");
    doc.landscape.emplace ();
    auto& s = *doc.landscape;
    s.sessionUuid = Uuid (root.Field ("sessionUuid"));
    Entities (root, doc, mode);
    const auto landscape = root.Field ("landscape");
    landscape.Fields ({ "sourceTerrainGuid", "propertyLine", "sources", "operations", "objects", "regions",
                        "assignments", "patches", "bindings" },
                      { "edges", "terrainControls" });
    if (landscape.Has ("edges") && !landscape.Field ("edges").Items ().empty ())
        Fail (landscape.path + ".edges", "edge definitions require a later declared schema");
    s.sourceTerrainGuid = Guid (landscape.Field ("sourceTerrainGuid"));
    s.propertyLineSourceId = landscape.Field ("propertyLine").Identifier ();
    if (landscape.Has ("terrainControls"))
        Each (landscape.Field ("terrainControls"), [&] (const Reader& item) {
            item.Fields ({ "source", "vertex", "enabled" });
            s.terrainControls.push_back ({ item.Field ("source").Identifier (), item.Field ("vertex").Identifier (),
                                           item.Field ("enabled").Flag () });
        });
    Each (landscape.Field ("sources"), [&] (const Reader& item) {
        item.Fields ({ "id", "entity", "path", "role", "guid", "revision", "deleted", "arcAngles" });
        LandscapeSource source;
        source.id = item.Field ("id").Identifier ();
        source.entityId = item.Field ("entity").Identifier ();
        source.pathId = item.Field ("path").Identifier ();
        source.role = Enum (item.Field ("role"), SourceRole::Terrain);
        source.archicadGuid = Guid (item.Field ("guid"));
        source.revision = item.Field ("revision").Identifier ();
        source.deleted = item.Field ("deleted").Flag ();
        Each (item.Field ("arcAngles"), [&] (const Reader& angle) { source.arcAngles.push_back (angle.Number ()); });
        s.sources.push_back (std::move (source));
    });
    Each (landscape.Field ("operations"), [&] (const Reader& item) {
        item.Fields ({ "id", "kind", "inputs", "faces", "enabled", "width", "offset", "material", "priority" });
        LandscapeOperation op;
        op.id = item.Field ("id").Identifier ();
        op.kind = Enum (item.Field ("kind"), OperationKind::ProtectedZone);
        op.inputs = item.Field ("inputs").Ids ();
        Each (item.Field ("faces"), [&] (const Reader& face) { op.faces.push_back (face.Descriptor ()); });
        op.enabled = item.Field ("enabled").Flag ();
        op.width = item.Field ("width").Number ();
        op.offset = item.Field ("offset").Number ();
        op.material = item.Field ("material").Material ();
        op.priority = item.Field ("priority").Integer ();
        s.operations.push_back (std::move (op));
    });
    Each (landscape.Field ("objects"), [&] (const Reader& item) {
        item.Fields ({ "id", "path", "footprint", "kind", "name", "material", "thickness", "masks" });
        HardscapeObject object;
        object.id = item.Field ("id").Identifier ();
        object.pathId = item.Field ("path").Identifier ();
        object.footprintOperation = item.Field ("footprint").Identifier ();
        object.kind = Enum (item.Field ("kind"), ObjectKind::Edge);
        object.name = item.Field ("name").Text ();
        object.baseMaterial = item.Field ("material").Material ();
        object.thickness = item.Field ("thickness").Number ();
        object.maskOperations = item.Field ("masks").Ids ();
        s.objects.push_back (std::move (object));
    });
    Each (landscape.Field ("regions"), [&] (const Reader& item) {
        item.Fields ({ "id", "descriptor", "predecessors", "active" });
        s.regions.push_back ({ item.Field ("id").Identifier (), item.Field ("descriptor").Descriptor (),
                               item.Field ("predecessors").Ids (), item.Field ("active").Flag () });
    });
    Each (landscape.Field ("assignments"), [&] (const Reader& item) {
        item.Fields ({ "id", "region", "mask", "material", "priority", "valid" });
        s.assignments.push_back ({ item.Field ("id").Identifier (), item.Field ("region").Identifier (),
                                   item.Field ("mask").Identifier (), item.Field ("material").Material (),
                                   item.Field ("priority").Integer (), item.Field ("valid").Flag () });
    });
    Each (landscape.Field ("patches"), [&] (const Reader& item) {
        item.Fields ({ "key", "object", "assignment", "role", "representation", "sources", "predecessorRegions",
                       "predecessorPatches" });
        s.patches.push_back ({ item.Field ("key").Identifier (), item.Field ("object").Identifier (),
                               item.Field ("assignment").Identifier (), item.Field ("role").Text (),
                               item.Field ("representation").Text (), item.Field ("sources").Ids (),
                               item.Field ("predecessorRegions").Ids (), item.Field ("predecessorPatches").Ids () });
    });
    Each (landscape.Field ("bindings"), [&] (const Reader& item) {
        item.Fields ({ "kind", "id", "guids", "metadataUuid", "role", "revision" });
        LandscapeBinding binding;
        binding.kind = Enum (item.Field ("kind"), BindingKind::Object);
        binding.logicalId = item.Field ("id").Identifier ();
        Each (item.Field ("guids"), [&] (const Reader& guid) {
            const auto value = Guid (guid);
            if (value.empty ())
                Fail (guid.path, "nonempty native GUID required");
            binding.nativeGuids.push_back (value);
        });
        binding.metadataUuid = Guid (item.Field ("metadataUuid"));
        binding.role = item.Field ("role").Text ();
        binding.revision = item.Field ("revision").Identifier ();
        s.bindings.push_back (std::move (binding));
    });
}
} // namespace tapioca::geometry::landscape_json

namespace tapioca::geometry {
bool ReadLandscape (const std::string& text, geomsrv::edit::Document& doc, std::string& error,
                    const LandscapeCapture& capture)
{
    namespace lj = landscape_json;
    error.clear ();
    try {
        std::string normalized;
        if (!lj::StrictJson (text, normalized, error))
            return false;
        const auto parsed = lj::json::Parse (normalized, 64);
        if (!parsed.ok) {
            error = "JSON byte " + std::to_string (parsed.offset) + ": " + parsed.error;
            return false;
        }
        lj::Reader root (parsed.value, "document");
        if (root.Field ("version").Integer () != 1)
            lj::Fail ("document.version", "unsupported landscape schema version");
        const auto modeText = root.Field ("mode").Text ();
        if (modeText != "native" && modeText != "virtual")
            lj::Fail ("document.mode", "explicit virtual/native mode required");
        const auto mode = modeText == "native" ? LandscapeStorage::Native : LandscapeStorage::Virtual;
        geomsrv::edit::Document candidate;
        lj::ReadDocument (root, candidate, mode);
        if (mode == LandscapeStorage::Native) {
            std::set<Id> captured;
            for (const auto& source : candidate.landscape->sources) {
                if (source.role == SourceRole::Terrain)
                    continue; // No authored points; its GUID is checked against the session's.
                if (source.archicadGuid.empty ())
                    lj::Fail ("landscape.sources.guid", "native storage requires source GUIDs");
                if (!capture)
                    lj::Fail ("landscape.sources", "native reload requires an authored-ID capture");
                std::map<Id, Vec3> points;
                if (!capture (source, points, error)) {
                    if (error.empty ())
                        error = "landscape.sources: source capture refused";
                    return false;
                }
                size_t matched = 0;
                const auto restore = [&] (Id id, Vec3& point) {
                    const auto found = points.find (id);
                    if (found == points.end () || !geomsrv::edit::Finite (found->second))
                        lj::Fail ("landscape.sources.capture", "missing/non-finite authored point");
                    if (captured.count (id) && !(point == found->second))
                        lj::Fail ("landscape.sources.capture", "conflicting captures for one authored point");
                    point = found->second;
                    captured.insert (id);
                    ++matched;
                };
                if (source.entityId) {
                    auto* entity = geomsrv::edit::Find (candidate, source.entityId);
                    if (!entity)
                        lj::Fail ("landscape.sources.entity", "unknown geometry owner");
                    for (auto& vertex : entity->vertices)
                        restore (vertex.id, vertex.p);
                }
                else if (source.pathId) {
                    auto* path = geomsrv::edit::FindPath (candidate, source.pathId);
                    if (!path)
                        lj::Fail ("landscape.sources.path", "unknown geometry owner");
                    for (auto& point : path->points)
                        restore (point.id, point.p);
                }
                if (matched != points.size ())
                    lj::Fail ("landscape.sources.capture", "extra or duplicate correspondence");
            }
        }
        if (!geomsrv::edit::Validate (candidate, error))
            return false;
        // Validate the restored semantic records without allocating/retargeting
        // during read. A caller explicitly rebuilds after hydration.
        if (!lj::ValidateStoredRegions (candidate, error))
            return false;
        doc = std::move (candidate);
        return true;
    }
    catch (const std::exception& e) {
        error = e.what ();
        return false;
    }
}
} // namespace tapioca::geometry
