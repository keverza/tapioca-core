#include "Edit/EditRegionTerrain.hpp"
#include "Edit/EditTerrainCapture.hpp"
#include "Edit/EditTerrainAdapterInternal.hpp"
#include "Edit/EditTerrainDesign.hpp"
#include "Edit/EditLandscapeFieldPlan.hpp"
#include "Edit/EditPublishedTerrain.hpp"
#include "Edit/EditLandscapeUnifiedBuild.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
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
    if (!plan.paths.empty () || !plan.outlines.empty ()) {
        // Band-moved heights carry the designed path's operation, exactly the
        // legacy band contract; outline-moved heights the requested path's (#149 T1).
        const OperationId operation =
            plan.paths.empty () ? plan.outlines.front ().operation : plan.paths.front ().operation;
        outcome.result.vertexOperations.reserve (unified.owners.size ());
        for (const auto& owned : unified.owners)
            outcome.result.vertexOperations.push_back (owned ? std::optional<OperationId> (operation) : std::nullopt);
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

// The outline's shortfalls in the user's words, the build's and the preview's alike: faces
// that never meet the ground, a reach widened past the one asked, faces stepping at the ring,
// and paths that do not validate.
void OutlineUnmet (uint32_t unreached, uint32_t clipped, double asked, double reached, size_t graded, size_t paths,
                   std::vector<std::string>& unmet)
{
    char text[256];
    if (unreached > 0) {
        std::snprintf (text, sizeof text,
                       "The side slopes do not meet the ground even %.0f m out: steepen them or add a wall.", reached);
        unmet.push_back (text);
    }
    else if (reached > asked) {
        std::snprintf (text, sizeof text,
                       "The side slopes do not meet the ground within %.1f m: built reaching up to %.1f m. "
                       "Steepen them or add a wall to keep them shorter.",
                       asked, reached);
        unmet.push_back (text);
    }
    if (clipped > 0)
        unmet.push_back ("The side slopes reach the edge of the terrain or property line before they meet the ground "
                         "and end there in a step: steepen them, move the path or add a wall.");
    if (paths > graded)
        unmet.push_back (std::to_string (paths - graded) + " of " + std::to_string (paths) +
                         " paths are not graded: they do not validate.");
}

} // namespace

TerrainRoute LandscapeTerrainRoute (const geomsrv::edit::Document& doc, bool hasPath, bool outline)
{
    // #149 T1 (owner decision 2026-11-07): the P4c "outline stays legacy" exception is dropped.
    // An outline request is one unified-build stage like any other, fixed heights included; the
    // legacy outline lane remains only as the parity oracle its tests compare against. A request
    // with neither a region target nor a path keeps the legacy refusal text.
    (void) outline;
    return TerrainRegionObjects (doc).empty () && !hasPath ? TerrainRoute::Legacy : TerrainRoute::Unified;
}

PathDesignRequest PathRequestFor (const TerrainDesignOptions& options, bool namedRegion)
{
    PathDesignRequest request;
    request.source = options.source;
    request.operation = options.operation;
    request.object = options.object;
    request.maxStep = options.maxStep;
    request.taper = options.taper;
    request.margin = options.margin;
    request.arcTolerance = options.arcTolerance;
    request.cutLimit = options.cutLimit;
    request.fillLimit = options.fillLimit;
    request.effect = options.effect;
    request.tolerance = options.tolerance;
    request.outline = options.outline;
    // The band lane trims stations to the cover whenever the caller named no explicit
    // region; the landscape lane never carries one.
    request.trimToCover = options.trimToCover || !namedRegion;
    request.trimMargin = options.trimMargin;
    return request;
}

