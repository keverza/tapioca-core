//! The study core as a Rust-owned handle: the portable half of a split
//! `StudyRecord` (issue #3, option A).
//!
//! Handle contract: `create`, `set_sample_meshes` and `reuse` require that no
//! other thread uses the handle (the core is still being prepared). After
//! that every call takes a shared handle and may run concurrently: the session
//! serialises itself (a second advance resolves 0 steps) and reads wait for
//! the session lock. Only `destroy` requires exclusive use again. A refused
//! domain read reports a status, never a failure; a poisoned session fails
//! with status 2 (`SunStudySessionPoisoned`).

use super::Regions;
use super::occlusion::{OcclusionState, TraversalVtable, VtableTraversal};
use super::patch_atlas::AtlasMeshView;
use super::reuse::{CancelCallback, ReuseMeshView, SeriesView};
use super::session::{SessionProgress, StepObserverCallback, state_of};
use super::surface::{PatchGridView, Reader, SampleGridView, patch_grid_of, sample_grid_of};
use crate::borrow::{BorrowContext, SliceView};
use crate::boundary::{Failure, call, memory_range};
use tapioca_sunstudy::atlas::AtlasOptions;
use tapioca_sunstudy::occlusion::{OcclusionAccumulator, StepOptions};
use tapioca_sunstudy::patch_atlas::AtlasMesh;
use tapioca_sunstudy::reuse::ReuseMesh;
use tapioca_sunstudy::series::SunSeries;
use tapioca_sunstudy::session::{AdvanceOptions, StudyInputs};
use tapioca_sunstudy::study::{PatchStudy, ReuseContext, StudyCore, StudyError, TriangleStudy};

/// Opaque study handle.
pub struct StudyHandle {
    pub(super) core: StudyCore,
}

pub(super) fn failure(error: StudyError) -> Failure {
    match error {
        StudyError::Poisoned => Failure::State("SunStudySessionPoisoned"),
        _ => Failure::State("SunStudyStudyRefused"),
    }
}

/// A shared reference to a live handle.
///
/// SAFETY: `handle` must be null or a live handle from `create`, not being
/// destroyed during the call.
pub(super) unsafe fn study<'a>(handle: *const StudyHandle) -> Result<&'a StudyCore, Failure> {
    memory_range(handle, 1)?;
    // SAFETY: non-null and aligned (checked); liveness is the caller's contract.
    unsafe { handle.as_ref() }
        .map(|h| &h.core)
        .ok_or(Failure::Argument("StudyHandle: null"))
}

/// An exclusive reference to a live handle.
///
/// SAFETY: as [`study`], and no other thread may use the handle.
unsafe fn study_mut<'a>(handle: *mut StudyHandle) -> Result<&'a mut StudyCore, Failure> {
    memory_range(handle.cast_const(), 1)?;
    // SAFETY: non-null and aligned (checked); exclusivity is the caller's
    // contract.
    unsafe { handle.as_mut() }
        .map(|h| &mut h.core)
        .ok_or(Failure::Argument("StudyHandle: null"))
}

/// Inputs of a new study. Only the arrays of the chosen `domain` are read.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StudyCreateRequest {
    /// 0 ground, 1 triangle, 2 patch.
    pub domain: u8,
    /// The day, used as it is.
    pub series: SeriesView,
    /// Grid spacing, metres.
    pub spacing: f64,
    /// Ground: xyz positions.
    pub positions: SliceView<f64>,
    /// Ground: xyz normals or empty.
    pub normals: SliceView<f64>,
    /// Triangle: the samples, with layouts.
    pub sample_grid: SampleGridView,
    /// Triangle: snapshot meshes for stable tile identities.
    pub meshes: SliceView<AtlasMeshView>,
    /// Triangle: gutter, maximum dimension. Patch: gutter ignored (1),
    /// maximum dimension used.
    pub atlas: [u32; 2],
    /// Patch: the samples.
    pub patch_grid: PatchGridView,
    /// The study being rerun, or null.
    pub previous: *const StudyHandle,
}

