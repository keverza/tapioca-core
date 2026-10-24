//! `tapioca_sunstudy_series`: the horizon filter, normalisation and version.

use super::Regions;
use crate::borrow::SliceView;
use crate::boundary::{Failure, call};
use tapioca_sunstudy::series::{SunSeries, SunStep};

/// Host-sampled steps and the filter settings.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SunSeriesRequest {
    /// Call-scoped steps in time order; directions need not be unit length.
    pub steps: SliceView<SunStep>,
    /// Minutes per step; values below 1 become 1.
    pub timestep_minutes: i32,
    /// Steps at or below this altitude (degrees) are dropped.
    pub min_altitude_degrees: f64,
}

/// What the series kept, published with the steps.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SunSeriesSummary {
    /// Steps written to the output array.
    pub step_count: usize,
    /// Steps in the request before the filter.
    pub source_step_count: usize,
    /// FNV-1a content hash; 0 only when no step was kept.
    pub version: u64,
    /// Effective minutes per step.
    pub timestep_minutes: i32,
    /// Hours per step.
    pub hours_per_step: f64,
    /// Kept steps x hours per step.
    pub daylight_hours: f64,
}

/// Caller-owned outputs, unchanged on refusal.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SunSeriesOutput {
    /// Kept steps, unit directions; null only at zero capacity.
    pub steps: *mut SunStep,
    /// Capacity in steps; must be at least the request's step count.
    pub capacity: usize,
    /// Required summary record.
    pub summary: *mut SunSeriesSummary,
}

/// Filter and normalise a day of host-sampled sun steps.
///
/// # Safety
/// The request, its step array and the output descriptor must be live,
/// initialized and immutable for the call. The output step capacity and the
/// summary must be live, aligned, exclusively writable and disjoint from every
/// other region including the error buffer. Shape checks cannot prove validity.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_series(
    request: *const SunSeriesRequest,
    output: *const SunSeriesOutput,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: the envelope checks the error range and contains unwinding; every
    // foreign region is checked before it is read or written.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.input(output, 1);
            regions.check()?;
            let context = regions.context();
            // SAFETY: descriptor shapes and error disjointness checked; POD copies.
            let (request, target) = (context.record(request)?, context.record(output)?);
            regions.input(request.steps.data, request.steps.count);
            regions.output(target.steps, target.capacity);
            regions.output(target.summary, 1);
            regions.check()?;
            if target.capacity < request.steps.count {
                return Err(Failure::Argument("SunSeriesOutputTooSmall"));
            }
            // SAFETY: checked immutable call-scoped input; no borrow survives.
            let steps = context.slice(request.steps)?;
            let series = SunSeries::from_steps(
                steps,
                request.timestep_minutes,
                request.min_altitude_degrees,
            );
            let summary = SunSeriesSummary {
                step_count: series.step_count(),
                source_step_count: series.source_step_count(),
                version: series.version(),
                timestep_minutes: series.timestep_minutes(),
                hours_per_step: series.hours_per_step(),
                daylight_hours: series.daylight_hours(),
            };
            let kept = series.steps();
            // SAFETY: kept.len() <= request count <= checked disjoint capacity.
            if !kept.is_empty() {
                std::ptr::copy_nonoverlapping(kept.as_ptr(), target.steps, kept.len());
            }
            target.summary.write(summary);
            Ok(())
        })
    }
}
