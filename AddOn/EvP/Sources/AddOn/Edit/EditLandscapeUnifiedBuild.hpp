#ifndef TAPIOCA_EDIT_LANDSCAPEUNIFIEDBUILD_HPP
#define TAPIOCA_EDIT_LANDSCAPEUNIFIEDBUILD_HPP

#include "Edit/EditLandscapeFieldPlan.hpp"
#include "Edit/EditTerrainDesignJob.hpp"
#include "Geometry/GeometryRustUnifiedTerrain.hpp"

namespace tapioca::geometry {

struct UnifiedLandscapeBuildOptions {
    double controlTolerance = 1e-3; // Absolute grading/fixed compatibility band, metres.
};

// Real compiler-to-build seam for #112 P3.1. Validates the complete plan and
// immutable capture, converts every domain/path/mask/control and its full input
// provenance, then calls the one Rust authority. No production dispatch, legacy
// fallback, document mutation or partial result. Worker-classified.
bool BuildUnifiedLandscapeTerrain (const geomsrv::edit::Document& doc, const TerrainCapture& capture,
                                   const FieldPlan& plan, const UnifiedLandscapeBuildOptions& options,
                                   UnifiedTerrainResult& result, std::string& error);

} // namespace tapioca::geometry
#endif