/// Create a study; `outcome` 0 created (and `out` written), 1 inconsistent
/// sample arrays, 2 patch atlas refused (nothing written to `out`).
///
/// # Safety
/// The request and every array of its domain must be live and immutable for
/// the call; `previous` null or live; `out` and `outcome` writable, disjoint.
/// Release the handle with `tapioca_sunstudy_study_destroy`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_study_create(
    request: *const StudyCreateRequest,
    out: *mut *mut StudyHandle,
    outcome: *mut u8,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.output(out, 1);
            regions.output(outcome, 1);
            regions.check()?;
            let request = regions.context().record(request)?;
            let previous = if request.previous.is_null() {
                None
            } else {
                Some(study(request.previous)?)
            };
            regions.input(request.series.steps.data, request.series.steps.count);
            let built = match request.domain {
                0 => ground(&mut regions, &request),
                1 => triangle(&mut regions, &request, previous),
                2 => patch(&mut regions, &request, previous),
                _ => Err(Failure::Argument("StudyCreateRequest: domain")),
            }?;
            match built {
                Ok(core) => {
                    out.write(Box::into_raw(Box::new(StudyHandle { core })));
                    outcome.write(0);
                }
                Err(StudyError::AtlasRefused) => outcome.write(2),
                Err(_) => outcome.write(1),
            }
            Ok(())
        })
    }
}

// SAFETY: the series steps were registered and checked.
unsafe fn series_of(context: &BorrowContext, view: &SeriesView) -> Result<SunSeries, Failure> {
    // SAFETY: forwarded from the caller's checked registration.
    let steps = unsafe { context.slice(view.steps)? }.to_vec();
    Ok(SunSeries::from_parts(
        steps,
        view.timestep_minutes,
        view.source_step_count,
        view.version,
    ))
}

// SAFETY: the request record is live; its ground arrays are registered here.
unsafe fn ground(
    regions: &mut Regions,
    request: &StudyCreateRequest,
) -> Result<Result<StudyCore, StudyError>, Failure> {
    regions.input(request.positions.data, request.positions.count);
    regions.input(request.normals.data, request.normals.count);
    regions.check()?;
    let context = regions.context();
    // SAFETY: shapes and disjointness checked just above.
    unsafe {
        Ok(StudyCore::ground(
            series_of(&context, &request.series)?,
            request.spacing,
            context.slice(request.positions)?.to_vec(),
            context.slice(request.normals)?.to_vec(),
        ))
    }
}

// SAFETY: the request record is live; its triangle arrays are registered here.
unsafe fn triangle(
    regions: &mut Regions,
    request: &StudyCreateRequest,
    previous: Option<&StudyCore>,
) -> Result<Result<StudyCore, StudyError>, Failure> {
    // SAFETY: each list is registered and checked before it is read.
    unsafe {
        let mut reader = Reader { regions };
        reader.sample_grid(&request.sample_grid);
        let mesh_views = reader.list(request.meshes)?;
        for mesh in &mesh_views {
            reader.regions.input(mesh.guid.data, mesh.guid.count);
        }
        regions.check()?;
        let context = regions.context();
        let meshes = mesh_views
            .iter()
            .map(|m| {
                Ok(AtlasMesh {
                    guid: context.text(m.guid)?,
                    triangle_count: m.triangle_count,
                })
            })
            .collect::<Result<Vec<_>, Failure>>()?;
        let [gutter, max_dimension] = request.atlas;
        Ok(StudyCore::triangle(TriangleStudy {
            series: series_of(&context, &request.series)?,
            spacing: request.spacing,
            grid: sample_grid_of(&context, &request.sample_grid)?,
            meshes: &meshes,
            previous,
            options: AtlasOptions {
                gutter,
                max_dimension,
            },
        }))
    }
}

// SAFETY: the request record is live; its patch arrays are registered here.
unsafe fn patch(
    regions: &mut Regions,
    request: &StudyCreateRequest,
    previous: Option<&StudyCore>,
) -> Result<Result<StudyCore, StudyError>, Failure> {
    // SAFETY: each list is registered and checked before it is read.
    unsafe {
        let mut reader = Reader { regions };
        reader.patch_grid(&request.patch_grid);
        let spans = reader.list(request.patch_grid.spans)?;
        for span in &spans {
            reader.regions.input(span.element.data, span.element.count);
        }
        regions.check()?;
        let context = regions.context();
        Ok(StudyCore::patch(PatchStudy {
            series: series_of(&context, &request.series)?,
            spacing: request.spacing,
            grid: patch_grid_of(&context, &request.patch_grid, &spans)?,
            previous,
            max_dimension: request.atlas[1],
        }))
    }
}

