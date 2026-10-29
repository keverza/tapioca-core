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
namespace terrain_detail {
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

bool NewTables (const ProvenanceRequest& input, uint64_t first, uint64_t second)
{
    std::set<uint64_t> tokens { input.table.context.tableId };
    for (const auto& support : input.table.supportTables)
        tokens.insert (support.context.tableId);
    return first != 0 && second != 0 && tokens.insert (first).second && tokens.insert (second).second;
}

const IdentityObject* UniformOwner (const ProvenanceRequest& input, ObjectId object)
{
    const auto owner = std::find_if (input.table.objects.begin (), input.table.objects.end (),
                                     [&] (const auto& value) { return value.id == object; });
    if (!input.requireObject || object == 0 || owner == input.table.objects.end () ||
        std::any_of (input.table.mappings.begin (), input.table.mappings.end (),
                     [&] (const auto& row) { return row.object != object; }))
        return nullptr;
    return &*owner;
}

bool CurrentParents (const geomsrv::edit::Document& doc, const ProvenanceRequest& input, OperationId operation)
{
    const auto found = std::find_if (doc.landscape->operations.begin (), doc.landscape->operations.end (),
                                     [&] (const auto& op) { return op.id == operation; });
    const auto row = std::find_if (input.table.operations.begin (), input.table.operations.end (),
                                   [&] (const auto& op) { return op.id == operation; });
    std::vector<Id> parents;
    for (Id id : found->inputs)
        if (std::any_of (doc.landscape->operations.begin (), doc.landscape->operations.end (),
                         [=] (const auto& op) { return op.id == id; }))
            parents.push_back (id);
    return parents.size () == row->parents.count &&
           std::equal (parents.begin (), parents.end (), input.table.operationIds.begin () + row->parents.offset);
}

bool CatalogSource (const geomsrv::edit::Document& doc, const ProvenanceRequest& input, SourceId id, const char* what,
                    std::string& error)
{
    const auto source = std::find_if (doc.landscape->sources.begin (), doc.landscape->sources.end (),
                                      [=] (const auto& value) { return value.id == id; });
    const auto row = std::find_if (input.table.sources.begin (), input.table.sources.end (),
                                   [=] (const auto& value) { return value.id == id; });
    if (source == doc.landscape->sources.end () || row == input.table.sources.end ()) {
        error = std::string ("ProvenanceUnattributable: ") + what + " is absent from the terrain catalog";
        return false;
    }
    if (!source->archicadGuid.empty ()) {
        bool bound = false;
        for (uint64_t i = 0; i < row->bindings.count; ++i) {
            const auto& table = input.table;
            const auto& binding = table.bindings.at (table.bindingRefs.at (row->bindings.offset + i));
            bound =
                bound || (binding.logicalKind == LogicalKind::Source && binding.logicalId == id &&
                          std::string (binding.nativeGuid.begin (), binding.nativeGuid.end ()) == source->archicadGuid);
        }
        if (!bound) {
            error = std::string ("ProvenanceUnattributable: ") + what + " native binding is absent";
            return false;
        }
    }
    return true;
}

EntityMapping Fragment (const ProvenanceRequest& input, std::span<const EntityMapping* const> parents,
                        IdentityDomain domain, uint64_t index, OperationId operation, std::span<const SourceId> guides,
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
    std::set<uint64_t> sources (guides.begin (), guides.end ());
    std::set<uint64_t> operations { operation };
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
EntityMapping Fragment (const ProvenanceRequest& input, std::span<const EntityMapping* const> parents,
                        IdentityDomain domain, uint64_t index, OperationId operation, SourceId guide,
                        std::span<const EntityPredecessor> predecessors, ProvenanceRequest& output)
{
    return Fragment (input, parents, domain, index, operation, std::array<SourceId, 1> { guide }, predecessors, output);
}
} // namespace terrain_detail

namespace {
using namespace terrain_detail;

bool Prepare (const geomsrv::edit::Document& doc, const EditTerrainInput& input, const EditTerrainSplitOptions& options,
              LandscapeResult& landscape, ProvenanceRequest& combined, EditTerrainSplitResult& candidate,
              std::string& error)
{
    if (!terrain_detail::ValidateInput (doc, input, error) ||
        !terrain_detail::ValidateOperations (doc, input.provenance, std::array<OperationId, 1> { options.operation },
                                             error))
        return false;
    if (!NewTables (input.provenance, options.splitTable, options.meshTable)) {
        error = "ProvenanceConflict: split and mesh table tokens must be new and distinct";
        return false;
    }
    const auto* owner = UniformOwner (input.provenance, options.object);
    if (!owner) {
        error = "ProvenanceConflict: property split requires one existing, uniformly attributed terrain owner";
        return false;
    }
    if (!CurrentParents (doc, input.provenance, options.operation)) {
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
    if (!CatalogSource (doc, input.provenance, property, "property guide", error))
        return false;
    const auto source = std::find_if (doc.landscape->sources.begin (), doc.landscape->sources.end (),
                                      [=] (const auto& value) { return value.id == property; });
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

bool Remap (const EditTerrainInput& input, const EditTerrainSplitOptions& options, std::span<const SourceId> sources,
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
    const size_t lineCount =
        candidate.derivedRingVertices ? candidate.derivedRingVertices : candidate.propertyVertices.size ();
    std::vector<bool> lineVertices (lineCount);
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
            if (mapping.index >= lineCount || mapping.values[0] >= vertices || mapping.values[1] >= vertices ||
                mapping.values[0] == mapping.values[1])
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
        if (v[1] >= lineCount || (v[0] != 1 && v[0] != 2))
            throw std::runtime_error ("unknown new vertex origin");
        const auto& guide =
            v[0] == 1 ? *rows.vertices.at (input.provenance.counts.vertices + v[1]) : *rows.edges.at (v[1]);
        parents.push_back (&guide);
        support.push_back ({ options.splitTable, guide.domain, guide.outputIndex });
        output.table.mappings.push_back (
            Fragment (combined, parents, IdentityDomain::Vertex, i, options.operation, sources, support, output));
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
                Fragment (combined, parents, IdentityDomain::Face, i, options.operation, sources, support, output));
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
            !Remap (input, options, std::array<SourceId, 1> { doc.landscape->propertyLineSourceId }, candidate, error))
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

bool SplitEditTerrainRing (const geomsrv::edit::Document& doc, const EditTerrainInput& input,
                           const EditTerrainSplitOptions& options, std::span<const Vec3> ring,
                           std::span<const SourceId> sources, EditTerrainSplitResult& result, std::string& error)
{
    try {
        if (ring.size () < 3 || ring.size () > 65536 || sources.empty () ||
            !terrain_detail::ValidateInput (doc, input, error) ||
            !terrain_detail::ValidateOperations (doc, input.provenance,
                                                 std::array<OperationId, 1> { options.operation }, error)) {
            if (error.empty ())
                error = "TerrainRingShape: bounded closed ring and source contributors required";
            return false;
        }
        const auto* owner = UniformOwner (input.provenance, options.object);
        if (!NewTables (input.provenance, options.splitTable, options.meshTable) || !owner ||
            !CurrentParents (doc, input.provenance, options.operation)) {
            error = "ProvenanceConflict: derived split needs fresh tables and its current uniform owner/operation";
            return false;
        }
        for (const auto source : sources)
            if (!CatalogSource (doc, input.provenance, source, "derived ring", error))
                return false;
        auto combined = input.provenance;
        combined.table.context.tableId = options.splitTable;
        combined.domains = 7;
        combined.counts.vertices += ring.size ();
        combined.counts.edges = ring.size ();
        combined.table.supportTables.push_back ({ input.provenance.table.context, input.provenance.counts, false });
        combined.table.mappings.clear ();
        for (const auto& row : input.provenance.table.mappings)
            combined.table.mappings.push_back (
                Supported (row, row.domain, row.outputIndex, input.provenance.table.context.tableId, combined.table));
        EntityMapping guide;
        guide.kind = MappingKind::Generated;
        guide.object = options.object;
        guide.sources = Append (combined.table.sourceIds, std::set<uint64_t> (sources.begin (), sources.end ()));
        // The constraint edits this terrain owner even when its boundary comes
        // from an independent object. Retain both contributors, as property
        // preparation does; never fabricate a dependency between their DAGs.
        guide.operations = Append (combined.table.operationIds, { owner->operation, options.operation });
        std::vector<double> line;
        for (size_t i = 0; i < ring.size (); ++i) {
            line.insert (line.end (), { ring[i].x, ring[i].y, ring[i].z });
            guide.domain = IdentityDomain::Vertex;
            guide.outputIndex = input.provenance.counts.vertices + i;
            combined.table.mappings.push_back (guide);
            guide.domain = IdentityDomain::Edge;
            guide.outputIndex = i;
            combined.table.mappings.push_back (guide);
        }
        EditTerrainSplitResult candidate;
        candidate.derivedRingVertices = ring.size ();
        if (!terrain_detail::Validate (combined, error) ||
            !SplitTerrainMesh ({ input.mesh.points, input.mesh.triangles, line, {}, combined }, candidate.split,
                               error) ||
            !Remap (input, options, sources, candidate, error))
            return false;
        candidate.revision = input.revision;
        candidate.sourceTerrainGuid = input.sourceTerrainGuid;
        candidate.baselineProvenance = input.provenance;
        result = std::move (candidate);
        error.clear ();
        return true;
    }
    catch (const std::exception& exception) {
        error = std::string ("TerrainRingSplit: ") + exception.what ();
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
