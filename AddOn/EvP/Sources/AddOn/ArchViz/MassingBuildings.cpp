#include "ArchViz/MassingBuildings.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace geomsrv::archviz::massingbuildings {
namespace {
std::string Key (const Record& record)
{
    return record.id.empty () ? "slab:" + record.guid : "building:" + record.id;
}
bool Has (const std::vector<std::string>& guids, const std::string& guid)
{
    return std::find (guids.begin (), guids.end (), guid) != guids.end ();
}
uint32_t Colour (size_t index)
{
    // Golden-angle hue spacing: deterministic under input reordering and no
    // repeated palette entries in the bounded 128-slab inspection.
    const double hue = std::fmod (double (index) * 0.618033988749895, 1.0);
    uint32_t colour = 0;
    for (double offset : { 0.0, 2.0 / 3, 1.0 / 3 }) {
        const double wave = std::abs (std::fmod (hue + offset, 1.0) * 6 - 3);
        const double channel = 0.82 * (0.28 + 0.72 * std::clamp (wave - 1, 0.0, 1.0));
        colour = (colour << 8) | uint32_t (std::lround (channel * 255));
    }
    return (colour << 8) | 0xFF;
}
hudsection::Section BuildingSection (const hudsection::Section& source, const std::vector<std::string>& guids)
{
    auto section = hudsection::Filter (source, guids);
    std::map<int64_t, hudsection::Floor> floors;
    for (const auto& floor : section.floors)
        for (const auto& part : floor.parts) {
            const double base = std::isfinite (part.base) ? part.base : floor.base;
            auto& row = floors[int64_t (std::llround (base * 1e6))];
            row.base = base;
            row.areaM2 += part.areaM2;
            row.parts.push_back (part);
            row.storey = floor.storey;
        }
    // Diagram rows follow actual building elevations, not each slab's independently
    // advancing numbers. Parts retain authored indices: no existing range is reindexed.
    int story = floors.empty () ? 0 : floors.begin ()->second.storey;
    section.floors.clear ();
    section.widestM2 = 0;
    for (auto& [base, row] : floors) {
        row.storey = story++;
        row.label = "Floor " + std::to_string (row.storey);
        section.widestM2 = (std::max) (section.widestM2, row.areaM2);
        section.floors.push_back (std::move (row));
    }
    return section;
}
} // namespace
std::string Id (const metadata::EntityMetadata& entity)
{
    const auto* property = metadata::FindProperty (entity, "massing.buildingId");
    return property && property->value.type == metadata::ValueType::String ? property->value.s : std::string ();
}
std::vector<Group> Groups (const std::vector<Record>& records)
{
    std::map<std::string, Group> groups;
    for (const auto& record : records) {
        auto& group = groups[Key (record)];
        group.key = Key (record);
        group.id = record.id;
        if (!Has (group.guids, record.guid))
            group.guids.push_back (record.guid);
    }
    std::vector<Group> out;
    for (auto& [key, group] : groups) {
        std::sort (group.guids.begin (), group.guids.end ());
        out.push_back (std::move (group));
    }
    return out;
}
std::vector<std::string> Members (const std::vector<Record>& records, const std::vector<std::string>& seeds)
{
    std::set<std::string> members (seeds.begin (), seeds.end ());
    for (const auto& group : Groups (records))
        if (std::any_of (group.guids.begin (), group.guids.end (),
                         [&] (const auto& guid) { return Has (seeds, guid); }))
            members.insert (group.guids.begin (), group.guids.end ());
    return { members.begin (), members.end () };
}
std::vector<Preview> Previews (const hudsection::Section& section, const std::vector<Record>& records,
                               const std::vector<hudmeta::Page>& heights, const std::vector<std::string>& selected)
{
    std::vector<Preview> out;
    for (const auto& group : Groups (records)) {
        if (!std::any_of (group.guids.begin (), group.guids.end (),
                          [&] (const auto& guid) { return Has (selected, guid); }))
            continue;
        Preview preview;
        preview.building = group;
        preview.section = BuildingSection (section, group.guids);
        if (std::any_of (group.guids.begin (), group.guids.end (), [&] (const auto& guid) {
                return std::none_of (section.spans.begin (), section.spans.end (),
                                     [&] (const auto& span) { return span.guid == guid; });
            })) {
            preview.section = {};
            preview.section.note =
                "Building section awaits every matching slab's current story slices; no partial building shown.";
        }
        for (const auto& page : heights)
            if (Has (group.guids, page.element))
                preview.heights.push_back (page);
        out.push_back (std::move (preview));
    }
    return out;
}
bool Inspect (const std::vector<Surface>& surfaces, const std::string& key, overlaylayers::Layer& layer,
              std::string& error)
{
    error.clear ();
    if (surfaces.size () > 128)
        return error = "Building inspection accepts at most 128 slabs.", false;
    std::vector<Record> records;
    for (const auto& surface : surfaces)
        records.push_back (surface.record);
    overlaylayers::Layer out;
    out.name = key.empty () ? kUniqueLayer : kSelectedLayer;
    out.views = overlaylayers::Views::Both;
    out.occlusion = overlaylayers::Behind::Fade;
    size_t points = 0, indices = 0, index = 0;
    for (const auto& group : Groups (records)) {
        const uint32_t rgba = key.empty () ? Colour (index++) : 0xD9822BFF;
        if (!key.empty () && key != group.key)
            continue;
        for (const auto& surface : surfaces) {
            if (!Has (group.guids, surface.record.guid))
                continue;
            if (!surface.body || surface.body->vertices.empty () || surface.body->triangles.empty ())
                return error =
                           "Building inspection awaits every current operated slab body; no partial building shown.",
                       false;
            points += surface.body->vertices.size ();
            indices += surface.body->triangles.size ();
            if (points > 1800000 || indices > 1800000)
                return error = "Building inspection exceeds its geometry budget.", false;
            overlaylayers::Mesh mesh;
            mesh.points = surface.body->vertices;
            mesh.indices = surface.body->triangles;
            mesh.rgba = rgba;
            mesh.styled = true;
            mesh.style.opacity = key.empty () ? 0.8f : 0.5f;
            mesh.style.behind = overlaylayers::Behind::Fade;
            mesh.style.edgeRgba = rgba;
            mesh.style.edgeWidthPixels = key.empty () ? 1.0f : 2.5f;
            mesh.hoverTitle = group.id.empty () ? "No building ID: " + surface.record.guid : "Building " + group.id;
            mesh.hoverRows.push_back ({ "Slab", surface.record.guid });
            out.meshes.push_back (std::move (mesh));
        }
    }
    error = overlaylayers::Validate (out);
    if (!error.empty ())
        return false;
    layer = std::move (out);
    return true;
}
} // namespace geomsrv::archviz::massingbuildings