/// Release a study. Null is accepted.
///
/// # Safety
/// `handle` must be null or a live handle that no thread is using and that is
/// never used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_study_destroy(
    handle: *mut StudyHandle,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; ownership returns to Rust once.
    unsafe {
        call(error, error_capacity, || {
            if !handle.is_null() {
                memory_range(handle.cast_const(), 1)?;
                // SAFETY: the caller transfers the live Box allocation back.
                drop(Box::from_raw(handle));
            }
            Ok(())
        })
    }
}

/// Point the session at geometry, sun and grid versions (0 = no such input).
///
/// # Safety
/// The handle must be live; `versions` three readable `u64`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_study_sync(
    handle: *const StudyHandle,
    versions: *const [u64; 3],
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(versions, 1);
            regions.check()?;
            let [geometry_version, sun_version, grid_version] =
                regions.context().record(versions)?;
            study(handle)?
                .sync(StudyInputs {
                    geometry_version,
                    sun_version,
                    grid_version,
                })
                .map_err(failure)
        })
    }
}

/// Snapshot meshes.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StudyMeshesView {
    /// The meshes, in snapshot order.
    pub meshes: SliceView<ReuseMeshView>,
}

/// Register a mesh list and then each mesh's arrays.
///
/// SAFETY: the list must be live and immutable for the call.
unsafe fn register_meshes(
    regions: &mut Regions,
    view: SliceView<ReuseMeshView>,
) -> Result<Vec<ReuseMeshView>, Failure> {
    // SAFETY: forwarded; the list is checked before it is read.
    let list = unsafe { Reader { regions }.list(view)? };
    Reader { regions }.meshes(&list);
    Ok(list)
}

// SAFETY: every mesh array was registered and checked.
unsafe fn meshes_of<'a>(
    context: &'a BorrowContext,
    list: &[ReuseMeshView],
) -> Result<Vec<ReuseMesh<'a>>, Failure> {
    list.iter()
        .map(|m| {
            // SAFETY: forwarded from the caller's checked registration.
            unsafe {
                Ok(ReuseMesh {
                    guid: context.text(m.guid)?,
                    vertices: context.slice(m.vertices)?,
                    triangles: context.slice(m.triangles)?,
                })
            }
        })
        .collect()
}

/// Fill the sample-to-mesh map (`SetSampleMeshes`).
///
/// # Safety
/// The handle must be live and used by no other thread; the view and every
/// mesh array live and immutable for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_study_set_sample_meshes(
    handle: *mut StudyHandle,
    view: *const StudyMeshesView,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(view, 1);
            regions.check()?;
            let view = regions.context().record(view)?;
            let list = register_meshes(&mut regions, view.meshes)?;
            regions.check()?;
            let context = regions.context();
            let meshes = meshes_of(&context, &list)?;
            study_mut(handle)?.set_sample_meshes(&meshes);
            Ok(())
        })
    }
}

/// Reuse inputs: the completed source and both snapshots' meshes and roles.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StudyReuseRequest {
    /// The converged source study.
    pub source: *const StudyHandle,
    /// The source study's meshes.
    pub source_meshes: SliceView<ReuseMeshView>,
    /// The source study's role byte per mesh.
    pub source_roles: SliceView<u8>,
    /// The target study's meshes.
    pub target_meshes: SliceView<ReuseMeshView>,
    /// The target study's role byte per mesh.
    pub target_roles: SliceView<u8>,
    /// 1 when the source traced with the default ray bounds.
    pub source_default_ray_bounds: u8,
    /// Optional cancellation poll.
    pub cancel: CancelCallback,
}

/// What a reuse seeded.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct StudyReuseOutcome {
    /// Samples with no dirty step.
    pub reused_samples: usize,
    /// Sample x steps kept.
    pub reused_sample_steps: usize,
    /// 1 when the session was seeded.
    pub seeded: u8,
}

