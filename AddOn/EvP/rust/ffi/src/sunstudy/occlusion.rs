//! The occlusion accumulator as a Rust-owned handle, traced through C++
//! traversal callbacks.
//!
//! Handle contract: created by `tapioca_sunstudy_occlusion_create`, released
//! by `tapioca_sunstudy_occlusion_destroy`, and used by ONE thread at a time
//! (the C++ store already serialises a record's session). A handle is never
//! retained by Rust after destroy. After a contained panic (status 3) the
//! handle's contents are unspecified and the caller should destroy it.
//!
//! Callback contract: the vtable's functions must not unwind into Rust (the
//! C++ shim catches everything), are called only on the calling thread during
//! the call, and their `context` must outlive the call.

use super::Regions;
use crate::borrow::{BorrowContext, SliceView};
use crate::boundary::{Failure, call, memory_range};
use tapioca_sunstudy::occlusion::{
    DirectionalQuery, OcclusionAccumulator, SampleSet, StepError, StepOptions, Traversal,
    TraversalError,
};

/// Opaque accumulator handle.
pub struct OcclusionHandle {
    accumulator: OcclusionAccumulator,
}

/// `ITraversal` as C function pointers.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TraversalVtable {
    /// Passed back to every callback.
    pub context: *mut core::ffi::c_void,
    /// Directional occlusion: returns 1 complete, 0 cancelled, 2 failed.
    pub occlude_directional: Option<
        unsafe extern "C" fn(
            context: *mut core::ffi::c_void,
            origins: *const f64,
            count: usize,
            direction: *const f64,
            tmin: f64,
            tmax: f64,
            out: *mut u8,
            max_parallel: usize,
        ) -> u8,
    >,
    /// Scene version (0 = no scene).
    pub scene_version: Option<unsafe extern "C" fn(context: *mut core::ffi::c_void) -> u64>,
    /// Optional cancellation poll: 1 cancelled.
    pub is_cancelled: Option<unsafe extern "C" fn(context: *mut core::ffi::c_void) -> u8>,
}

pub(super) struct VtableTraversal {
    pub(super) vtable: TraversalVtable,
}

impl VtableTraversal {
    pub(super) fn cancelled(&self) -> bool {
        // SAFETY: the caller guarantees the callback and context are valid for
        // the call and that the callback does not unwind.
        self.vtable
            .is_cancelled
            .is_some_and(|poll| unsafe { poll(self.vtable.context) } != 0)
    }
}

impl Traversal for VtableTraversal {
    fn occlude_directional(
        &self,
        query: &DirectionalQuery<'_>,
        out: &mut [u8],
    ) -> Result<bool, TraversalError> {
        let Some(trace) = self.vtable.occlude_directional else {
            return Err(TraversalError);
        };
        if out.len() * 3 != query.origins.len() {
            return Err(TraversalError);
        }
        // SAFETY: origins/out are live Rust buffers of the stated sizes for the
        // call; direction is a local [f64; 3]; the callback does not unwind
        // and does not retain the pointers.
        let status = unsafe {
            trace(
                self.vtable.context,
                query.origins.as_ptr(),
                out.len(),
                query.direction.as_ptr(),
                query.tmin,
                query.tmax,
                out.as_mut_ptr(),
                query.max_parallel,
            )
        };
        match status {
            1 => Ok(true),
            0 => Ok(false),
            _ => Err(TraversalError),
        }
    }

    fn scene_version(&self) -> u64 {
        // SAFETY: as `cancelled`.
        self.vtable
            .scene_version
            .map_or(0, |version| unsafe { version(self.vtable.context) })
    }
}

/// A live handle pointer, checked for null and alignment.
///
/// SAFETY: `handle` must be null or a live handle from `create`, used by no
/// other thread during the call.
pub(super) unsafe fn handle_mut<'a>(
    handle: *mut OcclusionHandle,
) -> Result<&'a mut OcclusionHandle, Failure> {
    memory_range(handle.cast_const(), 1)?;
    // SAFETY: non-null and aligned (checked); liveness/exclusivity are the
    // caller's contract.
    unsafe { handle.as_mut() }.ok_or(Failure::Argument("OcclusionHandle: null"))
}

