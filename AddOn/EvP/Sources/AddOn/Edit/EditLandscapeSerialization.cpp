#include "Edit/EditLandscapeJsonInternal.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <exception>
#include <set>

namespace tapioca::geometry::landscape_json {
namespace edit = geomsrv::edit;
namespace {
Value Number (double value)
{
    return Value::Double (value);
}
Value Integer (int value)
{
    return Value::Integer (value);
}
Value Text (const std::string& text)
{
    return Value::String (text);
}
Value Flag (bool value)
{
    return Value::Bool (value);
}
Value Numbers (const std::vector<double>& values)
{
    Array result;
    for (double value : values)
        result.push_back (Number (value));
    return Value::Array (std::move (result));
}
Value Descriptors (const std::vector<RegionDescriptor>& values)
{
    Array result;
    for (const auto& value : values)
        result.push_back (DescriptorValue (value));
    return Value::Array (std::move (result));
}
std::string UuidText (const std::array<uint8_t, 16>& uuid)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string text;
    for (size_t i = 0; i < uuid.size (); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            text += '-';
        text += hex[uuid[i] >> 4];
        text += hex[uuid[i] & 15];
    }
    return text;
}
Value Entities (const edit::Document& doc, LandscapeStorage mode)
{
    Array entities;
    for (const auto& entity : doc.entities) {
        Array vertices, segments;
        for (const auto& v : entity.vertices) {
            Object vertex { { "id", IdValue (v.id) },     { "lockXY", Flag (v.lockXY) },
                            { "lockZ", Flag (v.lockZ) },  { "radius", Number (v.radius) },
                            { "in", PointValue (v.in) },  { "out", PointValue (v.out) },
                            { "smooth", Flag (v.smooth) } };
            if (mode == LandscapeStorage::Virtual)
                vertex.emplace ("point", PointValue (v.p));
            vertices.push_back (Value::Object (std::move (vertex)));
        }
        for (const auto& s : entity.segments)
            segments.push_back (Value::Object (
                { { "id", IdValue (s.id) }, { "width", Number (s.width) }, { "tag", IdValue (s.tag) } }));
        entities.push_back (Value::Object ({ { "id", IdValue (entity.id) },
                                             { "kind", Integer (int (entity.kind)) },
                                             { "closed", Flag (entity.closed) },
                                             { "height", Number (entity.height) },
                                             { "role", Text (entity.role) },
                                             { "vertices", Value::Array (std::move (vertices)) },
                                             { "segments", Value::Array (std::move (segments)) } }));
    }
    return Value::Array (std::move (entities));
}
Value PathsValue (const edit::Document& doc, LandscapeStorage mode)
{
    Array paths;
    for (const auto& path : doc.paths) {
        Array points, profile, sections, kerbs;
        for (const auto& p : path.points) {
            Object point { { "id", IdValue (p.id) }, { "radius", Number (p.radius) }, { "lockXY", Flag (p.lockXY) } };
            if (mode == LandscapeStorage::Virtual)
                point.emplace ("point", PointValue (p.p));
            points.push_back (Value::Object (std::move (point)));
        }
        for (const auto& p : path.profile)
            profile.push_back (Value::Object ({ { "id", IdValue (p.id) },
                                                { "station", Number (p.station) },
                                                { "z", Number (p.z) },
                                                { "curve", Number (p.curve) },
                                                { "fixed", Flag (p.fixed) },
                                                { "anchor", IdValue (p.anchor) },
                                                { "binding", Text (p.binding) },
                                                { "crossing", IdValue (p.crossing) } }));
        for (const auto& s : path.sections)
            sections.push_back (Value::Object ({ { "id", IdValue (s.id) },
                                                 { "station", Number (s.station) },
                                                 { "widthLeft", Number (s.widthLeft) },
                                                 { "widthRight", Number (s.widthRight) },
                                                 { "crossfall", Number (s.crossfall) },
                                                 { "fall", Integer (int (s.fall)) },
                                                 { "shoulder", Number (s.shoulder) },
                                                 { "cutSlope", Number (s.cutSlope) },
                                                 { "fillSlope", Number (s.fillSlope) },
                                                 { "rounding", Number (s.rounding) } }));
        for (const auto& k : path.kerbs)
            kerbs.push_back (Value::Object ({ { "point", IdValue (k.point) },
                                              { "other", IdValue (k.other) },
                                              { "side", Integer (int (k.side)) },
                                              { "otherSide", Integer (int (k.otherSide)) },
                                              { "radius", Number (k.radius) } }));
        paths.push_back (Value::Object ({ { "id", IdValue (path.id) },
                                          { "role", Text (path.role) },
                                          { "edgeRadius", Number (path.edgeRadius) },
                                          { "points", Value::Array (std::move (points)) },
                                          { "profile", Value::Array (std::move (profile)) },
                                          { "sections", Value::Array (std::move (sections)) },
                                          { "kerbs", Value::Array (std::move (kerbs)) } }));
    }
    return Value::Array (std::move (paths));
}
// Locale-independent JSON numbers. The shared value reader owns syntax trees;
// persistence must not depend on a host process's C numeric locale.
void WriteValue (const Value& value, std::string& out)
{
    if (const auto* object = value.AsObject ()) {
        out += '{';
        bool first = true;
        for (const auto& [key, member] : *object) {
            if (!first)
                out += ',';
            first = false;
            out += json::Write (Text (key), 0);
            out += ':';
            WriteValue (member, out);
        }
        out += '}';
        return;
    }
    if (const auto* array = value.AsArray ()) {
        out += '[';
        bool first = true;
        for (const auto& member : *array) {
            if (!first)
                out += ',';
            first = false;
            WriteValue (member, out);
        }
        out += ']';
        return;
    }
    if (value.Kind () == json::JsonKind::Number) {
        char buffer[64];
        double number = 0;
        value.AsDouble (number);
        if (!std::isfinite (number))
            Fail ("document", "non-finite number cannot be serialized");
        const auto result = std::to_chars (buffer, buffer + sizeof buffer, number);
        if (result.ec != std::errc {})
            Fail ("document", "number cannot be serialized");
        out.append (buffer, result.ptr);
        return;
    }
    out += json::Write (value, 0);
}
} // namespace

