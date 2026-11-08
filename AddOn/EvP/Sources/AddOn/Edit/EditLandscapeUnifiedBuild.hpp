#ifndef TAPIOCA_EDIT_LANDSCAPEUNIFIEDBUILD_HPP
#define TAPIOCA_EDIT_LANDSCAPEUNIFIEDBUILD_HPP

#include "Edit/EditLandscapeFieldPlan.hpp"
#include "Edit/EditTerrainDesignJob.hpp"
#include "Geometry/GeometryRustUnifiedTerrain.hpp"

namespace tapioca::geometry {

struct UnifiedLandscapeBuildOptions {
    double controlTolerance = 1e-3; // Absolute grading/fixed compatibility band, metres.
    // #149 T1: the outline faces' plan reach, metres; 0 takes the request's. The job widens
    // it, as the outline lane did, while faces run out before they meet the ground.
    double outlineLimit = 0.0;
};

// #112 P4a: the two scopes one build answers to, kept explicitly separate.
// `rebuildRegion` is the ring the kernel actually rebuilt inside (for the
// current single-path scene the derived band reach clipped to property/cover);
// `measurementRegion` is the authored property line or the validated terrain
// cover the displayed site/context totals are conventionally measured against.
// The kernel's exact site/context volumes are rebuild-scope quantities; they
// are not yet the final site-total contract.
// #112 P4c: `stations` carries the single path's effective (cover-trimmed)
// stations in travel order — the publication mirror reports them exactly as the
// legacy band lane reports its trimmed station list. Empty without a path.
struct UnifiedLandscapeBuildScope {
    std::vector<double> rebuildRegion;
    std::vector<double> measurementRegion;
    std::vector<double> stations;
};

// Real compiler-to-build seam for #112 P3.1; #112 P4b: the mixed production lane
// (RunLandscapeTerrainDesign) compiles the plan on the worker and calls this once
// over the original immutable capture. Validates the complete plan and
// capture/session/revision/source-terrain identity, converts every
// domain/path/mask/control and its full input provenance, then calls the one Rust
// authority. No legacy fallback, document mutation or partial result. `scope`
// reports both regions on success. Worker-classified.
bool BuildUnifiedLandscapeTerrain (const geomsrv::edit::Document& doc, const TerrainCapture& capture,
                                   const FieldPlan& plan, const UnifiedLandscapeBuildOptions& options,
                                   UnifiedTerrainResult& result, UnifiedLandscapeBuildScope& scope, std::string& error);

// #175: the same compiled request previewed from its field (PreviewUnifiedTerrain): a grid
// surface, contours and cut/fill in milliseconds, with no mesh.
bool PreviewUnifiedLandscapeTerrain (const geomsrv::edit::Document& doc, const TerrainCapture& capture,
                                     const FieldPlan& plan, const UnifiedLandscapeBuildOptions& options,
                                     const UnifiedPreviewAsk& ask, UnifiedPreviewResult& result, std::string& error);

} // namespace tapioca::geometry
#endif
