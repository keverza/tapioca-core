//! Sun-study C ABI (`SunStudyRustFfi.h`). Envelope and panic containment are shared
//! with the geometry exports; the analysis itself lives in `tapioca-sunstudy`.
//!
//! Every export follows one shape: a request record and an output descriptor are
//! read, every foreign region is shape-checked and proven disjoint from the
//! writable ones, the complete result is computed in Rust-owned memory, and only
//! then is it copied out. A refusal writes nothing but the error buffer.

use crate::borrow::{BorrowContext, SliceView};
use crate::boundary::{Failure, call, memory_range, overlaps};

mod atlas;
mod camera;
mod follower;
mod ground;
mod limits;
mod occlusion;
mod patch_atlas;
mod patches;
mod reuse;
mod roles;
mod sampler;
mod series;
mod session;
mod steps;
mod study;
mod study_read;
mod surface;
mod views;
mod visibility;
mod winding;

pub use atlas::{
    AtlasOutput, AtlasSamplesView, AtlasSummary, AtlasTileRecord, TriangleAtlasRequest,
    tapioca_sunstudy_triangle_atlas,
};
pub use camera::{
    AccumLayoutOutput, GridLadderOutput, GridLadderRequest, ShadowCamera, ShadowCameraRequest,
    tapioca_sunstudy_accum_layout, tapioca_sunstudy_grid_ladder, tapioca_sunstudy_shadow_camera,
};
pub use follower::{
    FollowerEvent, FollowerHandle, FollowerSnapshot, FollowerTextOutput,
    tapioca_sunstudy_follower_apply, tapioca_sunstudy_follower_create,
    tapioca_sunstudy_follower_destroy, tapioca_sunstudy_follower_text,
};
pub use ground::{
    GroundGridOutput, GroundGridRequest, GroundGridSummary, tapioca_sunstudy_ground_grid,
};
pub use limits::{LimitsOutput, LimitsRequest, tapioca_sunstudy_limits};
pub use occlusion::{
    OcclusionHandle, OcclusionRangeRequest, OcclusionReadOutput, OcclusionSeedRequest,
    OcclusionState, OcclusionStepRequest, OcclusionTrace, TraversalVtable,
    tapioca_sunstudy_occlusion_create, tapioca_sunstudy_occlusion_destroy,
    tapioca_sunstudy_occlusion_range, tapioca_sunstudy_occlusion_read,
    tapioca_sunstudy_occlusion_reset, tapioca_sunstudy_occlusion_seed,
    tapioca_sunstudy_occlusion_state, tapioca_sunstudy_occlusion_step,
};
pub use patch_atlas::{
    AtlasMeshView, PatchAllocationRecord, PatchAllocationView, PatchAtlasCursorRecord,
    PatchAtlasFitOutput, PatchAtlasFitRequest, PatchAtlasStateOutput, PatchAtlasStateView,
    PatchAtlasUpdateRecord, PatchKeyView, PatchShapeView, StableAtlasOutput, StableAtlasRequest,
    tapioca_sunstudy_patch_atlas_fit, tapioca_sunstudy_stable_triangle_atlas,
};
pub use patches::{
    PatchGridOutput, PatchGridRequest, PatchGridSummary, PatchSpanRecord,
    tapioca_sunstudy_patch_grid,
};
pub use reuse::{
    CancelCallback, ReuseMeshView, ReusePlanOutput, ReusePlanRequest, ReusePlanSummary,
    ReuseStudyView, SampleMeshOutput, SampleMeshRequest, SeriesView, SpanOwnerView, SummaryRequest,
    tapioca_sunstudy_patch_sample_meshes, tapioca_sunstudy_plan_reuse, tapioca_sunstudy_summarize,
};
pub use roles::{
    BindingOutput, BindingRequest, BoundListView, MaterialTransparency, PresetAdviceOutput,
    PresetAdviceRequest, ReceiverMeshView, ReceiversOutput, ReceiversRequest, ReceiversSummary,
    RolesOutput, RolesRequest, RolesSummary, tapioca_sunstudy_preset_advice,
    tapioca_sunstudy_receivers, tapioca_sunstudy_refresh_binding, tapioca_sunstudy_resolve_roles,
};
pub use sampler::{
    FaceLayoutRecord, SampleGridOutput, SampleGridRequest, SampleGridSummary,
    tapioca_sunstudy_sample_grid,
};
pub use series::{SunSeriesOutput, SunSeriesRequest, SunSeriesSummary, tapioca_sunstudy_series};
pub use session::{
    SessionAdvanceRequest, SessionHandle, SessionProgress, SessionSeedRequest, SessionSyncRequest,
    StepObserverCallback, tapioca_sunstudy_session_advance, tapioca_sunstudy_session_create,
    tapioca_sunstudy_session_destroy, tapioca_sunstudy_session_progress,
    tapioca_sunstudy_session_read, tapioca_sunstudy_session_seed, tapioca_sunstudy_session_sync,
};
pub use steps::{
    StepMaskOutput, StepMaskRequest, StepScheduleOutput, StepScheduleRequest, TextureDeltaOutput,
    TextureDeltaRequest, tapioca_sunstudy_step_masks, tapioca_sunstudy_step_schedule,
    tapioca_sunstudy_texture_delta,
};
pub use study::{
    StudyAdvanceRequest, StudyCreateRequest, StudyHandle, StudyMeshesView, StudyReuseOutcome,
    StudyReuseRequest, tapioca_sunstudy_study_advance, tapioca_sunstudy_study_create,
    tapioca_sunstudy_study_destroy, tapioca_sunstudy_study_progress, tapioca_sunstudy_study_reuse,
    tapioca_sunstudy_study_set_sample_meshes, tapioca_sunstudy_study_sync,
};
pub use study_read::{
    StudyArraysOutput, StudyImageOutput, StudyImageSummary, StudyInfo, StudyReadAtRequest,
    StudyReadingRecord, StudyStepMaskOutput, StudyStepMaskSummary, tapioca_sunstudy_study_arrays,
    tapioca_sunstudy_study_image, tapioca_sunstudy_study_info, tapioca_sunstudy_study_read_at,
    tapioca_sunstudy_study_step_masks, tapioca_sunstudy_study_summary,
};
pub use surface::{
    MeshSpanRecord, PatchGridView, PatchSpanView, PreviousView, SampleGridView,
    SurfaceBuildRequest, SurfaceOptionsRecord, SurfaceReadOutput, SurfaceResultHandle,
    SurfaceResultSummary, tapioca_sunstudy_surface_build, tapioca_sunstudy_surface_destroy,
    tapioca_sunstudy_surface_read, tapioca_sunstudy_surface_summary,
};
pub use views::{
    FirstHitRayRecord, OcclusionRayRecord, RayHitRecord, RayQueryVtable, ViewpointOutput,
    ViewpointRequest, tapioca_sunstudy_viewpoint,
};
pub use visibility::{
    VisibilityHandle, VisibilityOptionsRecord, VisibilityReadOutput, VisibilityRunRequest,
    VisibilitySummary, tapioca_sunstudy_visibility_destroy, tapioca_sunstudy_visibility_grids,
    tapioca_sunstudy_visibility_read, tapioca_sunstudy_visibility_run,
    tapioca_sunstudy_visibility_summary,
};
pub use winding::{
    WindingOutput, WindingReportOutput, WindingRequest, tapioca_sunstudy_orient_outward,
};

