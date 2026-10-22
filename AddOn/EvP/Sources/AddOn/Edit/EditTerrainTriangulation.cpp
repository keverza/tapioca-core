#include "Edit/EditTerrainTriangulation.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditLandscape.hpp"
#include "Edit/EditTerrainAdapterInternal.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <exception>
#include <map>
#include <set>
#include <stdexcept>

namespace tapioca::geometry {
namespace {
bool Validate (const ProvenanceRequest& provenance, std::string& error)
{
    ProvenanceDiagnostic diagnostic = ProvenanceDiagnostic::None;
    return ValidateProvenance (provenance, diagnostic, error) && diagnostic == ProvenanceDiagnostic::None;
}

IdentityRange Append (std::vector<uint64_t>& pool, const std::set<uint64_t>& values)
{
    const IdentityRange range { pool.size (), values.size () };
    pool.insert (pool.end (), values.begin (), values.end ());
    return range;
}

EntityMapping Supported (const EntityMapping& input, IdentityDomain domain, uint64_t index, uint64_t table,
                         ProvenanceTable& output)
{
    auto row = input;
    row.domain = domain;
    row.outputIndex = index;
    row.kind = MappingKind::Exact;
    row.predecessors = { output.predecessors.size (), 1 };
    output.predecessors.push_back ({ table, input.domain, input.outputIndex });
    return row;
}

struct InputRows {
    std::vector<const EntityMapping*> vertices, edges, faces;
    explicit InputRows (const ProvenanceRequest& input)
        : vertices (input.counts.vertices), edges (input.counts.edges), faces (input.counts.faces)
    {
        for (const auto& row : input.table.mappings) {
            if (row.domain == IdentityDomain::Vertex)
                vertices.at (row.outputIndex) = &row;
            if (row.domain == IdentityDomain::Face)
                faces.at (row.outputIndex) = &row;
            if (row.domain == IdentityDomain::Edge)
                edges.at (row.outputIndex) = &row;
        }
    }
};

bool Prepare (const geomsrv::edit::Document& doc, const EditTerrainInput& input, const EditTerrainSplitOptions& options,
              LandscapeResult& landscape, ProvenanceRequest& combined, EditTerrainSplitResult& candidate,
              std::string& error)
{
    if (!terrain_detail::ValidateInput (doc, input, error) ||
        !terrain_detail::ValidateOperations (doc, input.provenance, std::array<OperationId, 1> { options.operation },
                                             error))
        return false;
    std::set<uint64_t> tokens { input.provenance.table.context.tableId };
    for (const auto& support : input.provenance.table.supportTables)
        tokens.insert (support.context.tableId);
    if (options.splitTable == 0 || options.meshTable == 0 || !tokens.insert (options.splitTable).second ||
        !tokens.insert (options.meshTable).second) {
        error = "ProvenanceConflict: split and mesh table tokens must be new and distinct";
        return false;
    }
    const auto owner = std::find_if (input.provenance.table.objects.begin (), input.provenance.table.objects.end (),
                                     [&] (const auto& object) { return object.id == options.object; });
    if (!input.provenance.requireObject || options.object == 0 || owner == input.provenance.table.objects.end () ||
        std::any_of (input.provenance.table.mappings.begin (), input.provenance.table.mappings.end (),
                     [&] (const auto& row) { return row.object != options.object; })) {
        error = "ProvenanceConflict: property split requires one existing, uniformly attributed terrain owner";
        return false;
    }
    const auto operation = std::find_if (doc.landscape->operations.begin (), doc.landscape->operations.end (),
                                         [&] (const auto& op) { return op.id == options.operation; });
    const auto operationRow =
        std::find_if (input.provenance.table.operations.begin (), input.provenance.table.operations.end (),
                      [&] (const auto& op) { return op.id == options.operation; });
    std::vector<Id> parents;
    for (Id id : operation->inputs)
        if (std::any_of (doc.landscape->operations.begin (), doc.landscape->operations.end (),
                         [=] (const auto& op) { return op.id == id; }))
            parents.push_back (id);
    if (parents.size () != operationRow->parents.count ||
        !std::equal (parents.begin (), parents.end (),
                     input.provenance.table.operationIds.begin () + operationRow->parents.offset)) {
        error = "ProvenanceStale: split operation parents differ from the current document";
        return false;
    }
    if (!EvaluateLandscape (doc, landscape)) {
        error = landscape.error;
        return false;
    }
    if (landscape.propertyLine.empty ()) {
        error = "PropertyLineOutside: property split requires a property line";
        return false;
    }
    const Id property = doc.landscape->propertyLineSourceId;
    const auto source = std::find_if (doc.landscape->sources.begin (), doc.landscape->sources.end (),
                                      [=] (const auto& value) { return value.id == property; });
    const auto sourceRow = std::find_if (input.provenance.table.sources.begin (), input.provenance.table.sources.end (),
                                         [=] (const auto& value) { return value.id == property; });
    if (sourceRow == input.provenance.table.sources.end ()) {
        error = "ProvenanceUnattributable: property guide is absent from the terrain catalog";
        return false;
    }
    if (!source->archicadGuid.empty ()) {
        bool bound = false;
        for (uint64_t i = 0; i < sourceRow->bindings.count; ++i) {
            const auto& table = input.provenance.table;
            const auto& binding = table.bindings.at (table.bindingRefs.at (sourceRow->bindings.offset + i));
            bound =
                bound || (binding.logicalKind == LogicalKind::Source && binding.logicalId == property &&
                          std::string (binding.nativeGuid.begin (), binding.nativeGuid.end ()) == source->archicadGuid);
        }
        if (!bound) {
            error = "ProvenanceUnattributable: property guide native binding is absent";
            return false;
        }
    }
    const auto* entity = geomsrv::edit::Find (doc, source->entityId);
    if (!entity || entity->vertices.size () != landscape.propertyLine.size () ||
        entity->segments.size () != landscape.propertyLine.size ()) {
        error = "ProvenanceUnattributable: straight property vertices/segments do not match the document";
        return false;
    }
    for (const auto& vertex : entity->vertices)
        candidate.propertyVertices.push_back (vertex.id);
    for (const auto& segment : entity->segments)
        candidate.propertySegments.push_back (segment.id);
    combined = input.provenance;
    combined.table.context.tableId = options.splitTable;
    combined.domains = 7;
    combined.counts.vertices += landscape.propertyLine.size ();
    combined.counts.edges = landscape.propertyLine.size ();
    combined.table.supportTables.push_back ({ input.provenance.table.context, input.provenance.counts, false });
    combined.table.mappings.clear ();
    for (const auto& row : input.provenance.table.mappings)
        combined.table.mappings.push_back (
            Supported (row, row.domain, row.outputIndex, input.provenance.table.context.tableId, combined.table));
    EntityMapping guide;
    guide.kind = MappingKind::Generated;
    guide.object = options.object;
    guide.sources = Append (combined.table.sourceIds, { property });
    guide.operations = Append (combined.table.operationIds, { owner->operation, options.operation });
    for (size_t i = 0; i < landscape.propertyLine.size (); ++i) {
        guide.domain = IdentityDomain::Vertex;
        guide.outputIndex = input.provenance.counts.vertices + i;
        combined.table.mappings.push_back (guide);
        guide.domain = IdentityDomain::Edge;
        guide.outputIndex = i;
        combined.table.mappings.push_back (guide);
    }
    return Validate (combined, error);
}

EntityMapping Fragment (const ProvenanceRequest& input, std::span<const EntityMapping* const> parents,
                        IdentityDomain domain, uint64_t index, OperationId operation, SourceId guide,
                        std::span<const EntityPredecessor> predecessors, ProvenanceRequest& output)
{
    EntityMapping row;
    row.domain = domain;
    row.outputIndex = index;
    row.kind = MappingKind::Split;
    row.object = parents.front ()->object;
    // A face fragment retains its one known patch membership. Interpolated
    // vertices need multiple parents; patch matching is not guessed there.
    row.patchKey = parents.size () == 1 ? parents.front ()->patchKey : 0;
    std::set<uint64_t> sources { guide }, operations { operation };
    for (const auto* parent : parents) {
        for (uint64_t i = 0; i < parent->sources.count; ++i)
            sources.insert (input.table.sourceIds.at (parent->sources.offset + i));
        for (uint64_t i = 0; i < parent->operations.count; ++i)
            operations.insert (input.table.operationIds.at (parent->operations.offset + i));
    }
    row.sources = Append (output.table.sourceIds, sources);
    row.operations = Append (output.table.operationIds, operations);
    row.predecessors = { output.table.predecessors.size (), predecessors.size () };
    output.table.predecessors.insert (output.table.predecessors.end (), predecessors.begin (), predecessors.end ());
    return row;
}

bool Remap (const EditTerrainInput& input, const EditTerrainSplitOptions& options, Id property,
            EditTerrainSplitResult& candidate, std::string& error)
{
    const auto& split = candidate.split;
    const auto& combined = split.inputProvenance;
    const size_t vertices = split.points.size () / 3, faces = split.triangles.size () / 3;
    if (split.points.size () % 3 != 0 || split.triangles.size () % 3 != 0 || vertices < input.mesh.points.size () / 3 ||
        !std::equal (
            input.mesh.points.begin (), input.mesh.points.end (), split.points.begin (),
            [] (double a, double b) { return std::bit_cast<uint64_t> (a) == std::bit_cast<uint64_t> (b); })) {
        error = "TerrainOutputShape: split did not retain its original vertex prefix";
        return false;
    }
    InputRows rows (combined);
    std::vector<const TerrainMapping*> newVertices (vertices), faceRows (faces);
    std::vector<uint32_t> parentCount (input.provenance.counts.faces);
    std::vector<bool> lineVertices (candidate.propertyVertices.size ());
    std::vector<uint32_t> lineOutputs (lineVertices.size ());
    for (const auto& mapping : split.mappings) {
        if (mapping.kind == TerrainMappingKind::SplitFace) {
            if (faceRows.at (mapping.index) || mapping.values[1] > 1)
                throw std::runtime_error ("duplicate/invalid split face support");
            faceRows.at (mapping.index) = &mapping;
            ++parentCount.at (mapping.values[0]);
        }
        else if (mapping.kind == TerrainMappingKind::SplitVertex) {
            if (mapping.index < input.provenance.counts.vertices || newVertices.at (mapping.index))
                throw std::runtime_error ("duplicate/invalid new vertex support");
            newVertices.at (mapping.index) = &mapping;
        }
        else if (mapping.kind == TerrainMappingKind::LineVertex) {
            if (lineVertices.at (mapping.index) || mapping.values[0] >= vertices)
                throw std::runtime_error ("duplicate/invalid property vertex support");
            lineVertices.at (mapping.index) = true;
            lineOutputs.at (mapping.index) = mapping.values[0];
        }
        else if (mapping.kind == TerrainMappingKind::LineEdge) {
            if (mapping.index >= candidate.propertySegments.size () || mapping.values[0] >= vertices ||
                mapping.values[1] >= vertices || mapping.values[0] == mapping.values[1])
                throw std::runtime_error ("invalid property edge support");
            candidate.propertyEdges.push_back ({ mapping.values[0], mapping.values[1], mapping.index });
        }
        else
            throw std::runtime_error ("unexpected split support kind");
    }
    if (std::any_of (lineVertices.begin (), lineVertices.end (), [] (bool present) { return !present; }) ||
        std::any_of (parentCount.begin (), parentCount.end (), [] (uint32_t count) { return count == 0; })) {
        error = "ProvenanceUnattributable: split has incomplete line/parent coverage";
        return false;
    }
    std::set<std::pair<uint32_t, uint32_t>> meshEdges;
    for (size_t i = 0; i < split.triangles.size (); i += 3)
        for (size_t j = 0; j < 3; ++j) {
            const auto a = split.triangles.at (i + j), b = split.triangles.at (i + (j + 1) % 3);
            if (a >= vertices || b >= vertices)
                throw std::runtime_error ("invalid output triangle index");
            meshEdges.emplace (std::min (a, b), std::max (a, b));
        }
    auto tails = lineOutputs;
    std::vector<size_t> edgeCounts (tails.size ());
    for (const auto& edge : candidate.propertyEdges) {
        if (tails.at (edge.segment) != edge.a ||
            !meshEdges.count ({ std::min (edge.a, edge.b), std::max (edge.a, edge.b) }))
            throw std::runtime_error ("property support is not a connected path of output mesh edges");
        tails.at (edge.segment) = edge.b;
        ++edgeCounts.at (edge.segment);
    }
    for (size_t i = 0; i < tails.size (); ++i)
        if (edgeCounts[i] == 0 || tails[i] != lineOutputs[(i + 1) % tails.size ()])
            throw std::runtime_error ("incomplete property segment path");
    auto& output = candidate.meshProvenance;
    output = combined;
    output.table.context.tableId = options.meshTable;
    output.domains = kMeshProvenanceDomains;
    output.counts = { vertices, 0, faces, 0 };
    output.table.supportTables.push_back ({ combined.table.context, combined.counts, false });
    output.table.mappings.clear ();
    for (size_t i = 0; i < vertices; ++i) {
        if (i < input.provenance.counts.vertices) {
            output.table.mappings.push_back (
                Supported (*rows.vertices.at (i), IdentityDomain::Vertex, i, options.splitTable, output.table));
            continue;
        }
        const auto* mapping = newVertices.at (i);
        if (!mapping)
            throw std::runtime_error ("missing new vertex support");
        const auto& v = mapping->values;
        std::vector<const EntityMapping*> parents;
        std::vector<EntityPredecessor> support;
        if (v[2] == 1) {
            if (v[3] >= input.provenance.counts.vertices || v[4] >= input.provenance.counts.vertices || v[3] == v[4])
                throw std::runtime_error ("invalid original mesh edge host");
            parents = { rows.vertices.at (v[3]), rows.vertices.at (v[4]) };
            support = { { options.splitTable, IdentityDomain::Vertex, v[3] },
                        { options.splitTable, IdentityDomain::Vertex, v[4] } };
        }
        else if (v[2] == 2) {
            parents = { rows.faces.at (v[3]) };
            support = { { options.splitTable, IdentityDomain::Face, v[3] } };
        }
        else
            throw std::runtime_error ("unknown new vertex host");
        if (v[1] >= candidate.propertyVertices.size () || (v[0] != 1 && v[0] != 2))
            throw std::runtime_error ("unknown new vertex origin");
        const auto& guide =
            v[0] == 1 ? *rows.vertices.at (input.provenance.counts.vertices + v[1]) : *rows.edges.at (v[1]);
        parents.push_back (&guide);
        support.push_back ({ options.splitTable, guide.domain, guide.outputIndex });
        output.table.mappings.push_back (
            Fragment (combined, parents, IdentityDomain::Vertex, i, options.operation, property, support, output));
    }
    for (size_t i = 0; i < faces; ++i) {
        const auto* mapping = faceRows.at (i);
        if (!mapping)
            throw std::runtime_error ("missing face support");
        const uint32_t parent = mapping->values[0];
        candidate.parents.push_back (parent);
        candidate.inside.push_back (static_cast<uint8_t> (mapping->values[1]));
        const auto* row = rows.faces.at (parent);
        const bool exact = parentCount.at (parent) == 1 &&
                           std::equal (split.triangles.begin () + i * 3, split.triangles.begin () + i * 3 + 3,
                                       input.mesh.triangles.begin () + parent * 3);
        if (exact)
            output.table.mappings.push_back (
                Supported (*row, IdentityDomain::Face, i, options.splitTable, output.table));
        else {
            const std::array<const EntityMapping*, 1> parents { row };
            const std::array<EntityPredecessor, 1> support { EntityPredecessor { options.splitTable,
                                                                                 IdentityDomain::Face, parent } };
            output.table.mappings.push_back (
                Fragment (combined, parents, IdentityDomain::Face, i, options.operation, property, support, output));
        }
    }
    return Validate (output, error);
}
} // namespace

bool SplitEditTerrain (const geomsrv::edit::Document& doc, const EditTerrainInput& input,
                       const EditTerrainSplitOptions& options, EditTerrainSplitResult& result, std::string& error)
{
    try {
        EditTerrainSplitResult candidate;
        LandscapeResult landscape;
        ProvenanceRequest combined;
        if (!Prepare (doc, input, options, landscape, combined, candidate, error))
            return false;
        std::vector<double> line;
        for (const auto& point : landscape.propertyLine)
            line.insert (line.end (), { point.x, point.y, point.z });
        if (!SplitTerrainMesh ({ input.mesh.points, input.mesh.triangles, line, {}, combined }, candidate.split,
                               error) ||
            !Remap (input, options, doc.landscape->propertyLineSourceId, candidate, error))
            return false;
        candidate.revision = input.revision;
        candidate.sourceTerrainGuid = input.sourceTerrainGuid;
        candidate.baselineProvenance = input.provenance;
        candidate.baselineTriangles.assign (input.mesh.triangles.begin (), input.mesh.triangles.end ());
        result = std::move (candidate);
        error.clear ();
        return true;
    }
    catch (const std::exception& exception) {
        error = std::string ("TerrainSplitRefused: ") + exception.what ();
        return false;
    }
}

bool VerifyEditTerrainOutside (const EditTerrainSplitResult& baseline, const EditTerrainResult& proposed,
                               EditTerrainPreservation& report, std::string& error)
{
    const auto& split = baseline.split;
    if (proposed.revision != baseline.revision || proposed.sourceTerrainGuid != baseline.sourceTerrainGuid ||
        proposed.inputProvenance != baseline.meshProvenance) {
        error = "ProvenanceStale: outside verification requires this exact split baseline";
        return false;
    }
    if (proposed.points.size () != split.points.size () || proposed.triangles != split.triangles ||
        baseline.inside.size () != split.triangles.size () / 3 || split.points.size () % 3 != 0 ||
        baseline.parents.size () != baseline.inside.size () || baseline.propertyVertices.size () < 3 ||
        baseline.propertySegments.size () != baseline.propertyVertices.size () || baseline.propertyEdges.empty () ||
        baseline.meshProvenance.table.context.tableId == 0 ||
        baseline.meshProvenance.domains != kMeshProvenanceDomains ||
        baseline.meshProvenance.counts !=
            EntityCounts { split.points.size () / 3, 0, split.triangles.size () / 3, 0 } ||
        std::any_of (baseline.inside.begin (), baseline.inside.end (), [] (uint8_t inside) { return inside > 1; })) {
        error = "TerrainOutputShape: outside verification requires unchanged split topology";
        return false;
    }
    try {
        std::vector<bool> fixed (split.points.size () / 3);
        EditTerrainPreservation candidate;
        for (size_t i = 0; i < baseline.inside.size (); ++i)
            if (!baseline.inside[i]) {
                ++candidate.outsideFaces;
                for (size_t j = 0; j < 3; ++j)
                    fixed.at (split.triangles.at (i * 3 + j)) = true;
            }
        for (const auto& edge : baseline.propertyEdges) {
            fixed.at (edge.a) = true;
            fixed.at (edge.b) = true;
        }
        for (size_t i = 0; i < proposed.points.size (); ++i) {
            const bool same = std::bit_cast<uint64_t> (proposed.points[i]) == std::bit_cast<uint64_t> (split.points[i]);
            if (!std::isfinite (proposed.points[i]) || ((i % 3 != 2 || fixed.at (i / 3)) && !same)) {
                error =
                    "PropertyLineOutside: proposed terrain moved plan coordinates or a fixed outside/boundary vertex";
                return false;
            }
        }
        candidate.fixedVertices = std::count (fixed.begin (), fixed.end (), true);
        candidate.propertyEdges = baseline.propertyEdges.size ();
        report = candidate;
        error.clear ();
        return true;
    }
    catch (const std::exception& exception) {
        error = std::string ("TerrainSplitRefused: ") + exception.what ();
        return false;
    }
}
} // namespace tapioca::geometry
