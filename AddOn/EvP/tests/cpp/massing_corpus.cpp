#include "massing_corpus.hpp"
#include "ArchViz/MassingBake.hpp"
#include "ArchViz/MassingHeadroom.hpp"
#include "ArchViz/MassingSlices.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace tapioca::testing::massingcorpus {
namespace {
namespace js = evp::nodegraph::json;
namespace ms = geomsrv::archviz::massingslices;
namespace slab = geomsrv::archviz::slabslices;
namespace bake = geomsrv::archviz::massingbake;
namespace buildings = geomsrv::archviz::massingbuildings;
using geomsrv::archviz::SliceChain;

const Value& Field (const Value& value, const char* key)
{
    const auto* member = value.Find (key);
    if (!member)
        throw std::runtime_error (std::string ("missing corpus field: ") + key);
    return *member;
}
std::string Text (const Value& value)
{
    std::string text;
    if (!value.AsString (text))
        throw std::runtime_error ("expected corpus text");
    return text;
}
double Number (const Value& value)
{
    double number = 0;
    if (!value.AsDouble (number) || !std::isfinite (number))
        throw std::runtime_error ("expected finite corpus number");
    return number;
}
const js::JsonArray& Array (const Value& value)
{
    const auto* array = value.AsArray ();
    if (!array)
        throw std::runtime_error ("expected corpus array");
    return *array;
}
std::vector<double> Numbers (const Value& value)
{
    std::vector<double> numbers;
    for (const auto& item : Array (value))
        numbers.push_back (Number (item));
    return numbers;
}
Value Numbers (const std::vector<double>& numbers)
{
    js::JsonArray out;
    for (double number : numbers) {
        if (!std::isfinite (number))
            throw std::runtime_error ("non-finite corpus output");
        out.push_back (Value::Double (number));
    }
    return Value::Array (std::move (out));
}
Value Chains (const std::vector<SliceChain>& chains)
{
    js::JsonArray out;
    for (const auto& chain : chains)
        out.push_back (Value::Object ({ { "closed", Value::Bool (chain.closed) }, { "xy", Numbers (chain.xy) } }));
    return Value::Array (std::move (out));
}
std::vector<SliceChain> Chains (const Value& value)
{
    std::vector<SliceChain> out;
    for (const auto& xy : Array (value))
        out.push_back ({ Numbers (xy), true });
    return out;
}
geomsrv::Mesh Mesh (const Value& meshes, const Value& name)
{
    const auto key = Text (name);
    const auto& value = Field (meshes, key.c_str ());
    geomsrv::Mesh mesh;
    mesh.guid = Text (Field (value, "guid"));
    mesh.vertices = Numbers (Field (value, "vertices"));
    for (const auto& item : Array (Field (value, "triangles"))) {
        int64_t index = -1;
        if (!item.AsInteger (index) || index < 0 || uint64_t (index) > std::numeric_limits<uint32_t>::max ())
            throw std::runtime_error ("invalid corpus triangle index");
        mesh.triangles.push_back (uint32_t (index));
    }
    for (size_t offset = 0; offset + 2 < mesh.vertices.size (); offset += 3)
        mesh.bounds.Expand (mesh.vertices[offset], mesh.vertices[offset + 1], mesh.vertices[offset + 2]);
    return mesh;
}
slab::Slab Slab (const Value& value)
{
    slab::Slab result;
    result.guid = Text (Field (value, "guid"));
    result.id = Text (Field (value, "id"));
    result.bottom = Number (Field (value, "bottom"));
    result.top = Number (Field (value, "top"));
    result.outer.xy = Numbers (Field (value, "outer"));
    for (const auto& hole : Array (Field (value, "holes")))
        result.holes.push_back ({ Numbers (hole), {} });
    return result;
}
geomsrv::archviz::ProjectStoreys Storeys (const Value& value)
{
    geomsrv::archviz::ProjectStoreys result;
    for (const auto& row : Array (value)) {
        int64_t index = 0;
        if (!Field (row, "index").AsInteger (index) || index < std::numeric_limits<int>::min () ||
            index > std::numeric_limits<int>::max ())
            throw std::runtime_error ("invalid corpus storey index");
        result.indices.push_back (int (index));
        result.names.push_back (Text (Field (row, "name")));
        result.levels.push_back (Number (Field (row, "z")));
    }
    return result;
}
std::vector<ms::Input> Inputs (const Value& value, const Value& meshes)
{
    std::vector<ms::Input> result;
    for (const auto& source : Array (value)) {
        ms::Input input;
        input.slab = Slab (source);
        const auto* metadata = Field (source, "metadata").AsObject ();
        if (!metadata)
            throw std::runtime_error ("expected corpus metadata object");
        for (const auto& [key, data] : *metadata) {
            geomsrv::metadata::Property property;
            property.key = key;
            property.value =
                data.Kind () == js::JsonKind::String
                    ? geomsrv::metadata::Value::Text (Text (data))
                    : geomsrv::metadata::Value::Number (Number (data), geomsrv::metadata::ValueType::Length);
            geomsrv::metadata::SetProperty (input.metadata, std::move (property));
        }
        if (const auto* body = source.Find ("body"))
            input.body = std::make_shared<const geomsrv::Mesh> (Mesh (meshes, *body));
        if (const auto* body = source.Find ("facadeBody"))
            input.facadeBody = std::make_shared<const geomsrv::Mesh> (Mesh (meshes, *body));
        result.push_back (std::move (input));
    }
    return result;
}
Value Section (const geomsrv::archviz::hudsection::Section& section)
{
    js::JsonArray floors, spans;
    for (const auto& floor : section.floors) {
        js::JsonArray parts;
        for (const auto& part : floor.parts)
            parts.push_back (Value::Object ({ { "guid", Value::String (part.guid) },
                                              { "sourceStory", Value::Integer (part.sourceStory) },
                                              { "base", Value::Double (part.base) },
                                              { "area", Value::Double (part.areaM2) },
                                              { "value", Value::String (part.value) },
                                              { "rgba", Value::Integer (part.rgba) } }));
        floors.push_back (Value::Object ({ { "story", Value::Integer (floor.storey) },
                                           { "label", Value::String (floor.label) },
                                           { "base", Value::Double (floor.base) },
                                           { "area", Value::Double (floor.areaM2) },
                                           { "parts", Value::Array (std::move (parts)) } }));
    }
    for (const auto& span : section.spans)
        spans.push_back (Value::Object ({ { "guid", Value::String (span.guid) },
                                          { "low", Value::Integer (span.low) },
                                          { "high", Value::Integer (span.high) } }));
    return Value::Object ({ { "known", Value::Bool (section.known) },
                            { "key", Value::String (section.key) },
                            { "note", Value::String (section.note) },
                            { "widest", Value::Double (section.widestM2) },
                            { "floors", Value::Array (std::move (floors)) },
                            { "spans", Value::Array (std::move (spans)) } });
}
Value Build (const Value& input, const Value& meshes)
{
    const auto sources = Inputs (Field (input, "slabs"), meshes);
    const auto storeys = Storeys (Field (input, "storeys"));
    geomsrv::archviz::massingcalculation::Result envelope;
    if (const auto* reference = input.Find ("envelope")) {
        const auto body = Mesh (meshes, *reference);
        geomsrv::archviz::overlaylayers::Mesh mesh;
        mesh.points = body.vertices;
        mesh.indices = body.triangles;
        envelope.hasEnvelope = true;
        envelope.layer.meshes.push_back (std::move (mesh));
    }
    ms::Result result;
    std::string error;
    const bool success = ms::Build (sources, storeys, envelope.hasEnvelope ? &envelope : nullptr, result, error);
    if (!success)
        return Value::Object ({ { "success", Value::Bool (false) }, { "error", Value::String (error) } });
    js::JsonArray rows;
    for (const auto& row : result.rows)
        rows.push_back (Value::Object ({ { "guid", Value::String (row.guid) },
                                         { "story", Value::Integer (row.story) },
                                         { "function", Value::String (row.function) },
                                         { "z", Value::Double (row.z) },
                                         { "height", Value::Double (row.floorHeight) },
                                         { "rawArea", Value::Double (row.rawArea) },
                                         { "allowedArea", Value::Double (row.allowedArea) },
                                         { "excludedArea", Value::Double (row.excludedArea) },
                                         { "rawVolume", Value::Double (row.rawVolume) },
                                         { "allowedVolume", Value::Double (row.allowedVolume) },
                                         { "clipped", Value::Bool (row.clipped) },
                                         { "rgba", Value::Integer (row.rgba) },
                                         { "raw", Chains (row.rawChains) },
                                         { "allowed", Chains (row.chains) },
                                         { "footprint", Chains (row.footprintChains) },
                                         { "low", Chains (row.lowChains) },
                                         { "highlight", Chains (row.highlightChains) } }));
    Value baked;
    const bool bakedOk = bake::Geometry (bake::Kind::Slices, &result, nullptr, {}, baked, error);
    Value bakeResult = Value::Object (
        { { "success", Value::Bool (bakedOk) }, { "error", Value::String (error) }, { "geometry", baked } });
    std::vector<buildings::Record> records;
    std::vector<std::string> selected;
    for (const auto& source : sources) {
        records.push_back ({ source.slab.guid, buildings::Id (source.metadata) });
        selected.push_back (source.slab.guid);
    }
    js::JsonArray previews;
    for (const auto& preview : buildings::Previews (result.section, records, result.heightControls, selected)) {
        js::JsonArray guids;
        for (const auto& guid : preview.building.guids)
            guids.push_back (Value::String (guid));
        previews.push_back (Value::Object ({ { "key", Value::String (preview.building.key) },
                                             { "id", Value::String (preview.building.id) },
                                             { "guids", Value::Array (std::move (guids)) },
                                             { "section", Section (preview.section) },
                                             { "heightControlCount", Value::Integer (preview.heights.size ()) } }));
    }
    return Value::Object ({ { "success", Value::Bool (true) },
                            { "complete", Value::Bool (result.complete) },
                            { "rawArea", Value::Double (result.rawArea) },
                            { "allowedArea", Value::Double (result.allowedArea) },
                            { "excludedArea", Value::Double (result.excludedArea) },
                            { "rawVolume", Value::Double (result.rawVolume) },
                            { "allowedVolume", Value::Double (result.allowedVolume) },
                            { "facadeArea", Value::Double (result.facadeArea) },
                            { "hasFacade", Value::Bool (result.hasFacade) },
                            { "clipped", Value::Bool (result.clipped) },
                            { "note", Value::String (result.note) },
                            { "floorCount", Value::Integer (result.rows.size ()) },
                            { "rows", Value::Array (std::move (rows)) },
                            { "section", Section (result.section) },
                            { "previews", Value::Array (std::move (previews)) },
                            { "bake", std::move (bakeResult) } });
}
Value Slice (const Value& input, const Value& meshes)
{
    const auto source = Slab (Field (input, "slab"));
    slab::Rule rule;
    const auto cut = Text (Field (input, "cut"));
    if (cut == "step")
        rule.cut = slab::Cut::Step;
    else if (cut == "storeys")
        rule.cut = slab::Cut::Storeys;
    else if (cut == "storeyLevels")
        rule.cut = slab::Cut::StoreyLevels;
    else if (cut == "levels")
        rule.cut = slab::Cut::Levels;
    else
        throw std::runtime_error ("unknown corpus slice rule");
    rule.stepMetres = Number (Field (input, "step"));
    rule.minTopMetres = Number (Field (input, "minTop"));
    rule.offsetMetres = Number (Field (input, "offset"));
    rule.levels = Numbers (Field (input, "levels"));
    const auto storeys = Storeys (Field (input, "storeys"));
    std::vector<geomsrv::archviz::storysliceoverlay::Slice> slices;
    slab::Summary summary;
    if (const auto* body = input.Find ("body"))
        summary = slab::SliceBody (source, Mesh (meshes, *body), rule, storeys, slices);
    else
        summary = slab::SliceSlab (source, rule, storeys, slices);
    js::JsonArray output, floors;
    for (const auto& floor : summary.floors)
        floors.push_back (Value::Object ({ { "base", Value::Double (floor.base) },
                                           { "height", Value::Double (floor.height) },
                                           { "area", Value::Double (floor.areaM2) } }));
    for (const auto& slice : slices)
        output.push_back (Value::Object ({ { "name", Value::String (slice.name) },
                                           { "story", Value::Integer (slice.storey) },
                                           { "z", Value::Double (slice.z) },
                                           { "area", Value::Double (slice.areaM2) },
                                           { "chains", Chains (slice.chains) } }));
    return Value::Object ({ { "guid", Value::String (summary.guid) },
                            { "id", Value::String (summary.id) },
                            { "problem", Value::String (summary.problem) },
                            { "body", Value::Bool (summary.body) },
                            { "footprint", Value::Double (summary.footprintM2) },
                            { "sliceArea", Value::Double (summary.sliceAreaM2) },
                            { "area", Value::Double (summary.areaM2) },
                            { "floors", Value::Array (std::move (floors)) },
                            { "slices", Value::Array (std::move (output)) } });
}
} // namespace

