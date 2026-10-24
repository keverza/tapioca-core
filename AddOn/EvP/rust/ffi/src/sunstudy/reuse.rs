//! Reuse planning, patch sample maps and the result summary.

use super::Regions;
use crate::borrow::{BorrowContext, SliceView};
use crate::boundary::{Failure, call};
use tapioca_sunstudy::reuse::{
    ReuseMesh, ReuseRequest, ReuseStudy, SampleMeshError, SpanOwner, patch_sample_meshes,
    plan_reuse,
};
use tapioca_sunstudy::series::{SunSeries, SunStep};
use tapioca_sunstudy::summary::{SunStudyResultSummary, summarize_sun_hours};

/// One snapshot mesh.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ReuseMeshView {
    /// Element GUID, UTF-8.
    pub guid: SliceView<u8>,
    /// xyz f64 vertices.
    pub vertices: SliceView<f64>,
    /// Index triples.
    pub triangles: SliceView<u32>,
}

/// An already-filtered day, used as it is.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SeriesView {
    /// Kept steps.
    pub steps: SliceView<SunStep>,
    /// Minutes per step.
    pub timestep_minutes: i32,
    /// Steps before the horizon filter.
    pub source_step_count: usize,
    /// Content version.
    pub version: u64,
}

/// One study's data that reuse compares.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ReuseStudyView {
    /// Snapshot meshes.
    pub meshes: SliceView<ReuseMeshView>,
    /// Role byte per mesh.
    pub roles: SliceView<u8>,
    /// xyz sample positions.
    pub positions: SliceView<f64>,
    /// xyz sample normals.
    pub normals: SliceView<f64>,
    /// Mesh index per sample.
    pub sample_meshes: SliceView<u32>,
    /// The study's day.
    pub series: SeriesView,
    /// Grid spacing, metres.
    pub grid_spacing: f64,
    /// Sampling domain (0 triangle, 1 patch).
    pub domain: u8,
}

/// An optional cancellation poll.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct CancelCallback {
    /// Passed back to `is_cancelled`.
    pub context: *mut core::ffi::c_void,
    /// Nullable; returns 1 when cancelled. Must not unwind.
    pub is_cancelled: Option<unsafe extern "C" fn(context: *mut core::ffi::c_void) -> u8>,
}

/// Reuse-planning inputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ReusePlanRequest {
    /// The converged source study.
    pub source: ReuseStudyView,
    /// The study being prepared.
    pub target: ReuseStudyView,
    /// Optional cancellation.
    pub cancel: CancelCallback,
    /// 1 when the source session converged.
    pub source_converged: u8,
    /// 1 when the source traced with the default ray bounds.
    pub source_default_ray_bounds: u8,
}

/// What the plan says.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ReusePlanSummary {
    /// Samples with no dirty step.
    pub reused_samples: usize,
    /// Sample x steps kept.
    pub reused_sample_steps: usize,
    /// 1 when a plan was produced (map and dirty words written), else 0.
    pub planned: u8,
}

/// Plan outputs: `map` holds one entry per target sample and `dirty` one word
/// per target sample per 64 steps.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ReusePlanOutput {
    /// Source sample per target sample (`SIZE_MAX` traces afresh).
    pub map: *mut usize,
    /// Capacity in entries.
    pub map_capacity: usize,
    /// Sample-major dirty-step words.
    pub dirty: *mut u64,
    /// Capacity in words.
    pub dirty_capacity: usize,
    /// Required summary.
    pub summary: *mut ReusePlanSummary,
}

fn register_study(regions: &mut Regions, study: &ReuseStudyView) {
    regions.input(study.meshes.data, study.meshes.count);
    regions.input(study.roles.data, study.roles.count);
    regions.input(study.positions.data, study.positions.count);
    regions.input(study.normals.data, study.normals.count);
    regions.input(study.sample_meshes.data, study.sample_meshes.count);
    regions.input(study.series.steps.data, study.series.steps.count);
}

/// Owned parts of a study view that the core borrows.
struct StudyParts<'a> {
    meshes: Vec<ReuseMesh<'a>>,
    series: SunSeries,
}