Value IdsValue (const std::vector<Id>& ids)
{
    Array result;
    for (Id id : ids)
        result.push_back (IdValue (id));
    return Value::Array (std::move (result));
}
Value PointValue (Vec3 point)
{
    return Value::Object ({ { "x", Number (point.x) }, { "y", Number (point.y) }, { "z", Number (point.z) } });
}
Value MaterialValue (const MaterialSpec& m)
{
    return Value::Object ({ { "preset", Integer (int (m.preset)) },
                            { "fillGuid", Text (m.fillGuid) },
                            { "fillName", Text (m.fillName) },
                            { "surfaceGuid", Text (m.surfaceGuid) },
                            { "surfaceName", Text (m.surfaceName) },
                            { "hatchDirection", Number (m.hatchDirection) },
                            { "hatchDensity", Number (m.hatchDensity) } });
}
Value DescriptorValue (const RegionDescriptor& d)
{
    Array cycles;
    for (const auto& boundary : d.boundaries) {
        Array cycle;
        for (const auto& ref : boundary)
            cycle.push_back (Value::Object ({ { "source", IdValue (ref.source) },
                                              { "segment", IdValue (ref.segment) },
                                              { "reversed", Flag (ref.reversed) } }));
        cycles.push_back (Value::Array (std::move (cycle)));
    }
    return Value::Object ({ { "object", IdValue (d.object) },
                            { "sources", IdsValue (d.sources) },
                            { "operations", IdsValue (d.operations) },
                            { "boundaries", Value::Array (std::move (cycles)) } });
}
Value DocumentValue (const edit::Document& doc, LandscapeStorage mode)
{
    const auto& s = *doc.landscape;
    Array sources, operations, objects, regions, assignments, patches, bindings, controls, entrances;
    for (const auto& e : s.entrances)
        entrances.push_back (Value::Object ({ { "id", IdValue (e.id) },
                                              { "source", IdValue (e.source) },
                                              { "along", Number (e.along) },
                                              { "z", Number (e.z) },
                                              { "door", Text (e.doorGuid) } }));
    for (const auto& control : s.terrainControls)
        controls.push_back (Value::Object ({ { "source", IdValue (control.source) },
                                             { "vertex", IdValue (control.vertex) },
                                             { "enabled", Flag (control.enabled) } }));
    for (const auto& v : s.sources)
        sources.push_back (Value::Object ({ { "id", IdValue (v.id) },
                                            { "entity", IdValue (v.entityId) },
                                            { "path", IdValue (v.pathId) },
                                            { "role", Integer (int (v.role)) },
                                            { "guid", Text (v.archicadGuid) },
                                            { "revision", IdValue (v.revision) },
                                            { "deleted", Flag (v.deleted) },
                                            { "arcAngles", Numbers (v.arcAngles) } }));
    for (const auto& v : s.operations)
        operations.push_back (Value::Object ({ { "id", IdValue (v.id) },
                                               { "kind", Integer (int (v.kind)) },
                                               { "inputs", IdsValue (v.inputs) },
                                               { "faces", Descriptors (v.faces) },
                                               { "enabled", Flag (v.enabled) },
                                               { "width", Number (v.width) },
                                               { "offset", Number (v.offset) },
                                               { "material", MaterialValue (v.material) },
                                               { "priority", Integer (v.priority) } }));
    for (const auto& v : s.objects)
        objects.push_back (Value::Object ({ { "id", IdValue (v.id) },
                                            { "path", IdValue (v.pathId) },
                                            { "footprint", IdValue (v.footprintOperation) },
                                            { "kind", Integer (int (v.kind)) },
                                            { "name", Text (v.name) },
                                            { "material", MaterialValue (v.baseMaterial) },
                                            { "thickness", Number (v.thickness) },
                                            { "masks", IdsValue (v.maskOperations) } }));
    for (const auto& v : s.regions)
        regions.push_back (Value::Object ({ { "id", IdValue (v.id) },
                                            { "descriptor", DescriptorValue (v.descriptor) },
                                            { "predecessors", IdsValue (v.predecessors) },
                                            { "active", Flag (v.active) } }));
    for (const auto& v : s.assignments)
        assignments.push_back (Value::Object ({ { "id", IdValue (v.id) },
                                                { "region", IdValue (v.region) },
                                                { "mask", IdValue (v.mask) },
                                                { "material", MaterialValue (v.material) },
                                                { "priority", Integer (v.priority) },
                                                { "valid", Flag (v.valid) } }));
    for (const auto& v : s.patches)
        patches.push_back (Value::Object ({ { "key", IdValue (v.key) },
                                            { "object", IdValue (v.object) },
                                            { "assignment", IdValue (v.assignment) },
                                            { "role", Text (v.role) },
                                            { "representation", Text (v.representation) },
                                            { "sources", IdsValue (v.sources) },
                                            { "predecessorRegions", IdsValue (v.predecessorRegions) },
                                            { "predecessorPatches", IdsValue (v.predecessorPatches) } }));
    for (const auto& v : s.bindings) {
        Array guids;
        for (const auto& guid : v.nativeGuids)
            guids.push_back (Text (guid));
        bindings.push_back (Value::Object ({ { "kind", Integer (int (v.kind)) },
                                             { "id", IdValue (v.logicalId) },
                                             { "guids", Value::Array (std::move (guids)) },
                                             { "metadataUuid", Text (v.metadataUuid) },
                                             { "role", Text (v.role) },
                                             { "revision", IdValue (v.revision) } }));
    }
    return Value::Object (
        { { "version", Integer (1) },
          { "mode", Text (mode == LandscapeStorage::Virtual ? "virtual" : "native") },
          { "sessionUuid", Text (UuidText (s.sessionUuid)) },
          { "nextId", IdValue (doc.nextId) },
          { "revision", IdValue (doc.revision) },
          { "entities", Entities (doc, mode) },
          { "paths", PathsValue (doc, mode) },
          { "landscape", Value::Object ({ { "sourceTerrainGuid", Text (s.sourceTerrainGuid) },
                                          { "propertyLine", IdValue (s.propertyLineSourceId) },
                                          { "sources", Value::Array (std::move (sources)) },
                                          { "operations", Value::Array (std::move (operations)) },
                                          { "objects", Value::Array (std::move (objects)) },
                                          { "regions", Value::Array (std::move (regions)) },
                                          { "assignments", Value::Array (std::move (assignments)) },
                                          { "patches", Value::Array (std::move (patches)) },
                                          { "bindings", Value::Array (std::move (bindings)) },
                                          { "terrainControls", Value::Array (std::move (controls)) },
                                          { "entrances", Value::Array (std::move (entrances)) } }) } });
}

