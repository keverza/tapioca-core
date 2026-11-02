#ifndef TAPIOCA_EDIT_LANDSCAPEUNIFIEDBUILD_HPP
#define TAPIOCA_EDIT_LANDSCAPEUNIFIEDBUILD_HPP

#include "Edit/EditLandscapeFieldPlan.hpp"
#include "Edit/EditTerrainDesignJob.hpp"
#include "Geometry/GeometryRustUnifiedTerrain.hpp"

namespace tapioca::geometry {

struct UnifiedLandscapeBuildOptions {
    double controlTolerance = 1e-3; // Absolute grading/fixed compatibility band, metres.
};

// #112 P4a: the two scopes one build answers to, kept explicitly separate.
// `rebuildRegion` is the ring the kernel actually rebuilt inside (for the
// current single-path scene the derived band reach clipped to property/cover);
// `measurementRegion` is the authored property line or the validated terrain
// cover the displayed site/context totals are conventionally measured against.
// The kernel's exact site/context volumes are rebuild-scope quantities; they
// are not yet the final site-total contract.
struct UnifiedLandscapeBuildScope {
    std::vector<double> rebuildRegion;
    std::vector<double> measurementRegion;
};

// Real compiler-to-build seam for #112 P3.1. Validates the complete plan and
// immutable capture, converts every domain/path/mask/control and its full input
// provenance, then calls the one Rust authority. No production dispatch, legacy
// fallback, document mutation or partial result. `scope` reports both regions
// on success. Worker-classified.
bool BuildUnifiedLandscapeTerrain (const geomsrv::edit::Document& doc, const TerrainCapture& capture,
                                   const FieldPlan& plan, const UnifiedLandscapeBuildOptions& options,
                                   UnifiedTerrainResult& result, UnifiedLandscapeBuildScope& scope, std::string& error);

} // namespace tapioca::geometry
#endif