/// Create an accumulator for `sample_count` samples and `step_count` steps.
///
/// # Safety
/// `out` must point to one writable pointer slot, disjoint from the error
/// buffer. The new handle must be released with `tapioca_sunstudy_occlusion_destroy`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_occlusion_create(
    sample_count: usize,
    step_count: usize,
    out: *mut *mut OcclusionHandle,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; the output slot is checked first.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.output(out, 1);
            regions.check()?;
            sample_count
                .checked_mul(step_count.div_ceil(64))
                .ok_or(Failure::Argument("OcclusionTooLarge"))?;
            let handle = Box::new(OcclusionHandle {
                accumulator: OcclusionAccumulator::new(sample_count, step_count),
            });
            out.write(Box::into_raw(handle));
            Ok(())
        })
    }
}

/// Release a handle. Null is accepted and ignored.
///
/// # Safety
/// `handle` must be null or a live handle from `create` that no thread is
/// using and that is never used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_occlusion_destroy(
    handle: *mut OcclusionHandle,
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

/// Clear every bit, flag and seeding.
///
/// # Safety
/// `handle` must be a live handle used by no other thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_occlusion_reset(
    handle: *mut OcclusionHandle,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; the handle contract is the caller's.
    unsafe {
        call(error, error_capacity, || {
            handle_mut(handle)?.accumulator.reset();
            Ok(())
        })
    }
}

/// Seeding inputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct OcclusionSeedRequest {
    /// A complete accumulator to copy from (distinct from the target).
    pub source: *const OcclusionHandle,
    /// Source sample per target sample; `SIZE_MAX` traces afresh.
    pub map: SliceView<usize>,
    /// Optional sample-major dirty-step words (empty = none).
    pub dirty: SliceView<u64>,
}

/// Seed reusable complete-day bits (`SeedReusable`); `accepted` 0 means the
/// inputs were refused and nothing changed.
///
/// # Safety
/// Both handles must be live and used by no other thread; the request and its
/// arrays must be live and immutable; `accepted` must be writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_occlusion_seed(
    handle: *mut OcclusionHandle,
    request: *const OcclusionSeedRequest,
    accepted: *mut u8,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.output(accepted, 1);
            regions.check()?;
            let request = regions.context().record(request)?;
            regions.input(request.map.data, request.map.count);
            regions.input(request.dirty.data, request.dirty.count);
            regions.check()?;
            if std::ptr::eq(request.source, handle) {
                accepted.write(0); // C++: seeding from itself is refused
                return Ok(());
            }
            memory_range(request.source, 1)?;
            let source = request
                .source
                .as_ref()
                .ok_or(Failure::Argument("OcclusionSeedSource: null"))?;
            let context = regions.context();
            let (map, dirty) = (context.slice(request.map)?, context.slice(request.dirty)?);
            let target = handle_mut(handle)?;
            let ok = target
                .accumulator
                .seed_reusable(&source.accumulator, map, dirty);
            accepted.write(u8::from(ok));
            Ok(())
        })
    }
}

/// The samples and trace settings shared by step and range calls.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct OcclusionTrace {
    /// Traversal callbacks (copied for the call).
    pub traversal: *const TraversalVtable,
    /// xyz positions.
    pub positions: SliceView<f64>,
    /// xyz unit normals, or empty for "always facing".
    pub normals: SliceView<f64>,
    /// Sample count the positions describe.
    pub sample_count: usize,
    /// Near distance, metres.
    pub tmin: f64,
    /// Far distance; <= 0 unbounded.
    pub tmax: f64,
    /// Passed to the traversal.
    pub max_parallel: usize,
}

/// One step: which, and toward where.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct OcclusionStepRequest {
    /// Samples and settings.
    pub trace: OcclusionTrace,
    /// Step index.
    pub step: usize,
    /// Direction toward the sun.
    pub sun: [f64; 3],
}