// Layouts are mirrored field for field in SunStudyRustFfi.h; SunStudyRust.cpp
// asserts the same sizes, so drift on either side fails to compile.
#[cfg(target_pointer_width = "64")]
const _: () = {
    use tapioca_sunstudy::series::SunStep;
    assert!(size_of::<SunStep>() == 40);
    assert!(size_of::<SunSeriesRequest>() == 32);
    assert!(size_of::<SunSeriesSummary>() == 48);
    assert!(size_of::<SunSeriesOutput>() == 24);
    assert!(size_of::<ShadowCameraRequest>() == 96);
    assert!(size_of::<ShadowCamera>() == 584);
    assert!(size_of::<AccumLayoutOutput>() == 12);
    assert!(size_of::<GridLadderRequest>() == 32);
    assert!(size_of::<GridLadderOutput>() == 32);
    assert!(size_of::<GroundGridRequest>() == 80);
    assert!(size_of::<GroundGridSummary>() == 48);
    assert!(size_of::<GroundGridOutput>() == 24);
    assert!(size_of::<WindingRequest>() == 56);
    assert!(size_of::<WindingReportOutput>() == 40);
    assert!(size_of::<WindingOutput>() == 24);
    assert!(size_of::<LimitsRequest>() == 48);
    assert!(size_of::<LimitsOutput>() == 80);
    assert!(size_of::<RolesRequest>() == 64);
    assert!(size_of::<RolesSummary>() == 56);
    assert!(size_of::<RolesOutput>() == 24);
    assert!(size_of::<ReceiverMeshView>() == 24);
    assert!(size_of::<MaterialTransparency>() == 16);
    assert!(size_of::<ReceiversRequest>() == 64);
    assert!(size_of::<ReceiversSummary>() == 56);
    assert!(size_of::<ReceiversOutput>() == 48);
    assert!(size_of::<PresetAdviceRequest>() == 24);
    assert!(size_of::<PresetAdviceOutput>() == 24);
    assert!(size_of::<BoundListView>() == 40);
    assert!(size_of::<BindingRequest>() == 136);
    assert!(size_of::<BindingOutput>() == 4);
    assert!(size_of::<SampleGridRequest>() == 120);
    assert!(size_of::<FaceLayoutRecord>() == 104);
    assert!(size_of::<SampleGridSummary>() == 48);
    assert!(size_of::<SampleGridOutput>() == 88);
    assert!(size_of::<PatchGridRequest>() == 136);
    assert!(size_of::<PatchSpanRecord>() == 192);
    assert!(size_of::<PatchGridSummary>() == 56);
    assert!(size_of::<PatchGridOutput>() == 96);
    assert!(size_of::<AtlasSamplesView>() == 64);
    assert!(size_of::<AtlasTileRecord>() == 16);
    assert!(size_of::<AtlasOutput>() == 40);
    assert!(size_of::<AtlasSummary>() == 40);
    assert!(size_of::<TriangleAtlasRequest>() == 72);
    assert!(size_of::<PatchKeyView>() == 32);
    assert!(size_of::<PatchAllocationView>() == 56);
    assert!(size_of::<PatchShapeView>() == 40);
    assert!(size_of::<PatchAtlasCursorRecord>() == 24);
    assert!(size_of::<PatchAtlasStateView>() == 40);
    assert!(size_of::<PatchAllocationRecord>() == 48);
    assert!(size_of::<PatchAtlasStateOutput>() == 56);
    assert!(size_of::<PatchAtlasUpdateRecord>() == 48);
    assert!(size_of::<PatchAtlasFitRequest>() == 64);
    assert!(size_of::<PatchAtlasFitOutput>() == 64);
    assert!(size_of::<AtlasMeshView>() == 24);
    assert!(size_of::<StableAtlasRequest>() == 176);
    assert!(size_of::<StableAtlasOutput>() == 96);
    assert!(size_of::<StepMaskRequest>() == 56);
    assert!(size_of::<StepMaskOutput>() == 24);
    assert!(size_of::<TextureDeltaRequest>() == 56);
    assert!(size_of::<TextureDeltaOutput>() == 24);
    assert!(size_of::<StepScheduleRequest>() == 24);
    assert!(size_of::<StepScheduleOutput>() == 40);
    assert!(size_of::<tapioca_sunstudy::texture_delta::AtlasRegion>() == 20);
    assert!(size_of::<TraversalVtable>() == 32);
    assert!(size_of::<OcclusionSeedRequest>() == 40);
    assert!(size_of::<OcclusionTrace>() == 72);
    assert!(size_of::<OcclusionStepRequest>() == 104);
    assert!(size_of::<OcclusionRangeRequest>() == 104);
    assert!(size_of::<OcclusionState>() == 72);
    assert!(size_of::<OcclusionReadOutput>() == 56);
    assert!(size_of::<SessionSyncRequest>() == 72);
    assert!(size_of::<SessionSeedRequest>() == 40);
    assert!(size_of::<StepObserverCallback>() == 16);
    assert!(size_of::<SessionAdvanceRequest>() == 96);
    assert!(size_of::<SessionProgress>() == 112);
    assert!(size_of::<ReuseMeshView>() == 48);
    assert!(size_of::<SeriesView>() == 40);
    assert!(size_of::<ReuseStudyView>() == 136);
    assert!(size_of::<CancelCallback>() == 16);
    assert!(size_of::<ReusePlanRequest>() == 296);
    assert!(size_of::<ReusePlanSummary>() == 24);
    assert!(size_of::<ReusePlanOutput>() == 40);
    assert!(size_of::<SpanOwnerView>() == 32);
    assert!(size_of::<SampleMeshRequest>() == 40);
    assert!(size_of::<SampleMeshOutput>() == 24);
    assert!(size_of::<SummaryRequest>() == 24);
    assert!(size_of::<tapioca_sunstudy::summary::SunStudyResultSummary>() == 56);
    assert!(size_of::<SurfaceOptionsRecord>() == 40);
    assert!(size_of::<MeshSpanRecord>() == 136);
    assert!(size_of::<SampleGridView>() == 160);
    assert!(size_of::<PatchSpanView>() == 208);
    assert!(size_of::<PatchGridView>() == 160);
    assert!(size_of::<PreviousView>() == 400);
    assert!(size_of::<SurfaceBuildRequest>() == 512);
    assert!(size_of::<SurfaceResultSummary>() == 224);
    assert!(size_of::<SurfaceReadOutput>() == 160);
    assert!(size_of::<StudyCreateRequest>() == 440);
    assert!(size_of::<StudyMeshesView>() == 16);
    assert!(size_of::<StudyReuseRequest>() == 96);
    assert!(size_of::<StudyReuseOutcome>() == 24);
    assert!(size_of::<StudyAdvanceRequest>() == 56);
    assert!(size_of::<StudyInfo>() == 88);
    assert!(size_of::<StudyArraysOutput>() == 48);
    assert!(size_of::<StudyImageOutput>() == 48);
    assert!(size_of::<StudyImageSummary>() == 32);
    assert!(size_of::<StudyStepMaskOutput>() == 32);
    assert!(size_of::<StudyStepMaskSummary>() == 40);
    assert!(size_of::<StudyReadAtRequest>() == 32);
    assert!(size_of::<StudyReadingRecord>() == 32);
    assert!(size_of::<FollowerEvent>() == 72);
    assert!(size_of::<FollowerSnapshot>() == 96);
    assert!(size_of::<FollowerTextOutput>() == 32);
    assert!(size_of::<FirstHitRayRecord>() == 56);
    assert!(size_of::<RayHitRecord>() == 16);
    assert!(size_of::<OcclusionRayRecord>() == 64);
    assert!(size_of::<RayQueryVtable>() == 40);
    assert!(size_of::<ViewpointRequest>() == 80);
    assert!(size_of::<ViewpointOutput>() == 40);
    assert!(size_of::<VisibilityOptionsRecord>() == 120);
    assert!(size_of::<VisibilityRunRequest>() == 192);
    assert!(size_of::<VisibilitySummary>() == 88);
    assert!(size_of::<VisibilityReadOutput>() == 112);
};