Value Evaluate (const Value& input, const Value& meshes)
{
    const auto mode = Text (Field (input, "mode"));
    if (mode == "build")
        return Build (input, meshes);
    if (mode == "slice")
        return Slice (input, meshes);
    std::string error;
    if (mode == "headroom") {
        const auto mesh = Mesh (meshes, Field (input, "body"));
        geomsrv::archviz::massingheadroom::Split split;
        size_t work = 0;
        const bool success =
            geomsrv::archviz::massingheadroom::Partition (Chains (Field (input, "chains")), Number (Field (input, "z")),
                                                          mesh.vertices, mesh.triangles, split, work, error);
        return Value::Object ({ { "success", Value::Bool (success) },
                                { "error", Value::String (error) },
                                { "countedArea", Value::Double (split.countedArea) },
                                { "excludedArea", Value::Double (split.excludedArea) },
                                { "counted", Chains (split.counted) },
                                { "excluded", Chains (split.excluded) } });
    }
    if (mode == "facade") {
        double area = 0;
        const bool success = ms::Facade (Inputs (Field (input, "slabs"), meshes), area, error);
        return Value::Object ({ { "success", Value::Bool (success) },
                                { "error", Value::String (error) },
                                { "area", Value::Double (area) } });
    }
    if (mode == "cleanRing") {
        slab::Ring ring { Numbers (Field (input, "xy")), Numbers (Field (input, "arcs")) }, cleaned;
        const bool success = bake::CleanSlabRing (ring, cleaned, error);
        return Value::Object ({ { "success", Value::Bool (success) },
                                { "error", Value::String (error) },
                                { "xy", Numbers (cleaned.xy) },
                                { "arcs", Numbers (cleaned.arcs) } });
    }
    throw std::runtime_error ("unknown corpus evaluation mode: " + mode);
}