/// A range of steps.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct OcclusionRangeRequest {
    /// Samples and settings.
    pub trace: OcclusionTrace,
    /// xyz sun direction per series step.
    pub directions: SliceView<f64>,
    /// First step.
    pub first: usize,
    /// Most steps to resolve.
    pub max_steps: usize,
}

pub(super) struct Prepared<'a> {
    pub(super) traversal: VtableTraversal,
    pub(super) samples: SampleSet<'a>,
    pub(super) tmin: f64,
    pub(super) tmax: f64,
    pub(super) max_parallel: usize,
}

pub(super) fn register_trace(regions: &mut Regions, trace: &OcclusionTrace) {
    regions.input(trace.traversal, 1);
    regions.input(trace.positions.data, trace.positions.count);
    regions.input(trace.normals.data, trace.normals.count);
}

// SAFETY: the trace's regions were registered and checked.
pub(super) unsafe fn prepare<'a>(
    context: &'a BorrowContext,
    trace: &OcclusionTrace,
) -> Result<Prepared<'a>, Failure> {
    // SAFETY: forwarded from the caller's checked registration.
    unsafe {
        let vtable = context.record(trace.traversal)?;
        let normals = context.slice(trace.normals)?;
        Ok(Prepared {
            traversal: VtableTraversal { vtable },
            samples: SampleSet {
                positions: context.slice(trace.positions)?,
                normals: (!normals.is_empty()).then_some(normals),
                count: trace.sample_count,
            },
            tmin: trace.tmin,
            tmax: trace.tmax,
            max_parallel: trace.max_parallel,
        })
    }
}

/// Resolve one step. `outcome`: 0 committed, 1 refused (bad step or sample
/// set), 2 cancelled. A traversal failure returns status 2 (state).
///
/// # Safety
/// The handle must be live and used by no other thread; the request, vtable
/// and arrays must be live and immutable for the call; `outcome` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_occlusion_step(
    handle: *mut OcclusionHandle,
    request: *const OcclusionStepRequest,
    outcome: *mut u8,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.output(outcome, 1);
            regions.check()?;
            let request = regions.context().record(request)?;
            register_trace(&mut regions, &request.trace);
            regions.check()?;
            let context = regions.context();
            let prepared = prepare(&context, &request.trace)?;
            let poll = || prepared.traversal.cancelled();
            let options = StepOptions {
                tmin: prepared.tmin,
                tmax: prepared.tmax,
                max_parallel: prepared.max_parallel,
                is_cancelled: Some(&poll),
            };
            let result = handle_mut(handle)?.accumulator.accumulate_step(
                &prepared.traversal,
                &prepared.samples,
                (request.step, request.sun),
                &options,
            );
            let code = match result {
                Ok(()) => 0,
                Err(StepError::Refused) => 1,
                Err(StepError::Cancelled) => 2,
                Err(StepError::Traversal) => {
                    return Err(Failure::State("OcclusionTraversalFailed"));
                }
            };
            outcome.write(code);
            Ok(())
        })
    }
}

/// Resolve up to `max_steps` steps from `first`, stopping at the first that
/// does not commit; `done` receives how many were resolved.
///
/// # Safety
/// As [`tapioca_sunstudy_occlusion_step`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_occlusion_range(
    handle: *mut OcclusionHandle,
    request: *const OcclusionRangeRequest,
    done: *mut usize,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.output(done, 1);
            regions.check()?;
            let request = regions.context().record(request)?;
            register_trace(&mut regions, &request.trace);
            regions.input(request.directions.data, request.directions.count);
            regions.check()?;
            let context = regions.context();
            let prepared = prepare(&context, &request.trace)?;
            let directions: Vec<[f64; 3]> = context
                .slice(request.directions)?
                .as_chunks::<3>()
                .0
                .to_vec();
            let poll = || prepared.traversal.cancelled();
            let options = StepOptions {
                tmin: prepared.tmin,
                tmax: prepared.tmax,
                max_parallel: prepared.max_parallel,
                is_cancelled: Some(&poll),
            };
            let resolved = handle_mut(handle)?.accumulator.accumulate_range(
                &prepared.traversal,
                &prepared.samples,
                (&directions, request.first, request.max_steps),
                &options,
            );
            done.write(resolved);
            Ok(())
        })
    }
}

