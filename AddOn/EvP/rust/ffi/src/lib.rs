//! Aggregate Tapioca C ABI for version, geometry ingress and structural attribution.
//!
//! Mesh/snapshot validators borrow the existing geometry arrays without mutation.
//! Separate provenance views validate declared identity relationships; geometry-only
//! validation is not D14 certification. S2b affine/polyline parity primitives
//! expose no host API and do not switch a production geometry consumer.

#![deny(unsafe_code)]

// All foreign-memory access is confined to this small C boundary. Geometry stays safe.
#[allow(
    unsafe_code,
    reason = "C ABI pointer validation, call-scoped writes and export attribute"
)]
mod boundary;

pub use boundary::tapioca_version;

#[allow(
    unsafe_code,
    reason = "Call-scoped validation views borrow checked foreign arrays only at the C boundary"
)]
mod validation;

pub use validation::{
    MeshView, SliceView, SnapshotView, tapioca_validate_mesh, tapioca_validate_snapshot,
};

#[allow(
    unsafe_code,
    reason = "Central checked call-scoped foreign borrow boundary shared by validation exports"
)]
mod borrow;

#[allow(
    unsafe_code,
    reason = "Shared descriptor copy after full-capacity pool preflight"
)]
mod pools;

#[allow(
    unsafe_code,
    reason = "Provenance POD ingress and checked nonoverlapping diagnostic output at the C boundary"
)]
mod identity;

pub use identity::{
    ProvenanceRequest, ProvenanceView, tapioca_validate_attributed_mesh,
    tapioca_validate_provenance,
};

#[allow(
    unsafe_code,
    reason = "Complete detached-value POD ingress uses shared checked call-scoped foreign borrows"
)]
mod value;
pub use value::{
    MaterialIdentity, MeshValueView, SnapshotValueRequest, SnapshotValueView, SurfaceAttribute,
    tapioca_validate_mesh_value, tapioca_validate_snapshot_value,
};

#[allow(
    unsafe_code,
    reason = "Affine POD ingress and checked disjoint caller-owned coordinate/orientation publication"
)]
mod affine;
pub use affine::{
    AffineOutput, AffineRequest, TransformBuilderRequest, tapioca_build_transform,
    tapioca_compose_transforms, tapioca_transform_vectors,
};

#[allow(
    unsafe_code,
    reason = "Polyline POD ingress and checked disjoint caller-owned measurement/sample/division publication"
)]
mod polyline;
pub use polyline::{
    DividePolylineRequest, PolylineOutput, PolylineRequest, tapioca_divide_polyline,
    tapioca_sample_polyline,
};

#[allow(
    unsafe_code,
    reason = "Arc POD ingress and checked disjoint caller-owned point publication"
)]
mod arc;
pub use arc::{ArcRequest, tapioca_make_arc};

#[allow(
    unsafe_code,
    reason = "Plane POD ingress and checked disjoint frame/projected-point publication"
)]
mod plane;
pub use plane::{PlaneOutput, PlaneRequest, tapioca_project_to_plane};

#[allow(
    unsafe_code,
    reason = "Attributed polygon POD/pool preflight and atomic caller-owned coordinate publication"
)]
mod polygon;
pub use polygon::{AttributedPolygonRequest, tapioca_copy_attributed_polygon};

#[allow(
    unsafe_code,
    reason = "Mesh-preparation POD ingress, full-capacity alias preflight and atomic disjoint output publication"
)]
mod mesh_prep;
pub use mesh_prep::{
    MeshAnalysisOutput, MeshAnalysisRequest, MeshAnalysisSummary, WeldOutput, WeldRequest,
    tapioca_analyze_mesh, tapioca_weld_corners,
};

#[allow(
    unsafe_code,
    reason = "Grading POD pool preflight and atomic caller-owned value/summary publication"
)]
mod grading;
pub use grading::{
    GradingBound, GradingDifference, GradingFixed, GradingOutput, GradingRequest, GradingRow,
    GradingSummary, GradingTerm, tapioca_grading_solve,
};

#[allow(
    unsafe_code,
    reason = "Terrain flat POD/pool preflight and atomic caller-owned support mapping publication"
)]
mod terrain;
pub use terrain::{
    TerrainClip, TerrainClipOutput, TerrainConflict, TerrainConflictOutput, TerrainConstraint,
    TerrainCounts, TerrainDaylight, TerrainEdit, TerrainEditOutput, TerrainEditReport,
    TerrainEditRequest, TerrainEditVolumes, TerrainFaceOwner, TerrainFieldSummary,
    TerrainHybridCounts, TerrainHybridDiagnostic, TerrainHybridDiagnosticOutput, TerrainHybridLine,
    TerrainHybridLockedRequest, TerrainHybridOutput, TerrainHybridRequest, TerrainHybridSide,
    TerrainHybridVolumes, TerrainMapping, TerrainOutlineCounts, TerrainOutlineOutput,
    TerrainOutlineRequest, TerrainOutput, TerrainRefinement, TerrainRequest, TerrainRing,
    TerrainRowsOutput, TerrainSample, TerrainSampleOutput, TerrainSampleRequest, TerrainSlope,
    TerrainSplitRequest, TerrainStation, TerrainSurfaceRequest, TerrainSurfaceView,
    TerrainVolumeOutput, TerrainVolumeRequest, TerrainVolumes, tapioca_terrain_edit,
    tapioca_terrain_edit_clips, tapioca_terrain_hybrid_build, tapioca_terrain_hybrid_conflicts,
    tapioca_terrain_hybrid_locked_assess, tapioca_terrain_hybrid_locked_build,
    tapioca_terrain_outline_build, tapioca_terrain_sample, tapioca_terrain_split_mesh,
    tapioca_terrain_surface, tapioca_terrain_triangulate, tapioca_terrain_volume,
};

#[allow(
    unsafe_code,
    reason = "Sun-study POD ingress and checked caller-owned publication at the C boundary"
)]
mod sunstudy;
pub use sunstudy::{
    AccumLayoutOutput, GridLadderOutput, GridLadderRequest, GroundGridOutput, GroundGridRequest,
    GroundGridSummary, LimitsOutput, LimitsRequest, ShadowCamera, ShadowCameraRequest,
    SunSeriesOutput, SunSeriesRequest, SunSeriesSummary, WindingOutput, WindingReportOutput,
    WindingRequest, tapioca_sunstudy_accum_layout, tapioca_sunstudy_grid_ladder,
    tapioca_sunstudy_ground_grid, tapioca_sunstudy_limits, tapioca_sunstudy_orient_outward,
    tapioca_sunstudy_series, tapioca_sunstudy_shadow_camera, tapioca_sunstudy_version,
};