bool ValidateStoredRegions (const edit::Document& doc, std::string& error)
{
    LandscapeResult rebuilt;
    if (!EvaluateLandscape (doc, rebuilt)) {
        error = rebuilt.error;
        return false;
    }
    // Persistence is a checkpoint, not a repair operation: active durable IDs
    // and current footprints must be a bijection. Inactive ancestry is retained.
    std::set<Id> matched;
    for (const auto& region : rebuilt.regions)
        if (region.regionId == 0 || !matched.insert (region.regionId).second) {
            error = "landscape.regions: footprint lacks a unique persisted active identity";
            return false;
        }
    for (const auto& identity : doc.landscape->regions)
        if (identity.active && !matched.count (identity.id)) {
            error = "landscape.regions: active identity has no current footprint";
            return false;
        }
    return true;
}
} // namespace tapioca::geometry::landscape_json

namespace tapioca::geometry {
bool WriteLandscape (const geomsrv::edit::Document& doc, LandscapeStorage mode, std::string& text, std::string& error)
{
    error.clear ();
    if (!doc.landscape || mode > LandscapeStorage::Native) {
        error = "landscape/mode: session and valid storage mode required";
        return false;
    }
    if (!geomsrv::edit::Validate (doc, error))
        return false;
    if (mode == LandscapeStorage::Native) {
        std::set<Id> entities, paths;
        for (const auto& source : doc.landscape->sources) {
            if (source.archicadGuid.empty ()) {
                error = "landscape.sources.guid: native storage requires a source GUID";
                return false;
            }
            if (source.entityId)
                entities.insert (source.entityId);
            if (source.pathId)
                paths.insert (source.pathId);
        }
        for (const auto& entity : doc.entities)
            if (!entities.count (entity.id)) {
                error = "entities: native storage requires a captured source binding for every entity";
                return false;
            }
        for (const auto& path : doc.paths)
            if (!paths.count (path.id)) {
                error = "paths: native storage requires a captured source binding for every path";
                return false;
            }
    }
    try {
        if (!landscape_json::ValidateStoredRegions (doc, error))
            return false;
        std::string candidate;
        landscape_json::WriteValue (landscape_json::DocumentValue (doc, mode), candidate);
        if (candidate.size () > 8 * 1024 * 1024) {
            error = "document exceeds the 8 MiB codec budget";
            return false;
        }
        text = std::move (candidate);
        return true;
    }
    catch (const std::exception& e) {
        error = e.what ();
        return false;
    }
}
} // namespace tapioca::geometry