type Range = Option<(usize, usize)>;

/// Foreign regions of one call. Writable regions must be disjoint from every
/// other region, including the error buffer; read-only regions may alias.
pub(crate) struct Regions {
    error: Range,
    inputs: Vec<Result<Range, &'static str>>,
    outputs: Vec<Result<Range, &'static str>>,
}

fn shape<T>(pointer: *const T, count: usize) -> Result<Range, &'static str> {
    memory_range(pointer, count).map_err(|failure| match failure {
        Failure::Argument(message) | Failure::State(message) => message,
        Failure::Overlap | Failure::Internal => "Malformed buffer",
    })
}

impl Regions {
    pub(crate) fn new(error: *mut u8, capacity: usize) -> Result<Self, Failure> {
        Ok(Self {
            error: memory_range(error.cast_const(), capacity)?,
            inputs: Vec::new(),
            outputs: Vec::new(),
        })
    }

    pub(crate) fn input<T>(&mut self, pointer: *const T, count: usize) {
        self.inputs.push(shape(pointer, count));
    }

    pub(crate) fn output<T>(&mut self, pointer: *mut T, count: usize) {
        self.outputs.push(shape(pointer.cast_const(), count));
    }

    pub(crate) fn context(&self) -> BorrowContext {
        BorrowContext {
            error_range: self.error,
            output_range: None,
        }
    }

