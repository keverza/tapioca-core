#ifndef TAPIOCA_EDIT_LANDSCAPE_TERRAIN_HPP
#define TAPIOCA_EDIT_LANDSCAPE_TERRAIN_HPP

#include "Edit/EditLandscape.hpp"
#include "Edit/EditSparseSolve.hpp"
#include "Edit/EditTerrainAdapter.hpp"
#include "Edit/EditTerrainTriangulation.hpp"

namespace tapioca::geometry {

struct LandscapeTerrainRequest {
    const EditTerrainInput& input;
    std::span<const EditTerrainOperation> edits;
    std::span<const TerrainMeshView> surfaces;
    std::optional<double> smoothing;
    std::span<const double> queriesXY;
    double sampleStep = 0.25;
    std::optional<EditTerrainVolumeOptions> volumes;
    std::span<const double> fallbackArea; // Derived terrain cover, only when there is no authored property line.
};
struct LandscapeConstraintHeights {
    Id operation = kNoId;
    OperationKind kind = OperationKind::GradingLimit;
    // Effective mask clipped to the property line. XY query locators only;
    // actual heights/coverage come from Rust, never the guide's authored Z.
    std::vector<Vec3> points;
    std::vector<TerrainSample> baseline, proposed;
};
struct LandscapeTerrainResult {
    LandscapeResult landscape;
    EditTerrainResult terrain;
    std::vector<LandscapeConstraintHeights> constraints;
};

// Worker-only evaluation using the same document/DAG as the planar editor.
// Property and positional masks are derived, never accepted from a second model.
// Area geometry comes from the operation footprint (one hole-free region for now);
// other tool parameters remain call-scoped flattened inputs to the existing adapter.
// Input provenance must retain every evaluated guide/operation and native binding;
// the catalog's ordered operation parents must match the document, not a second DAG.
// Grading limits bound influence; protection pins baseline heights, not an authored
// locked surface. Multiple edits require a common constraint scope; mixed scopes
// refuse until the kernel supports them without duplicating composition in C++.
// No native writes/history. Publishes the entire combined result only on success.
// This vertex-soft preview does not split straddling triangles or establish exact
// outside/zone surface preservation; those remain the split/rebuild lane's gate.
bool EvaluateLandscapeTerrain (const geomsrv::edit::Document& doc, const LandscapeTerrainRequest& request,
                               LandscapeTerrainResult& result, std::string& error);

struct SplitLandscapeTerrainResult {
    EditTerrainSplitResult baseline;
    LandscapeTerrainResult evaluation;
    EditTerrainPreservation preservation;
};
// Explicit split-first variant. The original baseline surface is split in Rust,
// then edits/quantities use its field-ready attribution, and publication waits for
// bitwise outside/boundary verification. Protection/grading-mask boundaries are
// still vertex-soft: this certifies the property exterior, not every inner zone.
bool EvaluateSplitLandscapeTerrain (const geomsrv::edit::Document& doc, const LandscapeTerrainRequest& request,
                                    const EditTerrainSplitOptions& options, SplitLandscapeTerrainResult& result,
                                    std::string& error);

struct SplitLandscapeSolveRequest {
    const EditTerrainSplitResult& baseline; // Prepared mesh; unknown locators index its vertices.
    OperationId operation = 0;
    std::span<const uint32_t> vertices; // Unknown i is this prepared vertex's height (metres).
    const GradingRequest& system;       // Caller-assembled; Rust certifies the solution.
    std::span<const double> queriesXY;  // Sample the solved surface.
    double sampleStep = 0.25;
    std::optional<EditTerrainVolumeOptions> volumes;
};
struct SplitLandscapeSolveResult {
    EditSparseSolveResult solve;
    EditTerrainResult terrain; // Solved heights on unchanged split topology.
    EditTerrainPreservation preservation;
};
// Applies a certified grading solve to the prepared split baseline. Unknowns are
// checked against the operation's document-derived scope before solving: a vertex
// on or inside a positional protection mask, or on or outside a reached grading-limit
// envelope, refuses with its own diagnostic (the two are never merged). Outside-face
// and property-edge vertices are left to the bitwise exterior verifier, the single
// authority for property preservation. Vertices whose solved height differs from the
// baseline are attributed to `operation`. Masks remain vertex-soft (a triangle that
// straddles a mask edge still tilts), and authored locked heights do not exist yet.
// No objective assembly, history or native write; atomic publication.
bool ApplySplitLandscapeSolve (const geomsrv::edit::Document& doc, const SplitLandscapeSolveRequest& request,
                               SplitLandscapeSolveResult& result, std::string& error);

} // namespace tapioca::geometry
#endif