TerrainDesignOutcome RunLandscapeTerrainDesign (const TerrainDesignJob& job)
{
    // #112 P4c: every scene routes through the one authority — region-only,
    // band or outline path, and mixed (#149 T1 dropped the outline exception).
    // A request with neither a region target nor a path keeps the legacy lane,
    // where the legacy refusal text is the established diagnostic. #112 P5: this scan is
    // only the cheap routing pre-filter so the legacy lane pays no plan cost;
    // inside the unified lane the compiled plan owns region discovery.
    if (job.doc &&
        LandscapeTerrainRoute (*job.doc, job.options.source != kNoId, job.options.outline) == TerrainRoute::Legacy)
        return RunTerrainDesign (job);
    TerrainDesignOutcome outcome;
    outcome.generation = job.generation;
    // The refusal assessment below needs the (possibly surface-prepared) capture.
    std::shared_ptr<const TerrainCapture> baseline = job.capture;
    // Every refusal below returns from this body and then reaches the refusal block after it:
    // an early `return outcome` from the try block used to skip it, so a unified build refusal
    // carried neither its condition text nor its located conflicts.
    const auto run = [&] () {
        if (!job.doc || !job.doc->landscape || !job.capture) {
            outcome.error = "SceneTerrain: missing immutable document/capture";
            return;
        }
        // As the legacy lane: the outcome names the capture it was attempted against, also when
        // the surface preparation below refuses.
        outcome.capture = job.capture;
        outcome.revision = job.doc->revision;
        outcome.session = job.doc->landscape->sessionUuid;
        outcome.sourceTerrainGuid = job.doc->landscape->sourceTerrainGuid;
        if (job.surfaceTable) {
            auto surface = std::make_shared<TerrainCapture> ();
            if (!PrepareTerrainSurface (baseline, job.surfaceTable, *surface, outcome.error))
                return;
            baseline = std::move (surface);
        }
        outcome.capture = baseline;
        // One plan → one unified build over the original immutable baseline →
        // one neutral publication, for every scene shape. The removed
        // region-first/path-second composition is never a failure fallback.
        // An outline request compiles to one outline stage after the regions
        // (#149 T1), never to the selected path's band.
        LandscapeSnapshot snapshot;
        snapshot.doc = job.doc.get ();
        snapshot.revision = job.doc->revision;
        snapshot.captureRevision = baseline->revision;
        snapshot.session = job.doc->landscape->sessionUuid;
        snapshot.sourceTerrainGuid = job.doc->landscape->sourceTerrainGuid;
        if (job.options.source)
            snapshot.path = PathRequestFor (job.options, !job.region.empty ());
        const FieldPlan plan = CompileLandscapeFieldPlan (snapshot);
        // #112 P5: the compiled plan owns region discovery — the region stage
        // runs exactly when the plan has entries (an active owner is a
        // configured effect on a live root on both sides). The P1 object scan
        // survives only as the cheap routing pre-filter above; the HUD keeps it
        // for per-frame polling gates, where compiling a plan per frame is not
        // viable.
        const bool hasRegions = !plan.entries.empty ();
        if (hasRegions) {
            // The attributed area stage stays the region diagnostic/reporting
            // view; the unified authority rebuilds the domains itself from the
            // same plan. Path-only scenes skip it (the plan has no entries),
            // exactly as the legacy lane never ran it.
            auto regions = std::make_shared<RegionTerrainResult> ();
            if (!EvaluateRegionTerrain (*job.doc, baseline, *regions, outcome.error))
                return;
            outcome.regions = std::move (regions);
        }
        if (!ValidateLandscapeFieldPlanForBuild (plan, outcome.error)) {
            outcome.error = "LandscapeFieldPlan: " + outcome.error;
            return;
        }
        UnifiedTerrainResult unified;
        UnifiedLandscapeBuildScope scope;
        UnifiedLandscapeBuildOptions build;
        if (!BuildUnifiedLandscapeTerrain (*job.doc, *baseline, plan, build, unified, scope, outcome.error))
            return;
        // #149 T1: the outline lane's reach policy, unchanged -- a face that runs out before
        // it meets the ground gets a wider reach, 4x at a time, at least 20 m, at most
        // kMaxSlopeReach; the widest build that succeeded is kept.
        const double asked = std::max (job.options.cutLimit, job.options.fillLimit);
        double reached = asked;
        if (!plan.outlines.empty ()) {
            build.outlineLimit = asked;
            while (unified.outlineUnreached > 0 && build.outlineLimit < kMaxSlopeReach) {
                build.outlineLimit = std::min (kMaxSlopeReach, std::max (build.outlineLimit * 4, 20.0));
                UnifiedTerrainResult wider;
                UnifiedLandscapeBuildScope widerScope;
                std::string ignored;
                if (!BuildUnifiedLandscapeTerrain (*job.doc, *baseline, plan, build, wider, widerScope, ignored))
                    break;
                unified = std::move (wider);
                scope = std::move (widerScope);
                reached = build.outlineLimit;
            }
        }
        auto published = std::make_shared<PublishedTerrain> ();
        if (!PublishUnifiedTerrain (unified, plan, *baseline, scope, *published, outcome.error))
            return;
        outcome.built = true;
        outcome.published = std::move (published);
        MirrorPublished (unified, plan, scope, *outcome.published, outcome);
        if (!plan.outlines.empty ()) {
            // The outline lane's located shortfalls, in its words.
            OutlineUnmet (unified.outlineUnreached, unified.outlineClipped, asked, reached,
                          plan.outlines.front ().paths.size (), job.doc->paths.size (), outcome.unmet);
        }
        else if (job.doc->paths.size () > plan.paths.size ())
            // The band lane grades one selected path; the others are said, not silently left out.
            outcome.unmet.push_back (std::to_string (job.doc->paths.size () - plan.paths.size ()) + " of " +
                                     std::to_string (job.doc->paths.size ()) +
                                     " paths are not graded: band grading grades one selected path.");
    };
    try {
        run ();
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
        if (job.options.source && job.doc && job.doc->landscape && baseline) {
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

TerrainPreviewOutcome RunLandscapeTerrainPreview (const TerrainDesignJob& job, const UnifiedPreviewAsk& ask)
{
    const auto started = std::chrono::steady_clock::now ();
    TerrainPreviewOutcome outcome;
    const auto run = [&] () {
        if (!job.doc || !job.doc->landscape || !job.capture) {
            outcome.error = "SceneTerrain: missing immutable document/capture";
            return;
        }
        outcome.revision = job.doc->revision;
        if (LandscapeTerrainRoute (*job.doc, job.options.source != kNoId, job.options.outline) ==
            TerrainRoute::Legacy) {
            outcome.error = "TerrainPreviewLegacy: this design builds on the legacy lane, which has no preview";
            return;
        }
        // As the build (RunLandscapeTerrainDesign): the surface a native solid prepares, then one
        // compiled plan over it.
        std::shared_ptr<const TerrainCapture> baseline = job.capture;
        if (job.surfaceTable) {
            auto surface = std::make_shared<TerrainCapture> ();
            if (!PrepareTerrainSurface (baseline, job.surfaceTable, *surface, outcome.error))
                return;
            baseline = std::move (surface);
        }
        LandscapeSnapshot snapshot;
        snapshot.doc = job.doc.get ();
        snapshot.revision = job.doc->revision;
        snapshot.captureRevision = baseline->revision;
        snapshot.session = job.doc->landscape->sessionUuid;
        snapshot.sourceTerrainGuid = job.doc->landscape->sourceTerrainGuid;
        if (job.options.source)
            snapshot.path = PathRequestFor (job.options, !job.region.empty ());
        const FieldPlan plan = CompileLandscapeFieldPlan (snapshot);
        if (!ValidateLandscapeFieldPlanForBuild (plan, outcome.error)) {
            outcome.error = "LandscapeFieldPlan: " + outcome.error;
            return;
        }
        // A short reach widens as the build's does (kMaxSlopeReach), inside the one Rust call.
        auto widened = ask;
        if (!plan.outlines.empty ())
            widened.maxReach = kMaxSlopeReach;
        outcome.built =
            PreviewUnifiedLandscapeTerrain (*job.doc, *baseline, plan, {}, widened, outcome.preview, outcome.error);
        if (outcome.built && !plan.outlines.empty ())
            OutlineUnmet (outcome.preview.unreached, outcome.preview.clipped,
                          std::max (job.options.cutLimit, job.options.fillLimit), outcome.preview.reach,
                          plan.outlines.front ().paths.size (), job.doc->paths.size (), outcome.unmet);
    };
    try {
        run ();
    }
    catch (const std::exception& exception) {
        outcome.built = false;
        outcome.error = std::string ("TerrainPreview: ") + exception.what ();
    }
    outcome.milliseconds =
        std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count ();
    return outcome;
}

std::future<TerrainPreviewOutcome> LaunchLandscapeTerrainPreview (TerrainDesignJob job, UnifiedPreviewAsk ask)
{
    return std::async (std::launch::async,
                       [job = std::move (job), ask] { return RunLandscapeTerrainPreview (job, ask); });
}
} // namespace tapioca::geometry