// SAFETY: every region of the study (and each mesh) was registered and checked.
unsafe fn study_parts<'a>(
    context: &'a BorrowContext,
    study: &ReuseStudyView,
    mesh_views: &[ReuseMeshView],
) -> Result<StudyParts<'a>, Failure> {
    // SAFETY: forwarded from the caller's checked registration.
    unsafe {
        let meshes = mesh_views
            .iter()
            .map(|m| {
                Ok(ReuseMesh {
                    guid: context.text(m.guid)?,
                    vertices: context.slice(m.vertices)?,
                    triangles: context.slice(m.triangles)?,
                })
            })
            .collect::<Result<Vec<_>, Failure>>()?;
        let series = SunSeries::from_parts(
            context.slice(study.series.steps)?.to_vec(),
            study.series.timestep_minutes,
            study.series.source_step_count,
            study.series.version,
        );
        Ok(StudyParts { meshes, series })
    }
}

// SAFETY: the study's arrays were registered and checked.
unsafe fn study_view<'a>(
    context: &'a BorrowContext,
    study: &ReuseStudyView,
    parts: &'a StudyParts<'a>,
) -> Result<ReuseStudy<'a>, Failure> {
    // SAFETY: forwarded from the caller's checked registration.
    unsafe {
        Ok(ReuseStudy {
            meshes: &parts.meshes,
            roles: context.slice(study.roles)?,
            positions: context.slice(study.positions)?,
            normals: context.slice(study.normals)?,
            sample_meshes: context.slice(study.sample_meshes)?,
            series: &parts.series,
            grid_spacing: study.grid_spacing,
            domain: study.domain,
        })
    }
}

/// Register a study's arrays and then each of its meshes' arrays.
///
/// SAFETY: the study's mesh list must be live and immutable.
unsafe fn register_meshes(
    regions: &mut Regions,
    study: &ReuseStudyView,
) -> Result<Vec<ReuseMeshView>, Failure> {
    register_study(regions, study);
    regions.check()?;
    let context = regions.context();
    // SAFETY: shape and disjointness checked just above; copied by value.
    let list = unsafe { context.slice(study.meshes)? }.to_vec();
    for mesh in &list {
        regions.input(mesh.guid.data, mesh.guid.count);
        regions.input(mesh.vertices.data, mesh.vertices.count);
        regions.input(mesh.triangles.data, mesh.triangles.count);
    }
    Ok(list)
}

/// Plan exact-sample reuse (`ReuseUnaffectedSamples` before seeding).
///
/// # Safety
/// The request, every array and text, and the cancellation context must be
/// live for the call; outputs at their capacities writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_plan_reuse(
    request: *const ReusePlanRequest,
    output: *const ReusePlanOutput,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.input(output, 1);
            regions.check()?;
            let (request, target) = (
                regions.context().record(request)?,
                regions.context().record(output)?,
            );
            let source_meshes = register_meshes(&mut regions, &request.source)?;
            let target_meshes = register_meshes(&mut regions, &request.target)?;
            regions.output(target.map, target.map_capacity);
            regions.output(target.dirty, target.dirty_capacity);
            regions.output(target.summary, 1);
            regions.check()?;
            let context = regions.context();
            let source_parts = study_parts(&context, &request.source, &source_meshes)?;
            let target_parts = study_parts(&context, &request.target, &target_meshes)?;
            let cancel = request.cancel;
            // The caller guarantees the poll and its context for the call; the
            // call runs inside this export's outer unsafe block.
            let poll = || cancel.is_cancelled.is_some_and(|f| f(cancel.context) != 0);
            let plan = plan_reuse(&ReuseRequest {
                source: study_view(&context, &request.source, &source_parts)?,
                target: study_view(&context, &request.target, &target_parts)?,
                source_converged: request.source_converged != 0,
                source_default_ray_bounds: request.source_default_ray_bounds != 0,
                is_cancelled: Some(&poll),
            });
            let mut summary = ReusePlanSummary::default();
            if let Some(plan) = plan {
                if target.map_capacity < plan.map.len()
                    || target.dirty_capacity < plan.dirty_steps.len()
                {
                    return Err(Failure::Argument("ReusePlanOutputTooSmall"));
                }
                std::ptr::copy_nonoverlapping(plan.map.as_ptr(), target.map, plan.map.len());
                std::ptr::copy_nonoverlapping(
                    plan.dirty_steps.as_ptr(),
                    target.dirty,
                    plan.dirty_steps.len(),
                );
                summary = ReusePlanSummary {
                    reused_samples: plan.reused_samples,
                    reused_sample_steps: plan.reused_sample_steps,
                    planned: 1,
                };
            }
            target.summary.write(summary);
            Ok(())
        })
    }
}

