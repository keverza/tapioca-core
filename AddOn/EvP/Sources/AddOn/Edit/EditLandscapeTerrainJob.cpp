#include "Edit/EditRegionTerrain.hpp"
#include "Edit/EditTerrainCapture.hpp"
#include "Edit/EditTerrainAdapterInternal.hpp"
#include "Edit/EditTerrainDesign.hpp"
#include "Edit/EditLandscapeFieldPlan.hpp"
#include "Edit/EditPublishedTerrain.hpp"
#include <algorithm>
#include <exception>

namespace tapioca::geometry {
namespace {

// #112 P4c: the transitional consumer view of one unified publication. Legacy
// consumers read `result.build` until they consume the neutral snapshot
// directly; the published terrain is the authority. Displayed totals keep the
// 0.25 m convention against the measurement region, never the kernel-exact
// authority. Path-scene fields (stations, design lines, locked vertices,
// band-height attribution, control catalog offsets) mirror the legacy band
// lane's contract so existing consumers keep working unchanged.
void MirrorPublished (const UnifiedTerrainResult& unified, const FieldPlan& plan,
                      const UnifiedLandscapeBuildScope& scope, const PublishedTerrain& published,
                      TerrainDesignOutcome& outcome)
{
    outcome.result.revision = outcome.revision;
    outcome.result.sourceTerrainGuid = outcome.sourceTerrainGuid;
    outcome.result.region = published.rebuildRegion;
    outcome.result.stations = scope.stations;
    outcome.result.meshProvenance = published.meshProvenance;
    auto& build = outcome.result.build;
    build.points = published.points;
    build.base = published.base;
    build.triangles = published.triangles;
    build.mappings = published.mappings;
    build.owners = unified.owners;
    build.site = published.displaySite;
    build.context = published.displayContext;
    build.inputProvenance = published.inputProvenance;
    build.inputPointCount = unified.inputPointCount;
    build.inputRegionCount = unified.inputPropertyCount;
    // Control offsets in the combined domains=7 catalog: mesh, property, entry
    // rings, mask rings, control line vertices, then point controls.
    size_t catalog = unified.inputPointCount + unified.inputPropertyCount;
    for (const auto& entry : plan.entries)
        for (const auto& part : entry.parts)
            for (const auto& ring : part.rings)
                catalog += ring.size ();
    for (const auto& mask : plan.masks)
        for (const auto& part : mask.parts)
            for (const auto& ring : part.rings)
                catalog += ring.size ();
    for (const auto& control : plan.controls) {
        build.inputControlLineStarts.push_back (catalog);
        catalog += control.line.size ();
    }
    build.inputControlPointStart = catalog;
    for (const auto& line : unified.lines)
        if (line.domain == UnifiedLineDomain::Path && line.kind)
            build.lines.push_back ({ *line.kind, line.side, line.root });
    if (!plan.paths.empty ()) {
        // Band-moved heights carry the designed path's operation, exactly the
        // legacy band contract.
        outcome.result.vertexOperations.reserve (unified.owners.size ());
        for (const auto& owned : unified.owners)
            outcome.result.vertexOperations.push_back (
                owned ? std::optional<OperationId> (plan.paths.front ().operation) : std::nullopt);
    }
    for (const auto& fixed : unified.locked) {
        if (fixed.control >= plan.controls.size ())
            continue; // PublishUnifiedTerrain already validated this.
        const auto& control = plan.controls[fixed.control];
        TerrainDesignResult::FixedVertex value { fixed.vertex, {} };
        TerrainDesignControlRef ref;
        ref.source = control.source;
        if (control.point) {
            ref.vertices = { control.vertex, control.vertex };
        }
        else if (!control.segments.empty ()) {
            // The unified kernel attributes a locked mesh vertex to its control
            // but not to the authored segment; the first segment carries the
            // authored reference (a presentation approximation, documented).
            ref.segment = control.segments.front ().segment;
            ref.vertices = control.segments.front ().vertices;
        }
        value.supports.push_back (std::move (ref));
        outcome.result.locked.push_back (std::move (value));
    }
}

} // namespace

TerrainDesignOutcome RunLandscapeTerrainDesign (const TerrainDesignJob& job)
{
    // #112 P4c: every scene with a unified equivalent routes through the one
    // authority — region-only, band path-only and mixed. Two documented
    // exceptions keep the legacy lane: outline grading (no unified equivalent
    // yet) and a request with neither a region target nor a path, where the
    // legacy refusal text is the established diagnostic.
    if (job.doc && TerrainRegionObjects (*job.doc).empty () && (!job.options.source || job.options.outline))
        return RunTerrainDesign (job);
    TerrainDesignOutcome outcome;
    outcome.generation = job.generation;
    // The refusal assessment below needs the (possibly surface-prepared) capture.
    std::shared_ptr<const TerrainCapture> baseline = job.capture;
    try {
        if (!job.doc || !job.doc->landscape || !job.capture) {
            outcome.error = "SceneTerrain: missing immutable document/capture";
            return outcome;
        }
        outcome.revision = job.doc->revision;
        outcome.session = job.doc->landscape->sessionUuid;
        outcome.sourceTerrainGuid = job.doc->landscape->sourceTerrainGuid;
        if (job.surfaceTable) {
            auto surface = std::make_shared<TerrainCapture> ();
            if (!PrepareTerrainSurface (baseline, job.surfaceTable, *surface, outcome.error))
                return outcome;
            baseline = std::move (surface);
        }
        outcome.capture = baseline;
        const bool hasRegions = !TerrainRegionObjects (*job.doc).empty ();
        if (hasRegions) {
            // The attributed area stage stays the region diagnostic/reporting
            // view; the unified authority rebuilds the domains itself from the
            // same plan. Path-only scenes skip it (it refuses without region
            // objects), exactly as the legacy lane never ran it.
            auto regions = std::make_shared<RegionTerrainResult> ();
            if (!EvaluateRegionTerrain (*job.doc, baseline, *regions, outcome.error))
                return outcome;
            outcome.regions = std::move (regions);
        }
        // One plan → one unified build over the original immutable baseline →
        // one neutral publication, for all three scene shapes. The removed
        // region-first/path-second composition is never a failure fallback.
        // Outline grading is not unified (a documented #112 limit): an outline
        // request with region objects present still grades the path band, as
        // the mixed lane has done since P4b.
        LandscapeSnapshot snapshot;
        snapshot.doc = job.doc.get ();
        snapshot.revision = job.doc->revision;
        snapshot.captureRevision = baseline->revision;
        snapshot.session = job.doc->landscape->sessionUuid;
        snapshot.sourceTerrainGuid = job.doc->landscape->sourceTerrainGuid;
        if (job.options.source) {
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
            // The band lane trims stations to the cover whenever the caller
            // named no explicit region; the landscape lane never carries one.
            request.trimToCover = job.options.trimToCover || job.region.empty ();
            request.trimMargin = job.options.trimMargin;
            snapshot.path = request;
        }
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
        MirrorPublished (unified, plan, scope, *outcome.published, outcome);
    }
    catch (const std::exception& exception) {
        outcome.built = false;
        outcome.error = std::string ("SceneTerrain: ") + exception.what ();
    }
    if (!outcome.built) {
        outcome.result = {};
        outcome.unmet = { TerrainConditionText (outcome.error) };
        // Located refusal places keep the legacy band-lane contract: assessed
        // through the same authority the band lane uses. A refusal without a
        // location keeps none.
        if (job.options.source && job.doc->landscape) {
            auto options = job.options;
            options.region = {};
            options.cover = TerrainDesignArea (*job.doc, baseline->Input ().mesh);
            options.trimToCover = !options.cover.empty ();
            std::string ignored;
            if (!AssessTerrainDesign (*job.doc, baseline->Input (), options, outcome.conflicts, ignored))
                outcome.conflicts.clear ();
        }
    }
    return outcome;
}

std::future<TerrainDesignOutcome> LaunchLandscapeTerrainDesign (TerrainDesignJob job)
{
    return std::async (std::launch::async, [job = std::move (job)] { return RunLandscapeTerrainDesign (job); });
}
} // namespace tapioca::geometry