bool Match (const Value& expected, const Value& actual, double tolerance, std::string& mismatch,
            const std::string& path, size_t depth)
{
    const auto fail = [&] () {
        mismatch = path;
        return false;
    };
    if (!std::isfinite (tolerance) || tolerance < 0 || depth > 64 || expected.Kind () != actual.Kind ())
        return fail ();
    if (expected.Kind () == js::JsonKind::Number) {
        if (expected.IsIntegral () != actual.IsIntegral ())
            return fail ();
        if (expected.IsIntegral ()) {
            int64_t a = 0, b = 0;
            return (expected.AsInteger (a) && actual.AsInteger (b) && a == b) || fail ();
        }
        double a = 0, b = 0;
        return (expected.AsDouble (a) && actual.AsDouble (b) && std::isfinite (a) && std::isfinite (b) &&
                std::abs (a - b) <= tolerance) ||
               fail ();
    }
    if (const auto* values = expected.AsArray ()) {
        const auto& other = *actual.AsArray ();
        if (values->size () != other.size ())
            return fail ();
        for (size_t i = 0; i < values->size (); ++i)
            if (!Match ((*values)[i], other[i], tolerance, mismatch, path + "[" + std::to_string (i) + "]", depth + 1))
                return false;
        return true;
    }
    if (const auto* values = expected.AsObject ()) {
        const auto& other = *actual.AsObject ();
        if (values->size () != other.size ())
            return fail ();
        for (const auto& [key, value] : *values) {
            const auto found = other.find (key);
            if (found == other.end () ||
                !Match (value, found->second, tolerance, mismatch, path + "." + key, depth + 1))
                return found == other.end () ? fail () : false;
        }
        return true;
    }
    return js::Write (expected, 0) == js::Write (actual, 0) || fail ();
}
} // namespace tapioca::testing::massingcorpus