/// One patch span's owner.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SpanOwnerView {
    /// The span key's element GUID.
    pub element: SliceView<u8>,
    /// First sample.
    pub first: usize,
    /// Sample count.
    pub count: usize,
}

/// Patch sample-map inputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SampleMeshRequest {
    /// Snapshot meshes (only GUIDs and vertices are read).
    pub meshes: SliceView<ReuseMeshView>,
    /// Patch spans.
    pub spans: SliceView<SpanOwnerView>,
    /// Samples in the study.
    pub sample_count: usize,
}

/// Patch sample-map outputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SampleMeshOutput {
    /// Mesh index per sample (written only on success).
    pub meshes: *mut u32,
    /// Capacity in samples.
    pub capacity: usize,
    /// 0 written, 1 ambiguous meshes (keep any existing map), 2 bad span
    /// (clear the map).
    pub outcome: *mut u8,
}

/// Mesh index per sample for a patch-domain study (`SetSampleMeshes`).
///
/// # Safety
/// The request, its arrays and texts must be live and immutable; outputs
/// writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_patch_sample_meshes(
    request: *const SampleMeshRequest,
    output: *const SampleMeshOutput,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.input(output, 1);
            regions.check()?;
            let (request, target) = (
                regions.context().record(request)?,
                regions.context().record(output)?,
            );
            regions.input(request.meshes.data, request.meshes.count);
            regions.input(request.spans.data, request.spans.count);
            regions.check()?;
            let (mesh_views, span_views) = {
                let context = regions.context();
                (
                    context.slice(request.meshes)?.to_vec(),
                    context.slice(request.spans)?.to_vec(),
                )
            };
            for m in &mesh_views {
                regions.input(m.guid.data, m.guid.count);
                regions.input(m.vertices.data, m.vertices.count);
                regions.input(m.triangles.data, m.triangles.count);
            }
            for s in &span_views {
                regions.input(s.element.data, s.element.count);
            }
            regions.output(target.meshes, target.capacity);
            regions.output(target.outcome, 1);
            regions.check()?;
            let context = regions.context();
            let meshes = mesh_views
                .iter()
                .map(|m| {
                    Ok(ReuseMesh {
                        guid: context.text(m.guid)?,
                        vertices: context.slice(m.vertices)?,
                        triangles: context.slice(m.triangles)?,
                    })
                })
                .collect::<Result<Vec<_>, Failure>>()?;
            let spans = span_views
                .iter()
                .map(|s| {
                    Ok(SpanOwner {
                        element: context.text(s.element)?,
                        first: s.first,
                        count: s.count,
                    })
                })
                .collect::<Result<Vec<_>, Failure>>()?;
            let code = match patch_sample_meshes(&meshes, request.sample_count, &spans) {
                Ok(map) => {
                    if target.capacity < map.len() {
                        return Err(Failure::Argument("SampleMeshOutputTooSmall"));
                    }
                    if !map.is_empty() {
                        std::ptr::copy_nonoverlapping(map.as_ptr(), target.meshes, map.len());
                    }
                    0
                }
                Err(SampleMeshError::Ambiguous) => 1,
                Err(SampleMeshError::BadSpan) => 2,
            };
            target.outcome.write(code);
            Ok(())
        })
    }
}

/// Summary inputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SummaryRequest {
    /// Per-sample hours.
    pub hours: SliceView<f64>,
    /// The day's possible daylight, hours.
    pub daylight_hours: f64,
}

/// `SummarizeSunHours`.
///
/// # Safety
/// The request and hours must be live and immutable; `out` writable and
/// disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_summarize(
    request: *const SummaryRequest,
    out: *mut SunStudyResultSummary,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.output(out, 1);
            regions.check()?;
            let request = regions.context().record(request)?;
            regions.input(request.hours.data, request.hours.count);
            regions.check()?;
            let hours = regions.context().slice(request.hours)?.to_vec();
            out.write(summarize_sun_hours(&hours, request.daylight_hours));
            Ok(())
        })
    }
}