    /// Register a list of UTF-8 text views: the list itself is checked and copied,
    /// then each text is registered as an input for the next [`Self::check`].
    ///
    /// SAFETY: a nonempty list must be live, initialized, immutable storage of
    /// `view.count` text descriptors for the call.
    pub(crate) unsafe fn text_list(
        &mut self,
        view: SliceView<SliceView<u8>>,
    ) -> Result<Vec<SliceView<u8>>, Failure> {
        self.input(view.data, view.count);
        self.check()?;
        let context = self.context();
        // SAFETY: shape and error disjointness checked; descriptors are copied.
        let list = unsafe { context.slice(view)? }.to_vec();
        for text in &list {
            self.input(text.data, text.count);
        }
        Ok(list)
    }

    /// Overlap with the error buffer is reported first, before any shape error,
    /// so the diagnostic write cannot land in a live region.
    pub(crate) fn check(&self) -> Result<(), Failure> {
        let known = self.inputs.iter().chain(&self.outputs).flatten();
        if known.clone().any(|range| overlaps(*range, self.error)) {
            return Err(Failure::Overlap);
        }
        let mut inputs = Vec::with_capacity(self.inputs.len());
        for range in &self.inputs {
            inputs.push((*range).map_err(Failure::Argument)?);
        }
        let mut outputs = Vec::with_capacity(self.outputs.len());
        for range in &self.outputs {
            outputs.push((*range).map_err(Failure::Argument)?);
        }
        for (index, output) in outputs.iter().enumerate() {
            let others = outputs.iter().skip(index + 1).chain(&inputs);
            if others.clone().any(|other| overlaps(*output, *other)) {
                return Err(Failure::Overlap);
            }
        }
        Ok(())
    }
}

