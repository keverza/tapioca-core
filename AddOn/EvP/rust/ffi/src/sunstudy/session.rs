//! The study session as a Rust-owned handle.
//!
//! Handle contract: unlike the bare accumulator, a session handle may be used
//! from several threads at once, because the session serialises itself: a
//! concurrent advance is refused (resolves 0 steps) and every other call waits
//! for the session's lock. Only `destroy` requires that no other thread is
//! using the handle. The step observer runs on the advancing thread while the
//! session is locked and must not call back into the session. After a
//! contained panic the session is poisoned: calls return status 2
//! (`SunStudySessionPoisoned`) and the caller should destroy it.

use super::Regions;
use super::occlusion::{
    OcclusionReadOutput, OcclusionState, OcclusionTrace, prepare, register_trace,
};
use crate::borrow::SliceView;
use crate::boundary::{Failure, call, memory_range};
use tapioca_sunstudy::occlusion::{OcclusionAccumulator, StepOptions};
use tapioca_sunstudy::series::{SunSeries, SunStep};
use tapioca_sunstudy::session::{AdvanceOptions, Poisoned, StudyInputs, SunStudySession};

/// Opaque session handle.
pub struct SessionHandle {
    session: SunStudySession,
}

fn poisoned(_: Poisoned) -> Failure {
    Failure::State("SunStudySessionPoisoned")
}

/// A shared reference to a live session.
///
/// SAFETY: `handle` must be null or a live handle from `create`; it may be
/// shared with other threads (the session is internally synchronised) but
/// must not be destroyed during the call.
unsafe fn session<'a>(handle: *const SessionHandle) -> Result<&'a SunStudySession, Failure> {
    memory_range(handle, 1)?;
    // SAFETY: non-null and aligned (checked); liveness is the caller's contract.
    unsafe { handle.as_ref() }
        .map(|h| &h.session)
        .ok_or(Failure::Argument("SessionHandle: null"))
}

pub(super) fn state_of(a: &OcclusionAccumulator) -> OcclusionState {
    OcclusionState {
        sample_count: a.sample_count(),
        step_count: a.step_count(),
        words_per_sample: a.words_per_sample(),
        resolved_steps: a.resolved_step_count(),
        active_samples: a.active_sample_count(),
        last_ray_count: a.last_ray_count(),
        compact_ms: a.last_compact_milliseconds(),
        trace_ms: a.last_trace_milliseconds(),
        complete: u8::from(a.complete()),
    }
}

/// Create an empty session.
///
/// # Safety
/// `out` must point to one writable pointer slot, disjoint from the error
/// buffer. Release the handle with `tapioca_sunstudy_session_destroy`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_session_create(
    out: *mut *mut SessionHandle,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; the output slot is checked first.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.output(out, 1);
            regions.check()?;
            out.write(Box::into_raw(Box::new(SessionHandle {
                session: SunStudySession::default(),
            })));
            Ok(())
        })
    }
}

/// Release a session. Null is accepted.
///
/// # Safety
/// `handle` must be null or a live handle that no thread is using and that is
/// never used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_session_destroy(
    handle: *mut SessionHandle,
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

/// `Sync` inputs: the three versions, an already-filtered day and the sample
/// count.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SessionSyncRequest {
    /// Geometry, sun and grid versions (0 = no such input).
    pub versions: [u64; 3],
    /// The series' kept steps, used as they are (no refiltering).
    pub steps: SliceView<SunStep>,
    /// Minutes per step.
    pub timestep_minutes: i32,
    /// Steps before the horizon filter.
    pub source_step_count: usize,
    /// The series' content version.
    pub series_version: u64,
    /// Samples the study measures.
    pub sample_count: usize,
}

/// Point the session at a scene, day and sample count.
///
/// # Safety
/// The handle must be live; the request and its steps must be live and
/// immutable for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_session_sync(
    handle: *const SessionHandle,
    request: *const SessionSyncRequest,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.check()?;
            let request = regions.context().record(request)?;
            regions.input(request.steps.data, request.steps.count);
            regions.check()?;
            let steps = regions.context().slice(request.steps)?.to_vec();
            let series = SunSeries::from_parts(
                steps,
                request.timestep_minutes,
                request.source_step_count,
                request.series_version,
            );
            let [geometry_version, sun_version, grid_version] = request.versions;
            let inputs = StudyInputs {
                geometry_version,
                sun_version,
                grid_version,
            };
            session(handle)?
                .sync(inputs, &series, request.sample_count)
                .map_err(poisoned)
        })
    }
}

/// Seeding inputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SessionSeedRequest {
    /// A converged session to copy from.
    pub source: *const SessionHandle,
    /// Source sample per sample; `SIZE_MAX` traces afresh.
    pub map: SliceView<usize>,
    /// Optional sample-major dirty-step words.
    pub dirty: SliceView<u64>,
}

/// Seed reusable bits before the first advance; `accepted` 0 means refused.
///
/// # Safety
/// Both handles must be live; the request and arrays live and immutable;
/// `accepted` writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_session_seed(
    handle: *const SessionHandle,
    request: *const SessionSeedRequest,
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
            let context = regions.context();
            let ok = session(handle)?
                .seed_reusable(
                    session(request.source)?,
                    context.slice(request.map)?,
                    context.slice(request.dirty)?,
                )
                .map_err(poisoned)?;
            accepted.write(u8::from(ok));
            Ok(())
        })
    }
}