/// Seed the unpublished study from a completed one (after `sync` and
/// `set_sample_meshes`, before the first advance).
///
/// # Safety
/// `handle` live and used by no other thread; `source` live and distinct;
/// the request, arrays and cancellation context live for the call; `out`
/// writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_study_reuse(
    handle: *mut StudyHandle,
    request: *const StudyReuseRequest,
    out: *mut StudyReuseOutcome,
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
            if std::ptr::eq(request.source, handle.cast_const()) {
                return Err(Failure::Argument("StudyReuseRequest: source is the target"));
            }
            let source_list = register_meshes(&mut regions, request.source_meshes)?;
            let target_list = register_meshes(&mut regions, request.target_meshes)?;
            regions.input(request.source_roles.data, request.source_roles.count);
            regions.input(request.target_roles.data, request.target_roles.count);
            regions.check()?;
            let context = regions.context();
            let source_meshes = meshes_of(&context, &source_list)?;
            let target_meshes = meshes_of(&context, &target_list)?;
            let cancel = request.cancel;
            // The caller guarantees the poll and its context for the call; the
            // call runs inside this export's outer unsafe block.
            let poll = || cancel.is_cancelled.is_some_and(|f| f(cancel.context) != 0);
            let reuse = ReuseContext {
                source_meshes: &source_meshes,
                source_roles: context.slice(request.source_roles)?,
                target_meshes: &target_meshes,
                target_roles: context.slice(request.target_roles)?,
                source_default_ray_bounds: super::flag(
                    request.source_default_ray_bounds,
                    "StudyReuseRequest: source_default_ray_bounds",
                )?,
                is_cancelled: Some(&poll),
            };
            let source = study(request.source)?;
            let outcome = study_mut(handle)?
                .reuse_from(source, &reuse)
                .map_err(failure)?;
            out.write(
                outcome.map_or_else(StudyReuseOutcome::default, |o| StudyReuseOutcome {
                    reused_samples: o.reused_samples,
                    reused_sample_steps: o.reused_sample_steps,
                    seeded: 1,
                }),
            );
            Ok(())
        })
    }
}

/// `Advance` inputs: the samples are the study's own.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StudyAdvanceRequest {
    /// Traversal callbacks (copied for the call).
    pub traversal: *const TraversalVtable,
    /// Near distance, metres.
    pub tmin: f64,
    /// Far distance; <= 0 unbounded.
    pub tmax: f64,
    /// Passed to the traversal.
    pub max_parallel: usize,
    /// The caller's step budget.
    pub max_steps: usize,
    /// Optional observer.
    pub observer: StepObserverCallback,
}

/// Resolve up to `max_steps` more steps; `resolved` receives how many (0 also
/// when a concurrent advance holds the session).
///
/// # Safety
/// The handle must be live; the request, vtable and observer context live for
/// the call; `resolved` writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_study_advance(
    handle: *const StudyHandle,
    request: *const StudyAdvanceRequest,
    resolved: *mut usize,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.output(resolved, 1);
            regions.check()?;
            let request = regions.context().record(request)?;
            regions.input(request.traversal, 1);
            regions.check()?;
            let traversal = VtableTraversal {
                vtable: regions.context().record(request.traversal)?,
            };
            let poll = || traversal.cancelled();
            let options = AdvanceOptions {
                max_steps: request.max_steps,
                step: StepOptions {
                    tmin: request.tmin,
                    tmax: request.tmax,
                    max_parallel: request.max_parallel,
                    is_cancelled: Some(&poll),
                },
            };
            let callback = request.observer;
            let mut observe = |step: usize, accumulator: &OcclusionAccumulator, wall_ms: f64| {
                if let Some(observe) = callback.observe {
                    let state: OcclusionState = state_of(accumulator);
                    // SAFETY: the caller guarantees the callback/context for
                    // the call and that it neither unwinds nor re-enters.
                    observe(callback.context, step, wall_ms, &raw const state);
                }
            };
            let done = study(handle)?
                .advance(&traversal, &options, Some(&mut observe))
                .map_err(failure)?;
            resolved.write(done);
            Ok(())
        })
    }
}

/// Read progress and the accumulator's counters.
///
/// # Safety
/// The handle must be live; `out` writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_study_progress(
    handle: *const StudyHandle,
    out: *mut SessionProgress,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; the output is checked first.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.output(out, 1);
            regions.check()?;
            let session = study(handle)?.session();
            let advancing = session.advancing();
            let p = session
                .progress()
                .map_err(|_| failure(StudyError::Poisoned))?;
            let accumulator = session
                .with_accumulator(state_of)
                .map_err(|_| failure(StudyError::Poisoned))?;
            out.write(SessionProgress {
                generation: p.generation,
                resolved_steps: p.resolved_steps,
                total_steps: p.total_steps,
                sample_count: p.sample_count,
                accumulator,
                converged: u8::from(p.converged),
                empty: u8::from(p.empty),
                advancing: u8::from(advancing),
            });
            Ok(())
        })
    }
}
