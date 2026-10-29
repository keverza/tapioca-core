#include "Edit/EditLandscapeTerrain.hpp"
#include "Edit/EditLandscapeTerrainInternal.hpp"
#include "Edit/EditTerrainAdapterInternal.hpp"
#include <algorithm>
#include <exception>
#include <map>
#include <set>

namespace tapioca::geometry {
namespace {
using namespace landscape_detail;
using Scope = std::set<Id>;

bool ConstraintScopes (const LandscapeSession& session, const LandscapeResult& landscape, std::map<Id, Scope>& scopes,
                       std::string& error)
{
    std::vector<Id> order;
    if (!ValidateLandscapeDag (session, order, error))
        return false;
    std::set<Id> active, nonuniform;
    for (const auto& constraint : landscape.terrainConstraints) {
        active.insert (constraint.operation);
        if (!constraint.uniformScope)
            nonuniform.insert (constraint.operation);
    }
    std::map<Id, const LandscapeOperation*> operations;
    for (const auto& operation : session.operations)
        operations.emplace (operation.id, &operation);
    // Mixed child scopes cannot be represented by the shipped global-mask edit
    // request. Propagate that fact, but refuse only if the caller edits this branch.
    std::set<Id> mixed;
    for (Id id : order) {
        const auto& operation = *operations.at (id);
        Scope scope;
        if (!operation.inputs.empty ()) {
            scope = scopes[operation.inputs.front ()];
            if (mixed.count (operation.inputs.front ()))
                mixed.insert (id);
            const bool firstOnly = !operation.enabled || operation.kind == OperationKind::MaterialMask ||
                                   operation.kind == OperationKind::GradingLimit ||
                                   operation.kind == OperationKind::ProtectedZone ||
                                   operation.kind == OperationKind::Split;
            if (!firstOnly)
                for (size_t i = 1; i < operation.inputs.size (); ++i)
                    if (mixed.count (operation.inputs[i]) || scopes[operation.inputs[i]] != scope)
                        mixed.insert (id);
        }
        if (active.count (id))
            scope.insert (id);
        if (nonuniform.count (id))
            mixed.insert (id);
        scopes[id] = std::move (scope);
    }
    // The sentinel is request-local and is never exposed as authored identity.
    for (Id id : mixed)
        scopes[id].insert (kNoId);
    return true;
}

std::vector<double> Flatten (const std::vector<Vec3>& ring)
{
    std::vector<double> result;
    result.reserve (ring.size () * 3);
    for (const auto& point : ring)
        result.insert (result.end (), { point.x, point.y, point.z });
    return result;
}

bool MaskBudget (const cp::PathsD& paths, std::string& error)
{
    size_t count = 0;
    const size_t limit = PlanarOptions {}.maxSegments;
    for (const auto& path : paths) {
        if (path.size () > limit - count) {
            error = "TerrainMaskWorkLimit: derived masks exceed the planar segment budget";
            return false;
        }
        count += path.size ();
    }
    return true;
}

bool ValidateCatalog (const geomsrv::edit::Document& doc, const ProvenanceRequest& provenance,
                      const std::map<Id, Shape>& footprints, std::span<const EditTerrainOperation> edits,
                      const Scope& scope, const LandscapeResult& landscape, std::string& error)
{
    std::set<Id> sources, operations;
    const auto include = [&] (const Shape& shape) {
        for (const auto& region : shape) {
            sources.insert (region.sources.begin (), region.sources.end ());
            operations.insert (region.operations.begin (), region.operations.end ());
        }
    };
    if (doc.landscape->propertyLineSourceId)
        sources.insert (doc.landscape->propertyLineSourceId);
    for (const auto& edit : edits)
        include (footprints.at (edit.operation));
    for (const auto& constraint : landscape.terrainConstraints)
        if (scope.count (constraint.operation)) {
            operations.insert (constraint.operation);
            include (constraint.masks);
        }
    std::map<SourceId, const IdentitySource*> sourceRows;
    for (const auto& source : provenance.table.sources)
        sourceRows.emplace (source.id, &source);
    for (Id id : sources) {
        const auto row = sourceRows.find (id);
        if (row == sourceRows.end ()) {
            error = "ProvenanceUnattributable: terrain input catalog omits an evaluated guide source";
            return false;
        }
        const auto source = std::find_if (doc.landscape->sources.begin (), doc.landscape->sources.end (),
                                          [=] (const auto& value) { return value.id == id; });
        if (source == doc.landscape->sources.end () || source->archicadGuid.empty ())
            continue;
        bool bound = false;
        const auto range = row->second->bindings;
        for (uint64_t i = 0; i < range.count; ++i) {
            const auto& binding = provenance.table.bindings.at (provenance.table.bindingRefs.at (range.offset + i));
            bound =
                bound || (binding.logicalKind == LogicalKind::Source && binding.logicalId == id &&
                          std::string (binding.nativeGuid.begin (), binding.nativeGuid.end ()) == source->archicadGuid);
        }
        if (!bound) {
            error = "ProvenanceUnattributable: terrain input catalog omits a native guide binding";
            return false;
        }
    }
    std::map<OperationId, const IdentityOperation*> operationRows;
    for (const auto& operation : provenance.table.operations)
        operationRows.emplace (operation.id, &operation);
    for (Id id : operations) {
        const auto row = operationRows.find (id);
        if (row == operationRows.end ()) {
            error = "ProvenanceUnattributable: terrain input catalog omits an evaluated operation";
            return false;
        }
        const auto operation = std::find_if (doc.landscape->operations.begin (), doc.landscape->operations.end (),
                                             [=] (const auto& value) { return value.id == id; });
        std::vector<Id> parents;
        if (operation != doc.landscape->operations.end ())
            for (Id input : operation->inputs)
                if (std::any_of (doc.landscape->operations.begin (), doc.landscape->operations.end (),
                                 [=] (const auto& value) { return value.id == input; }))
                    parents.push_back (input);
        const auto range = row->second->parents;
        if (parents.size () != range.count ||
            !std::equal (parents.begin (), parents.end (), provenance.table.operationIds.begin () + range.offset)) {
            error = "ProvenanceStale: terrain catalog operation parents differ from the evaluated DAG";
            return false;
        }
    }
    return true;
}

bool ScopedMasks (const LandscapeResult& landscape, const Scope& scope, landscape_terrain_detail::ScopeMasks& masks,
                  std::vector<LandscapeConstraintHeights>& constraints, std::string& error)
{
    DerivedRegion propertyRegion;
    propertyRegion.rings = { landscape.propertyLine };
    const auto property = Paths (propertyRegion);
    cp::PathsD allowed = property, protectedPaths;
    bool envelope = false;
    size_t sampleCount = 0;
    for (const auto& constraint : landscape.terrainConstraints) {
        if (!scope.count (constraint.operation))
            continue;
        const auto authored = Paths (constraint.masks);
        if (!MaskBudget (authored, error))
            return false;
        const auto mask = cp::Intersect (authored, property, cp::FillRule::NonZero, kPrecision);
        if (!MaskBudget (mask, error))
            return false;
        LandscapeConstraintHeights report;
        report.operation = constraint.operation;
        report.kind = constraint.kind;
        for (const auto& ring : mask)
            for (const auto& point : ring)
                report.points.push_back ({ point.x, point.y, 0 });
        if (report.points.size () > PlanarOptions {}.maxSegments - sampleCount) {
            error = "TerrainMaskWorkLimit: constraint samples exceed the planar segment budget";
            return false;
        }
        sampleCount += report.points.size ();
        constraints.push_back (std::move (report));
        if (constraint.kind == OperationKind::GradingLimit) {
            allowed = cp::Intersect (allowed, mask, cp::FillRule::NonZero, kPrecision);
            envelope = true;
        }
        else
            protectedPaths.insert (protectedPaths.end (), mask.begin (), mask.end ());
        if (!MaskBudget (allowed, error) || !MaskBudget (protectedPaths, error))
            return false;
    }
    masks = { property, std::move (allowed), std::move (protectedPaths), envelope };
    return true;
}

bool BuildMasks (const LandscapeResult& landscape, const Scope& scope, std::vector<std::vector<double>>& protectedRings,
                 std::vector<LandscapeConstraintHeights>& constraints, std::string& error)
{
    landscape_terrain_detail::ScopeMasks masks;
    if (!ScopedMasks (landscape, scope, masks, constraints, error))
        return false;
    auto excluded = cp::Difference (masks.property, masks.allowed, cp::FillRule::NonZero, kPrecision);
    excluded.insert (excluded.end (), masks.protectedPaths.begin (), masks.protectedPaths.end ());
    excluded = cp::Union (excluded, cp::FillRule::NonZero, kPrecision);
    if (!MaskBudget (excluded, error))
        return false;
    if (excluded.empty ())
        return true;
    // The Rust positional-mask ABI accepts a union of simple rings, not holes.
    // Decompose only the 2D exclusion mask with the existing polygon service;
    // this is NOT a terrain TIN or a C++ terrain remesher.
    DerivedRegion maskRegion;
    for (const auto& ring : excluded) {
        std::vector<Vec3> points;
        for (const auto& point : ring)
            points.push_back ({ point.x, point.y, 0 });
        maskRegion.rings.push_back (std::move (points));
    }
    std::vector<Vec3> triangles;
    if (!LandscapePreviewTriangles (maskRegion, triangles, error)) {
        error = "TerrainMaskUnsupported: " + error;
        return false;
    }
    if (triangles.size () % 3 != 0) {
        error = "TerrainMaskUnsupported: polygon mask triangulation returned incomplete triangles";
        return false;
    }
    for (size_t i = 0; i < triangles.size (); i += 3)
        protectedRings.push_back (Flatten ({ triangles[i], triangles[i + 1], triangles[i + 2] }));
    return true;
}

bool Evaluate (const geomsrv::edit::Document& doc, const LandscapeTerrainRequest& request,
               LandscapeTerrainResult& candidate, std::string& error)
{
    if (!terrain_detail::ValidateInput (doc, request.input, error))
        return false;
    std::map<Id, Shape> footprints;
    if (!EvaluateLandscapeOperations (doc, candidate.landscape, &footprints)) {
        error = candidate.landscape.error;
        return false;
    }
    if (candidate.landscape.propertyLine.empty () && !request.fallbackArea.empty () &&
        (request.fallbackArea.size () < 9 || request.fallbackArea.size () % 3 != 0)) {
        error = "TerrainRegionShape: fallback terrain cover requires at least three XYZ points";
        return false;
    }
    if (candidate.landscape.propertyLine.empty ())
        for (size_t i = 0; i + 2 < request.fallbackArea.size (); i += 3)
            candidate.landscape.propertyLine.push_back (
                { request.fallbackArea[i], request.fallbackArea[i + 1], request.fallbackArea[i + 2] });
    std::vector<OperationId> operationIds;
    for (const auto& edit : request.edits)
        operationIds.push_back (edit.operation);
    if (!terrain_detail::ValidateOperations (doc, request.input.provenance, operationIds, error))
        return false;
    if (request.smoothing && request.edits.empty ()) {
        error = "ProvenanceUnattributable: landscape smoothing needs an attributed edit scope";
        return false;
    }
    std::map<Id, Scope> scopes;
    if (!ConstraintScopes (*doc.landscape, candidate.landscape, scopes, error))
        return false;
    Scope scope;
    std::vector<EditTerrainOperation> edits (request.edits.begin (), request.edits.end ());
    // Owned footprint buffers must be complete before publishing any borrowed views.
    std::vector<std::vector<double>> areas (edits.size ());
    size_t areaVertices = 0;
    for (size_t i = 0; i < edits.size (); ++i) {
        const auto& footprint = footprints.at (edits[i].operation);
        const auto& current = scopes.at (edits[i].operation);
        if (current.count (kNoId) || (i != 0 && current != scope)) {
            error = "TerrainConstraintScopeUnsupported: edits require one common, unmixed constraint scope";
            return false;
        }
        scope = current;
        if (footprint.empty ()) {
            error = "ClipEmpty: terrain edit operation has no evaluated footprint";
            return false;
        }
        if (edits[i].parameters.target == TerrainEditTarget::Area) {
            if (footprint.size () != 1 || footprint.front ().rings.size () != 1) {
                error = "TerrainFootprintUnsupported: area edit requires one hole-free operation footprint";
                return false;
            }
            if (footprint.front ().rings.front ().size () > PlanarOptions {}.maxSegments - areaVertices) {
                error = "TerrainFootprintWorkLimit: derived areas exceed the planar segment budget";
                return false;
            }
            areaVertices += footprint.front ().rings.front ().size ();
            areas[i] = Flatten (footprint.front ().rings.front ());
            edits[i].parameters.geometry = areas[i];
        }
    }
    if (!ValidateCatalog (doc, request.input.provenance, footprints, edits, scope, candidate.landscape, error))
        return false;
    std::vector<std::vector<double>> protectedRings;
    if (!BuildMasks (candidate.landscape, scope, protectedRings, candidate.constraints, error))
        return false;
    std::vector<std::span<const double>> protectedViews;
    for (const auto& ring : protectedRings)
        protectedViews.push_back (ring);
    std::vector<double> queries (request.queriesXY.begin (), request.queriesXY.end ());
    if (queries.size () % 2 != 0) {
        error = "TerrainQueryShape: terrain queries require XY pairs";
        return false;
    }
    const size_t userSamples = queries.size () / 2;
    for (const auto& constraint : candidate.constraints)
        for (const auto& point : constraint.points)
            queries.insert (queries.end (), { point.x, point.y });
    std::vector<TerrainSample> baseline;
    TerrainFieldSummary summary;
    if (!candidate.constraints.empty () &&
        !SampleTerrain (request.input.mesh, std::span<const double> (queries).subspan (userSamples * 2),
                        request.sampleStep, baseline, summary, error))
        return false;
    if (std::any_of (baseline.begin (), baseline.end (), [] (const auto& sample) { return !sample.onSurface; })) {
        error = "TerrainCoverageMissing: a constraint boundary is not covered by the baseline field";
        return false;
    }
    const auto property = Flatten (candidate.landscape.propertyLine);
    EditTerrainRequest terrainRequest { request.input };
    terrainRequest.operations = edits;
    terrainRequest.surfaces = request.surfaces;
    terrainRequest.property = property;
    terrainRequest.protectedRings = protectedViews;
    terrainRequest.smoothing = request.smoothing;
    terrainRequest.queriesXY = queries;
    terrainRequest.sampleStep = request.sampleStep;
    terrainRequest.volumes = request.volumes;
    if (!EvaluateEditTerrain (doc, terrainRequest, candidate.terrain, error))
        return false;
    size_t offset = 0;
    for (auto& constraint : candidate.constraints)
        for (size_t i = 0; i < constraint.points.size (); ++i, ++offset) {
            constraint.baseline.push_back (baseline.at (offset));
            const auto& proposed = candidate.terrain.samples.at (userSamples + offset);
            if (!proposed.onSurface) {
                error = "TerrainCoverageMissing: a constraint boundary is not covered by the proposed field";
                return false;
            }
            constraint.proposed.push_back (proposed);
        }
    candidate.terrain.samples.resize (userSamples);
    return true;
}
} // namespace

namespace landscape_terrain_detail {
bool DeriveScopeMasks (const geomsrv::edit::Document& doc, const ProvenanceRequest& provenance, OperationId operation,
                       LandscapeResult& landscape, ScopeMasks& masks, std::string& error)
{
    std::map<Id, Shape> footprints;
    if (!EvaluateLandscapeOperations (doc, landscape, &footprints)) {
        error = landscape.error;
        return false;
    }
    std::map<Id, Scope> scopes;
    if (!ConstraintScopes (*doc.landscape, landscape, scopes, error))
        return false;
    const auto scope = scopes.find (operation);
    if (scope == scopes.end () || scope->second.count (kNoId)) {
        error = "TerrainConstraintScopeUnsupported: operation requires one unmixed constraint scope";
        return false;
    }
    std::vector<LandscapeConstraintHeights> constraints;
    return ValidateCatalog (doc, provenance, footprints, {}, scope->second, landscape, error) &&
           ScopedMasks (landscape, scope->second, masks, constraints, error);
}
} // namespace landscape_terrain_detail

bool EvaluateLandscapeTerrain (const geomsrv::edit::Document& doc, const LandscapeTerrainRequest& request,
                               LandscapeTerrainResult& result, std::string& error)
{
    try {
        LandscapeTerrainResult candidate;
        if (!Evaluate (doc, request, candidate, error))
            return false;
        result = std::move (candidate);
        error.clear ();
        return true;
    }
    catch (const std::exception& exception) {
        error = std::string ("LandscapeTerrainRefused: ") + exception.what ();
        return false;
    }
}
} // namespace tapioca::geometry