/// An optional per-step observer: step index, wall milliseconds and the
/// accumulator's counters after the step.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StepObserverCallback {
    /// Passed back to `observe`.
    pub context: *mut core::ffi::c_void,
    /// Null for no observer. Must not unwind or call into the session.
    pub observe: Option<
        unsafe extern "C" fn(
            context: *mut core::ffi::c_void,
            step: usize,
            wall_ms: f64,
            state: *const OcclusionState,
        ),
    >,
}

/// `Advance` inputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SessionAdvanceRequest {
    /// Samples, traversal and trace settings (sample count must match `sync`).
    pub trace: OcclusionTrace,
    /// The caller's step budget.
    pub max_steps: usize,
    /// Optional observer.
    pub observer: StepObserverCallback,
}

/// Resolve up to `max_steps` more steps; `resolved` receives how many (0 also
/// when a concurrent advance holds the session).
///
/// # Safety
/// The handle must be live; the request, vtable, observer context and arrays
/// must be live for the call; `resolved` writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_session_advance(
    handle: *const SessionHandle,
    request: *const SessionAdvanceRequest,
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
            register_trace(&mut regions, &request.trace);
            regions.check()?;
            let context = regions.context();
            let prepared = prepare(&context, &request.trace)?;
            let poll = || prepared.traversal.cancelled();
            let options = AdvanceOptions {
                max_steps: request.max_steps,
                step: StepOptions {
                    tmin: prepared.tmin,
                    tmax: prepared.tmax,
                    max_parallel: prepared.max_parallel,
                    is_cancelled: Some(&poll),
                },
            };
            let callback = request.observer;
            let mut observe = |step: usize, accumulator: &OcclusionAccumulator, wall_ms: f64| {
                if let Some(observe) = callback.observe {
                    let state = state_of(accumulator);
                    // SAFETY: the caller guarantees the callback/context for
                    // the call and that it neither unwinds nor re-enters.
                    observe(callback.context, step, wall_ms, &raw const state);
                }
            };
            let done = session(handle)?
                .advance(
                    &prepared.traversal,
                    &prepared.samples,
                    &options,
                    Some(&mut observe),
                )
                .map_err(poisoned)?;
            resolved.write(done);
            Ok(())
        })
    }
}

/// Progress, the publish gate and the accumulator's counters.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SessionProgress {
    /// Reset generation.
    pub generation: u64,
    /// Steps resolved.
    pub resolved_steps: usize,
    /// Steps in the day.
    pub total_steps: usize,
    /// Samples.
    pub sample_count: usize,
    /// The accumulator's counters.
    pub accumulator: OcclusionState,
    /// 1 when complete and non-empty.
    pub converged: u8,
    /// 1 when there was nothing to study.
    pub empty: u8,
    /// 1 while an advance is running (diagnostics only).
    pub advancing: u8,
}

/// Read progress.
///
/// # Safety
/// The handle must be live; `out` writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_session_progress(
    handle: *const SessionHandle,
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
            let s = session(handle)?;
            let advancing = s.advancing();
            let p = s.progress().map_err(poisoned)?;
            let accumulator = s.with_accumulator(state_of).map_err(poisoned)?;
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

/// Copy the bitset, resolved flags and/or sun hours out. `hours_per_step` in
/// the descriptor is ignored: hours use the session's own day.
///
/// # Safety
/// As `tapioca_sunstudy_occlusion_read`, with a live session handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_session_read(
    handle: *const SessionHandle,
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
            let s = session(handle)?;
            let hours = if target.hours_capacity == 0 {
                Vec::new()
            } else {
                s.sun_hours().map_err(poisoned)?
            };
            s.with_accumulator(|a| -> Result<(), Failure> {
                let wants = |capacity: usize, needed: usize| match capacity {
                    0 => Ok(false),
                    c if c >= needed => Ok(true),
                    _ => Err(Failure::Argument("SessionReadOutputTooSmall")),
                };
                let (bits, flags) = (a.bits(), a.steps_resolved());
                let (want_bits, want_flags, want_hours) = (
                    wants(target.bit_capacity, bits.len())?,
                    wants(target.step_capacity, flags.len())?,
                    wants(target.hours_capacity, hours.len())?,
                );
                // Each length was checked against its disjoint capacity above;
                // these writes run inside this export's outer unsafe block.
                if want_bits && !bits.is_empty() {
                    std::ptr::copy_nonoverlapping(bits.as_ptr(), target.bits, bits.len());
                }
                if want_flags {
                    for (i, flag) in flags.iter().enumerate() {
                        target.steps_resolved.add(i).write(u8::from(*flag));
                    }
                }
                if want_hours && !hours.is_empty() {
                    std::ptr::copy_nonoverlapping(hours.as_ptr(), target.sun_hours, hours.len());
                }
                Ok(())
            })
            .map_err(poisoned)?
        })
    }
}
