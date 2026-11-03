#include "Edit/EditRegionTerrain.hpp"
#include "Edit/EditTerrainCapture.hpp"
#include "Edit/EditTerrainAdapterInternal.hpp"
#include "Edit/EditLandscapeFieldPlan.hpp"
#include "Edit/EditPublishedTerrain.hpp"
#include <algorithm>
#include <map>
#include <set>

namespace tapioca::geometry {
namespace {
std::shared_ptr<TerrainCapture> ProposedCapture (const RegionTerrainResult& regions, std::string& error)
{
    const auto& edited = regions.evaluation.terrain;
    auto out = std::make_shared<TerrainCapture> ();
    out->revision = edited.revision;
    out->sourceTerrainGuid = edited.sourceTerrainGuid;
    out->points = edited.points;
    out->triangles = edited.triangles;
    out->provenance = edited.inputProvenance;
    auto& provenance = out->provenance;
    const auto inputTable = provenance.table.context;
    uint64_t token = 10000;
    while (token == inputTable.tableId ||
           std::any_of (provenance.table.supportTables.begin (), provenance.table.supportTables.end (),
                        [=] (const auto& table) { return table.context.tableId == token; }))
        ++token;
    provenance.table.context.tableId = token;
    provenance.table.supportTables.push_back ({ inputTable, provenance.counts, false });
    const auto rows = provenance.table.mappings;
    std::map<uint32_t, std::set<uint64_t>> faceOperations;
    for (const auto& face : edited.faces)
        faceOperations[face.face].insert (face.operation);
    provenance.table.mappings.clear ();
    for (const auto& input : rows) {
        auto row =
            terrain_detail::Supported (input, input.domain, input.outputIndex, inputTable.tableId, provenance.table);
        std::set<uint64_t> operations;
        for (uint64_t i = 0; i < input.operations.count; ++i)
            operations.insert (edited.inputProvenance.table.operationIds.at (input.operations.offset + i));
        bool moved = false;
        if (input.domain == IdentityDomain::Vertex && edited.vertexOperations.at (input.outputIndex)) {
            operations.insert (*edited.vertexOperations.at (input.outputIndex));
            moved = true;
        }
        if (input.domain == IdentityDomain::Face) {
            const auto found = faceOperations.find (static_cast<uint32_t> (input.outputIndex));
            if (found != faceOperations.end ()) {
                operations.insert (found->second.begin (), found->second.end ());
                moved = true;
            }
        }
        if (moved) {
            row.kind = MappingKind::Inherited;
            row.operations = terrain_detail::Append (provenance.table.operationIds, operations);
        }
        provenance.table.mappings.push_back (row);
    }
    if (!terrain_detail::Validate (provenance, error))
        return {};
    return out;
}

bool Totals (const TerrainCapture& baseline, TerrainDesignOutcome& outcome, std::string& error)
{
    auto& build = outcome.result.build;
    TerrainVolumeRequest request;
    request.existing = { baseline.points, baseline.triangles };
    request.proposed = TerrainVolumeRequest::Proposed::Mesh;
    request.proposedMesh = { build.points, build.triangles };
    request.cell = 0.25;
    std::vector<double> property;
    if (outcome.regions)
        for (const auto& p : outcome.regions->evaluation.landscape.propertyLine)
            property.insert (property.end (), { p.x, p.y, p.z });
    request.property = property;
    TerrainVolumeReport report;
    if (!MeasureTerrainVolumes (request, report, error))
        return false;
    const auto copy = [] (const TerrainVolumes& v) { return HybridVolumes { v.cut, v.fill, v.net, v.area, 0 }; };
    build.site = copy (report.site);
    build.context = copy (report.context);
    std::vector<double> queries;
    for (size_t i = 0; i < build.points.size (); i += 3)
        queries.insert (queries.end (), { build.points[i], build.points[i + 1] });
    std::vector<TerrainSample> samples;
    TerrainFieldSummary summary;
    if (!SampleTerrain (request.existing, queries, 0.25, samples, summary, error))
        return false;
    build.base.clear ();
    for (const auto& sample : samples) {
        if (!sample.onSurface) {
            error = "SceneTerrainCoverage: proposed vertex is outside the original baseline";
            return false;
        }
        build.base.push_back (sample.height);
    }
    return true;
}
} // namespace

TerrainDesignOutcome RunLandscapeTerrainDesign (const TerrainDesignJob& job)
{
    if (job.doc && TerrainRegionObjects (*job.doc).empty ())
        return RunTerrainDesign (job);
    TerrainDesignOutcome outcome;
    outcome.generation = job.generation;
    try {
        if (!job.doc || !job.doc->landscape || !job.capture) {
            outcome.error = "SceneTerrain: missing immutable document/capture";
            return outcome;
        }
        outcome.revision = job.doc->revision;
        outcome.session = job.doc->landscape->sessionUuid;
        outcome.sourceTerrainGuid = job.doc->landscape->sourceTerrainGuid;
        auto baseline = job.capture;
        if (job.surfaceTable) {
            auto surface = std::make_shared<TerrainCapture> ();
            if (!PrepareTerrainSurface (baseline, job.surfaceTable, *surface, outcome.error))
                return outcome;
            baseline = std::move (surface);
        }
        outcome.capture = baseline;
        // The attributed area stage stays the region diagnostic/reporting view;
        // the unified authority rebuilds the domains itself from the same plan.
        auto regions = std::make_shared<RegionTerrainResult> ();
        if (!EvaluateRegionTerrain (*job.doc, baseline, *regions, outcome.error))
            return outcome;
        outcome.regions = regions;
        if (job.options.source) {
            // #112 P4b: one unified build over the original immutable baseline.
            // The mixed region-first/path-second ProposedCapture composition is
            // removed from this lane and is never a failure fallback. Outline
            // grading is not unified (a documented #112 limit): the compiled
            // single-path request grades the path band.
            LandscapeSnapshot snapshot;
            snapshot.doc = job.doc.get ();
            snapshot.revision = job.doc->revision;
            snapshot.captureRevision = baseline->revision;
            snapshot.session = job.doc->landscape->sessionUuid;
            snapshot.sourceTerrainGuid = job.doc->landscape->sourceTerrainGuid;
            PathDesignRequest request;
            request.source = job.options.source;
            request.operation = job.options.operation;
            request.object = job.options.object;
            request.maxStep = job.options.maxStep;
            request.taper = job.options.taper;
            request.margin = job.options.margin;
            request.arcTolerance = job.options.arcTolerance;
            request.cutLimit = job.options.cutLimit;
            request.fillLimit = job.options.fillLimit;
            request.effect = job.options.effect;
            request.tolerance = job.options.tolerance;
            request.outline = job.options.outline;
            snapshot.path = request;
            const FieldPlan plan = CompileLandscapeFieldPlan (snapshot);
            if (!ValidateLandscapeFieldPlanForBuild (plan, outcome.error)) {
                outcome.error = "LandscapeFieldPlan: " + outcome.error;
                return outcome;
            }
            UnifiedTerrainResult unified;
            UnifiedLandscapeBuildScope scope;
            if (!BuildUnifiedLandscapeTerrain (*job.doc, *baseline, plan, {}, unified, scope, outcome.error))
                return outcome;
            auto published = std::make_shared<PublishedTerrain> ();
            if (!PublishUnifiedTerrain (unified, plan, *baseline, scope, *published, outcome.error))
                return outcome;
            outcome.built = true;
            outcome.published = std::move (published);
            // Transitional mirror (P4b): legacy consumers read result.build until
            // P4c consumes the neutral snapshot directly. The published terrain
            // is the authority; displayed totals keep the 0.25 m convention
            // against the measurement region, never the kernel-exact authority.
            const auto& publishedTerrain = *outcome.published;
            outcome.result.revision = outcome.revision;
            outcome.result.sourceTerrainGuid = outcome.sourceTerrainGuid;
            outcome.result.region = publishedTerrain.rebuildRegion;
            auto& build = outcome.result.build;
            build.points = publishedTerrain.points;
            build.base = publishedTerrain.base;
            build.triangles = publishedTerrain.triangles;
            build.mappings = publishedTerrain.mappings;
            build.site = publishedTerrain.displaySite;
            build.context = publishedTerrain.displayContext;
            build.inputProvenance = publishedTerrain.inputProvenance;
        }
        else {
            // Region-only stays on the composition lane until P4c routes it
            // through the unified authority with full P0 parity.
            const auto proposed = ProposedCapture (*regions, outcome.error);
            if (!proposed)
                return outcome;
            outcome.built = true;
            outcome.result.revision = outcome.revision;
            outcome.result.sourceTerrainGuid = outcome.sourceTerrainGuid;
            outcome.result.build.points = proposed->points;
            outcome.result.build.triangles = proposed->triangles;
            outcome.result.build.inputProvenance = proposed->provenance;
            if (!Totals (*baseline, outcome, outcome.error))
                outcome.built = false;
        }
    }
    catch (const std::exception& exception) {
        outcome.built = false;
        outcome.error = std::string ("SceneTerrain: ") + exception.what ();
    }
    if (!outcome.built) {
        outcome.result = {};
        outcome.unmet = { TerrainConditionText (outcome.error) };
    }
    return outcome;
}

std::future<TerrainDesignOutcome> LaunchLandscapeTerrainDesign (TerrainDesignJob job)
{
    return std::async (std::launch::async, [job = std::move (job)] { return RunLandscapeTerrainDesign (job); });
}
} // namespace tapioca::geometry