/// Decode a C `uint8_t` flag that must be 0 or 1.
pub(crate) fn flag(value: u8, message: &'static str) -> Result<bool, Failure> {
    match value {
        0 => Ok(false),
        1 => Ok(true),
        _ => Err(Failure::Argument(message)),
    }
}

/// Read the sun-study ABI version (currently 1), independent of the aggregate
/// `tapioca_version`, so the C++ wrapper can refuse a mismatched archive.
///
/// Returns status 0 on success, 1 for malformed arguments, or 3 for a caught
/// unwinding panic. On failure the version output is unchanged.
///
/// # Safety
/// `version` must point to one aligned, writable `u32`. If `error_capacity` is
/// nonzero, `error` must point to that many writable bytes. The regions must not
/// overlap. Null/alignment/size checks cannot prove an allocation is valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_version(
    version: *mut u32,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: foreign allocations/lifetimes are guaranteed by the caller; call
    // checks the error range and contains unwinding panics from the operation.
    unsafe {
        call(error, error_capacity, || {
            let output = memory_range(version.cast_const(), 1)?;
            if overlaps(output, memory_range(error.cast_const(), error_capacity)?) {
                return Err(Failure::Overlap);
            }
            // SAFETY: representation checked; caller guarantees the allocation.
            version.write(tapioca_sunstudy::ABI_VERSION);
            Ok(())
        })
    }
}

#[cfg(test)]
mod tests;
