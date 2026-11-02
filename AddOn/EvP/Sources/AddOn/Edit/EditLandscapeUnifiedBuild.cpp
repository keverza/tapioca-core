#include "Edit/EditLandscapeUnifiedBuild.hpp"

#include "Edit/EditDocument.hpp"
#include "Edit/EditTerrainAdapterInternal.hpp"
#include "Edit/EditTerrainStation.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <optional>
#include <set>

namespace tapioca::geometry {
namespace {
namespace edit = geomsrv::edit;
using terrain_detail::Append;
using terrain_detail::Supported;

struct OwnedRequest {
    std::vector<double> property;
    struct Part {
        std::vector<double> outer;
        std::vector<std::vector<double>> holeStorage;
        std::vector<std::span<const double>> holes;
    };
    std::vector<Part> partStorage;
    std::vector<UnifiedDomainPart> parts;
    std::vector<UnifiedEntry> entries;
    std::vector<TerrainStation> stations;
    std::vector<UnifiedPath> paths;
    std::vector<UnifiedMask> masks;
    std::vector<std::vector<double>> controlLines;
    std::vector<UnifiedControl> controls;
    ProvenanceRequest provenance;
    std::optional<UnifiedPlanRequest> request;
};

std::vector<double> Flat (const std::vector<edit::Vec3>& points)
{
    std::vector<double> flat;
    flat.reserve (points.size () * 3);
    for (const auto& point : points)
        flat.insert (flat.end (), { point.x, point.y, point.z });
    return flat;
}

void ConvertPart (const FieldPlanPart& input, OwnedRequest& owned, size_t partIndex)
{
    auto& output = owned.partStorage[partIndex];
    output.outer = Flat (input.rings.front ());
    output.holeStorage.reserve (input.rings.size () - 1);
    output.holes.reserve (input.rings.size () - 1);
    for (size_t ring = 1; ring < input.rings.size (); ++ring) {
        output.holeStorage.push_back (Flat (input.rings[ring]));
        output.holes.push_back (output.holeStorage.back ());
    }
    owned.parts[partIndex] = { output.outer, output.holes };
}

std::set<SourceId> BaselineSources (const ProvenanceRequest& capture)
{
    std::set<SourceId> sources;
    for (const auto& row : capture.table.mappings) {
        for (uint64_t i = 0; i < row.sources.count; ++i)
            sources.insert (capture.table.sourceIds.at (row.sources.offset + i));
        if (!sources.empty ())
            break;
    }
    return sources;
}

void EnsureOperations (const edit::Document& doc, ProvenanceRequest& output)
{
    for (const auto& operation : doc.landscape->operations) {
        if (std::any_of (output.table.operations.begin (), output.table.operations.end (),
                         [&] (const IdentityOperation& row) { return row.id == operation.id; }))
            continue;
        std::set<OperationId> parents;
        for (Id input : operation.inputs)
            if (std::any_of (doc.landscape->operations.begin (), doc.landscape->operations.end (),
                             [=] (const LandscapeOperation& parent) { return parent.id == input; }))
                parents.insert (input);
        output.table.operations.push_back ({ operation.id, Append (output.table.operationIds, parents) });
    }
}

bool AppendGenerated (ProvenanceRequest& output, IdentityDomain domain, size_t count, const std::set<SourceId>& sources,
                      const std::set<OperationId>& operations, ObjectId object)
{
    if (sources.empty () || operations.empty ())
        return false;
    for (size_t index = 0; index < count; ++index) {
        EntityMapping row;
        row.kind = MappingKind::Generated;
        row.domain = domain;
        row.outputIndex = domain == IdentityDomain::Vertex ? output.counts.vertices++ : output.counts.edges++;
        row.sources = Append (output.table.sourceIds, sources);
        row.operations = Append (output.table.operationIds, operations);
        row.object = object;
        output.table.mappings.push_back (row);
    }
    return true;
}

bool BuildProvenance (const edit::Document& doc, const TerrainCapture& capture, const FieldPlan& plan,
                      size_t propertyVertices, const std::set<SourceId>& propertySources, ProvenanceRequest& result,
                      std::string& error)
{
    if (capture.provenance.table.objects.size () != 1) {
        error = "ProvenanceConflict: unified terrain requires one retained terrain owner";
        return false;
    }
    const auto& owner = capture.provenance.table.objects.front ();
    if (!terrain_detail::UniformOwner (capture.provenance, owner.id)) {
        error = "ProvenanceConflict: unified terrain requires one uniformly attributed terrain owner";
        return false;
    }
    std::set<SourceId> catalogSources = propertySources;
    std::set<OperationId> catalogOperations;
    for (const auto& entry : plan.entries) {
        catalogSources.insert (entry.sources.begin (), entry.sources.end ());
        catalogOperations.insert (entry.rootOperation);
    }
    for (const auto& path : plan.paths) {
        catalogSources.insert (path.source);
        catalogOperations.insert (path.operation);
    }
    for (const auto& mask : plan.masks) {
        catalogSources.insert (mask.sources.begin (), mask.sources.end ());
        catalogOperations.insert (mask.operation);
    }
    for (const auto& control : plan.controls)
        catalogSources.insert (control.source);
    catalogSources.erase (kNoId);
    catalogOperations.erase (0);
    for (SourceId source : catalogSources)
        if (!terrain_detail::CatalogSource (doc, capture.provenance, source, "unified plan source", error))
            return false;
    for (OperationId operation : catalogOperations)
        if (!terrain_detail::ValidateOperations (doc, capture.provenance, std::array<OperationId, 1> { operation },
                                                 error))
            return false;

    ProvenanceRequest combined = capture.provenance;
    combined.expectedRevision = doc.revision;
    combined.domains = 7;
    std::set<uint64_t> used { combined.table.context.tableId };
    for (const auto& support : combined.table.supportTables)
        used.insert (support.context.tableId);
    uint64_t token = 1;
    while (used.count (token))
        ++token;
    combined.table.context.tableId = token;
    combined.table.supportTables.push_back ({ capture.provenance.table.context, capture.provenance.counts, false });
    combined.table.mappings.clear ();
    for (const auto& row : capture.provenance.table.mappings)
        combined.table.mappings.push_back (
            Supported (row, row.domain, row.outputIndex, capture.provenance.table.context.tableId, combined.table));
    EnsureOperations (doc, combined);

    const auto append = [&] (IdentityDomain domain, size_t count, const std::set<SourceId>& sources,
                             std::set<OperationId> operations) {
        operations.insert (owner.operation);
        return AppendGenerated (combined, domain, count, sources, operations, owner.id);
    };
    if (!append (IdentityDomain::Vertex, propertyVertices, propertySources, catalogOperations) ||
        !append (IdentityDomain::Edge, propertyVertices, propertySources, catalogOperations)) {
        error = "ProvenanceUnattributable: the property/coverage ring has no source and operation contributors";
        return false;
    }
    const auto domain = [&] (const std::vector<FieldPlanPart>& parts, const std::vector<SourceId>& sources,
                             OperationId operation) {
        const std::set<SourceId> contributors (sources.begin (), sources.end ());
        for (const auto& part : parts)
            for (size_t ring = 0; ring < part.rings.size (); ++ring) {
                const size_t count = part.rings[ring].size ();
                if (!append (IdentityDomain::Vertex, count, contributors, { operation }) ||
                    !append (IdentityDomain::Edge, count, contributors, { operation }))
                    return false;
            }
        return true;
    };
    for (const auto& entry : plan.entries)
        if (!domain (entry.parts, entry.sources, entry.rootOperation)) {
            error = "ProvenanceUnattributable: an entry ring has no source contributors";
            return false;
        }
    for (const auto& mask : plan.masks)
        if (!domain (mask.parts, mask.sources, mask.operation)) {
            error = "ProvenanceUnattributable: a mask ring has no source contributors";
            return false;
        }
    for (const auto& control : plan.controls) {
        const std::set<SourceId> contributors { control.source };
        const size_t vertices = control.point ? 1 : control.line.size ();
        const size_t edges = control.point || control.line.empty () ? 0 : control.line.size () - 1;
        if (!append (IdentityDomain::Vertex, vertices, contributors, {}) ||
            (edges && !append (IdentityDomain::Edge, edges, contributors, {}))) {
            error = "ProvenanceUnattributable: a fixed control has no source contributors";
            return false;
        }
    }
    if (!terrain_detail::Validate (combined, error))
        return false;
    result = std::move (combined);
    return true;
}

bool Convert (const edit::Document& doc, const TerrainCapture& capture, const FieldPlan& plan,
              const UnifiedLandscapeBuildOptions& options, OwnedRequest& owned, UnifiedLandscapeBuildScope& scope,
              std::string& error)
{
    if (!ValidateLandscapeFieldPlanForBuild (plan, error) || !doc.landscape || plan.revision != doc.revision ||
        plan.captureRevision != capture.revision || plan.session != doc.landscape->sessionUuid ||
        plan.sourceTerrainGuid != doc.landscape->sourceTerrainGuid) {
        if (error.empty ())
            error = "ProvenanceStale: the field plan and capture do not describe the current landscape";
        return false;
    }
    if (!terrain_detail::ValidateInput (doc, capture.Input (), error) || !ValidateLandscape (doc, error))
        return false;

    const bool needsCoverage =
        !plan.entries.empty () || !plan.paths.empty () || !plan.masks.empty () || !plan.controls.empty ();
    std::set<SourceId> propertySources { plan.propertyLineSource };
    if (needsCoverage) {
        if (!plan.propertyLine.empty ())
            owned.property = Flat (plan.propertyLine);
        else {
            owned.property = TerrainDesignArea (doc, capture.Input ().mesh);
            propertySources = BaselineSources (capture.provenance);
        }
        if (owned.property.size () < 9) {
            error = "RegionTerrainCoverage: the unified terrain/property intersection is empty";
            return false;
        }
        // Authored property (or validated cover): the measurement scope for the
        // displayed site/context totals, independent of the rebuild scope.
        scope.measurementRegion = owned.property;
    }
    propertySources.erase (kNoId);
    if (needsCoverage && propertySources.empty ()) {
        error = "ProvenanceUnattributable: the unified coverage ring has no source";
        return false;
    }

    size_t partCount = 0;
    for (const auto& entry : plan.entries)
        partCount += entry.parts.size ();
    for (const auto& mask : plan.masks)
        partCount += mask.parts.size ();
    owned.partStorage.resize (partCount);
    owned.parts.resize (partCount);
    size_t partIndex = 0;
    owned.entries.reserve (plan.entries.size ());
    for (const auto& entry : plan.entries) {
        UnifiedEntry output;
        output.order = entry.order;
        output.object = entry.object;
        output.rootOperation = entry.rootOperation;
        output.sources = entry.sources;
        const size_t firstPart = partIndex;
        for (const auto& part : entry.parts)
            ConvertPart (part, owned, partIndex++);
        output.parts = std::span<const UnifiedDomainPart> (owned.parts).subspan (firstPart, entry.parts.size ());
        output.effect = static_cast<UnifiedTerrainEffect> (entry.terrain.effect);
        output.transition = { entry.terrain.transition == RegionTerrainTransition::Falloff
                                  ? UnifiedTransitionKind::Falloff
                                  : UnifiedTransitionKind::Daylight,
                              entry.terrain.falloff, entry.terrain.cut, entry.terrain.fill, entry.terrain.reach };
        if (entry.surface->elevation == RegionElevationMode::Existing)
            output.surface.kind = UnifiedSurfaceKind::Existing;
        else if (entry.surface->elevation == RegionElevationMode::Boundary ||
                 entry.surface->slope != RegionSlopeMode::None)
            output.surface = { UnifiedSurfaceKind::Triangulated, 0.0, entry.surfacePoints, entry.surfaceTriangles };
        else
            output.surface = { UnifiedSurfaceKind::Constant, entry.surface->first.z };
        owned.entries.push_back (output);
    }
    size_t stationCount = 0;
    for (const auto& path : plan.paths)
        stationCount += path.stations.size ();
    owned.stations.reserve (stationCount);
    owned.paths.reserve (plan.paths.size ());
    for (const auto& path : plan.paths) {
        const size_t firstStation = owned.stations.size ();
        for (size_t index = 0; index < path.stations.size (); ++index)
            owned.stations.push_back (CompileTerrainStation (path.stations[index], path.crossfallPercent[index],
                                                             path.design.cutLimit, path.design.fillLimit));
        owned.paths.push_back (
            { path.order, path.source, path.operation, path.object,
              std::span<const TerrainStation> (owned.stations).subspan (firstStation, path.stations.size ()),
              path.design.effect, path.design.tolerance });
    }
    if (owned.paths.size () == 1) {
        const auto& path = plan.paths.front ();
        TerrainDesignOptions design;
        design.source = path.source;
        design.operation = path.operation;
        design.object = path.object;
        design.margin = path.design.margin;
        design.arcTolerance = path.design.arcTolerance;
        design.cover = TerrainDesignArea (doc, capture.Input ().mesh);
        // P3 supports one path. Its production rebuild region is the band reach
        // clipped to property/cover, not the wider region-only coverage ring.
        // P4a keeps this rebuild scope separate from the measurement scope the
        // scope struct already recorded.
        if (!ResolveTerrainDesignRegion (doc, owned.paths.front ().stations, design, owned.property, error))
            return false;
        scope.rebuildRegion = owned.property;
        propertySources = { path.source };
    }
    else if (needsCoverage)
        scope.rebuildRegion = scope.measurementRegion;
    owned.masks.reserve (plan.masks.size ());
    for (const auto& mask : plan.masks) {
        UnifiedMask output;
        output.operation = mask.operation;
        output.sources = mask.sources;
        output.kind =
            mask.kind == OperationKind::GradingLimit ? UnifiedMaskKind::GradingLimit : UnifiedMaskKind::ProtectedZone;
        const size_t firstPart = partIndex;
        for (const auto& part : mask.parts)
            ConvertPart (part, owned, partIndex++);
        output.parts = std::span<const UnifiedDomainPart> (owned.parts).subspan (firstPart, mask.parts.size ());
        owned.masks.push_back (output);
    }
    owned.controlLines.reserve (plan.controls.size ());
    owned.controls.reserve (plan.controls.size ());
    for (const auto& control : plan.controls) {
        owned.controlLines.push_back (Flat (control.line));
        owned.controls.push_back ({ control.source,
                                    control.vertex,
                                    control.point,
                                    { control.pointPosition.x, control.pointPosition.y, control.pointPosition.z },
                                    owned.controlLines.back () });
    }
    if (!BuildProvenance (doc, capture, plan, owned.property.size () / 3, propertySources, owned.provenance, error))
        return false;
    owned.request.emplace (UnifiedPlanRequest { { capture.points, capture.triangles },
                                                owned.property,
                                                owned.entries,
                                                owned.paths,
                                                owned.masks,
                                                owned.controls,
                                                options.controlTolerance,
                                                plan.revision,
                                                plan.hash,
                                                owned.provenance });
    return true;
}

} // namespace

bool BuildUnifiedLandscapeTerrain (const edit::Document& doc, const TerrainCapture& capture, const FieldPlan& plan,
                                   const UnifiedLandscapeBuildOptions& options, UnifiedTerrainResult& result,
                                   UnifiedLandscapeBuildScope& scope, std::string& error)
{
    try {
        OwnedRequest owned;
        // The request carries a reference to owned.provenance; both stay alive and
        // unmoved until the low-level facade has published its owned result.
        if (!Convert (doc, capture, plan, options, owned, scope, error))
            return false;
        UnifiedTerrainResult candidate;
        if (!BuildUnifiedTerrain (*owned.request, candidate, error))
            return false;
        result = std::move (candidate);
        error.clear ();
        return true;
    }
    catch (const std::exception& exception) {
        error = std::string ("UnifiedLandscapeBuild: ") + exception.what ();
        return false;
    }
}

} // namespace tapioca::geometry
