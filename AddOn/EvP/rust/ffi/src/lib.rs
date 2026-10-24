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
pub use sunstudy::{
    AtlasMeshView, AtlasOutput, AtlasSamplesView, AtlasSummary, AtlasTileRecord,
    PatchAllocationRecord, PatchAllocationView, PatchAtlasCursorRecord, PatchAtlasFitOutput,
    PatchAtlasFitRequest, PatchAtlasStateOutput, PatchAtlasStateView, PatchAtlasUpdateRecord,
    PatchKeyView, PatchShapeView, StableAtlasOutput, StableAtlasRequest, StepMaskOutput,
    StepMaskRequest, StepScheduleOutput, StepScheduleRequest, TextureDeltaOutput,
    TextureDeltaRequest, TriangleAtlasRequest, tapioca_sunstudy_patch_atlas_fit,
    tapioca_sunstudy_stable_triangle_atlas, tapioca_sunstudy_step_masks,
    tapioca_sunstudy_step_schedule, tapioca_sunstudy_texture_delta,
    tapioca_sunstudy_triangle_atlas,
};
pub use sunstudy::{
    BindingOutput, BindingRequest, BoundListView, MaterialTransparency, PresetAdviceOutput,
    PresetAdviceRequest, ReceiverMeshView, ReceiversOutput, ReceiversRequest, ReceiversSummary,
    RolesOutput, RolesRequest, RolesSummary, tapioca_sunstudy_preset_advice,
    tapioca_sunstudy_receivers, tapioca_sunstudy_refresh_binding, tapioca_sunstudy_resolve_roles,
};
pub use sunstudy::{
    CancelCallback, ReuseMeshView, ReusePlanOutput, ReusePlanRequest, ReusePlanSummary,
    ReuseStudyView, SampleMeshOutput, SampleMeshRequest, SeriesView, SpanOwnerView, SummaryRequest,
    tapioca_sunstudy_patch_sample_meshes, tapioca_sunstudy_plan_reuse, tapioca_sunstudy_summarize,
};
pub use sunstudy::{
    FaceLayoutRecord, SampleGridOutput, SampleGridRequest, SampleGridSummary,
    tapioca_sunstudy_sample_grid,
};
pub use sunstudy::{
    MeshSpanRecord, PatchGridView, PatchSpanView, PreviousView, SampleGridView,
    SurfaceBuildRequest, SurfaceOptionsRecord, SurfaceReadOutput, SurfaceResultHandle,
    SurfaceResultSummary, tapioca_sunstudy_surface_build, tapioca_sunstudy_surface_destroy,
    tapioca_sunstudy_surface_read, tapioca_sunstudy_surface_summary,
};
pub use sunstudy::{
    OcclusionHandle, OcclusionRangeRequest, OcclusionReadOutput, OcclusionSeedRequest,
    OcclusionState, OcclusionStepRequest, OcclusionTrace, TraversalVtable,
    tapioca_sunstudy_occlusion_create, tapioca_sunstudy_occlusion_destroy,
    tapioca_sunstudy_occlusion_range, tapioca_sunstudy_occlusion_read,
    tapioca_sunstudy_occlusion_reset, tapioca_sunstudy_occlusion_seed,
    tapioca_sunstudy_occlusion_state, tapioca_sunstudy_occlusion_step,
};
pub use sunstudy::{
    PatchGridOutput, PatchGridRequest, PatchGridSummary, PatchSpanRecord,
    tapioca_sunstudy_patch_grid,
};
pub use sunstudy::{
    SessionAdvanceRequest, SessionHandle, SessionProgress, SessionSeedRequest, SessionSyncRequest,
    StepObserverCallback, tapioca_sunstudy_session_advance, tapioca_sunstudy_session_create,
    tapioca_sunstudy_session_destroy, tapioca_sunstudy_session_progress,
    tapioca_sunstudy_session_read, tapioca_sunstudy_session_seed, tapioca_sunstudy_session_sync,
};
pub use sunstudy::{
    StudyAdvanceRequest, StudyArraysOutput, StudyCreateRequest, StudyHandle, StudyImageOutput,
    StudyImageSummary, StudyInfo, StudyMeshesView, StudyReadAtRequest, StudyReadingRecord,
    StudyReuseOutcome, StudyReuseRequest, StudyStepMaskOutput, StudyStepMaskSummary,
    tapioca_sunstudy_study_advance, tapioca_sunstudy_study_arrays, tapioca_sunstudy_study_create,
    tapioca_sunstudy_study_destroy, tapioca_sunstudy_study_image, tapioca_sunstudy_study_info,
    tapioca_sunstudy_study_progress, tapioca_sunstudy_study_read_at, tapioca_sunstudy_study_reuse,
    tapioca_sunstudy_study_set_sample_meshes, tapioca_sunstudy_study_step_masks,
    tapioca_sunstudy_study_summary, tapioca_sunstudy_study_sync,
};