/// Counters and timings.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct OcclusionState {
    /// Samples.
    pub sample_count: usize,
    /// Steps.
    pub step_count: usize,
    /// 64-bit words per sample.
    pub words_per_sample: usize,
    /// Steps resolved.
    pub resolved_steps: usize,
    /// Samples still traced.
    pub active_samples: usize,
    /// Rays in the last step.
    pub last_ray_count: usize,
    /// Last step's compaction time, ms.
    pub compact_ms: f64,
    /// Last step's trace time, ms.
    pub trace_ms: f64,
    /// 1 when every step is resolved.
    pub complete: u8,
}

/// Read the counters.
///
/// # Safety
/// The handle must be live and used by no other thread; `out` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_occlusion_state(
    handle: *mut OcclusionHandle,
    out: *mut OcclusionState,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; the output is checked first.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.output(out, 1);
            regions.check()?;
            let a = &handle_mut(handle)?.accumulator;
            out.write(OcclusionState {
                sample_count: a.sample_count(),
                step_count: a.step_count(),
                words_per_sample: a.words_per_sample(),
                resolved_steps: a.resolved_step_count(),
                active_samples: a.active_sample_count(),
                last_ray_count: a.last_ray_count(),
                compact_ms: a.last_compact_milliseconds(),
                trace_ms: a.last_trace_milliseconds(),
                complete: u8::from(a.complete()),
            });
            Ok(())
        })
    }
}

/// Copy-out destinations; each may be null with zero capacity to skip it.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct OcclusionReadOutput {
    /// `sample_count * words_per_sample` bit words.
    pub bits: *mut u64,
    /// Capacity in words.
    pub bit_capacity: usize,
    /// One flag per step.
    pub steps_resolved: *mut u8,
    /// Capacity in steps.
    pub step_capacity: usize,
    /// One value per sample: lit steps x `hours_per_step`.
    pub sun_hours: *mut f64,
    /// Capacity in samples.
    pub hours_capacity: usize,
    /// Hours one step represents.
    pub hours_per_step: f64,
}

/// Copy the bitset, the resolved flags and/or the sun hours out.
///
/// # Safety
/// The handle must be live and used by no other thread; the descriptor must be
/// live; each nonzero-capacity array must be writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_occlusion_read(
    handle: *mut OcclusionHandle,
    output: *const OcclusionReadOutput,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(output, 1);
            regions.check()?;
            let target = regions.context().record(output)?;
            regions.output(target.bits, target.bit_capacity);
            regions.output(target.steps_resolved, target.step_capacity);
            regions.output(target.sun_hours, target.hours_capacity);
            regions.check()?;
            let a = &handle_mut(handle)?.accumulator;
            let wants = |capacity: usize, needed: usize| -> Result<bool, Failure> {
                match capacity {
                    0 => Ok(false),
                    c if c >= needed => Ok(true),
                    _ => Err(Failure::Argument("OcclusionReadOutputTooSmall")),
                }
            };
            let (bits, flags) = (a.bits(), a.steps_resolved());
            let want_bits = wants(target.bit_capacity, bits.len())?;
            let want_flags = wants(target.step_capacity, flags.len())?;
            let want_hours = wants(target.hours_capacity, a.sample_count())?;
            if want_bits && !bits.is_empty() {
                std::ptr::copy_nonoverlapping(bits.as_ptr(), target.bits, bits.len());
            }
            if want_flags {
                let bytes: Vec<u8> = flags.iter().map(|f| u8::from(*f)).collect();
                if !bytes.is_empty() {
                    std::ptr::copy_nonoverlapping(
                        bytes.as_ptr(),
                        target.steps_resolved,
                        bytes.len(),
                    );
                }
            }
            if want_hours {
                let hours = a.sun_hours(target.hours_per_step);
                if !hours.is_empty() {
                    std::ptr::copy_nonoverlapping(hours.as_ptr(), target.sun_hours, hours.len());
                }
            }
            Ok(())
        })
    }
}
