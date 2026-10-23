#include "Edit/EditTerrainControls.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditTerrainAdapterInternal.hpp"
#include <algorithm>
#include <stdexcept>

namespace tapioca::geometry::terrain_detail {
namespace edit = geomsrv::edit;

bool CompileTerrainControls (const edit::Document& doc, const ProvenanceRequest& captured, TerrainControls& result,
                             std::string& error)
{
    error.clear ();
    if (!doc.landscape) {
        error = "TerrainControlOrphaned: fixed controls require a landscape session";
        return false;
    }
    if (!ValidateLandscape (doc, error))
        return false;
    TerrainControls candidate;
    for (const auto& control : doc.landscape->terrainControls) {
        if (!control.enabled)
            continue;
        const auto source = std::find_if (doc.landscape->sources.begin (), doc.landscape->sources.end (),
                                          [&] (const auto& s) { return s.id == control.source && !s.deleted; });
        const auto* entity = source == doc.landscape->sources.end () ? nullptr : edit::Find (doc, source->entityId);
        if (!entity || entity->kind != edit::Kind::Polyline ||
            !CatalogSource (doc, captured, control.source, "fixed terrain control", error)) {
            if (error.empty ())
                error = "TerrainControlOrphaned: an enabled control has no live captured guide";
            return false;
        }
        if (control.vertex) {
            const auto vertex = std::find_if (entity->vertices.begin (), entity->vertices.end (),
                                              [&] (const auto& v) { return v.id == control.vertex; });
            if (vertex == entity->vertices.end ()) {
                error = "TerrainControlOrphaned: an enabled fixed vertex was deleted";
                return false;
            }
            candidate.points.insert (candidate.points.end (), { vertex->p.x, vertex->p.y, vertex->p.z });
            candidate.pointSupport.push_back ({ control.source, 0, { vertex->id, 0 } });
            continue;
        }
        if (entity->closed || entity->vertices.size () < 2 ||
            std::any_of (
                entity->vertices.begin (), entity->vertices.end (),
                [] (const auto& v) { return v.radius != 0 || v.in != edit::Vec3 {} || v.out != edit::Vec3 {}; }) ||
            std::any_of (source->arcAngles.begin (), source->arcAngles.end (), [] (double a) { return a != 0; })) {
            error = "TerrainControlUnsupported: fixed lines require an open straight polyline without arcs or handles";
            return false;
        }
        std::vector<double> line;
        std::vector<TerrainDesignControlRef> support;
        for (const auto& vertex : entity->vertices)
            line.insert (line.end (), { vertex.p.x, vertex.p.y, vertex.p.z });
        for (size_t i = 0; i + 1 < entity->vertices.size (); ++i)
            support.push_back (
                { control.source, entity->segments.at (i).id, { entity->vertices[i].id, entity->vertices[i + 1].id } });
        candidate.lines.push_back (std::move (line));
        candidate.lineSupport.push_back (std::move (support));
    }
    result = std::move (candidate);
    return true;
}

bool AppendTerrainControls (const TerrainControls& controls, const TerrainDesignOptions& options,
                            const IdentityObject& owner, ProvenanceRequest& combined, std::string& error)
{
    const auto append = [&] (IdentityDomain domain, Id source) {
        EntityMapping row;
        row.kind = MappingKind::Generated;
        row.domain = domain;
        row.outputIndex = domain == IdentityDomain::Vertex ? combined.counts.vertices++ : combined.counts.edges++;
        row.sources = Append (combined.table.sourceIds, { source });
        row.operations = Append (combined.table.operationIds, { owner.operation, options.operation });
        row.object = options.object;
        combined.table.mappings.push_back (row);
    };
    for (size_t line = 0; line < controls.lines.size (); ++line) {
        const auto source = controls.lineSupport.at (line).at (0).source;
        for (size_t v = 0; v < controls.lines[line].size () / 3; ++v)
            append (IdentityDomain::Vertex, source);
        for (const auto& segment : controls.lineSupport.at (line))
            append (IdentityDomain::Edge, segment.source);
    }
    for (const auto& point : controls.pointSupport)
        append (IdentityDomain::Vertex, point.source);
    return Validate (combined, error);
}

TerrainDesignControlRef ResolveTerrainControl (const TerrainControls& controls, const HybridControlRef& ref)
{
    if (ref.kind == HybridControlRef::Kind::Point && ref.segment == 0)
        return controls.pointSupport.at (ref.index);
    if (ref.kind == HybridControlRef::Kind::Segment)
        return controls.lineSupport.at (ref.index).at (ref.segment);
    throw std::runtime_error ("unknown fixed-control locator");
}
} // namespace tapioca::geometry::terrain_detail
