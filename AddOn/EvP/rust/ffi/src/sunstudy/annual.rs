//! The annual accumulator as a Rust-owned handle (#152).
//!
//! Handle contract: one thread at a time. `add_*` and `destroy` need exclusive
//! use; `info`, `values` and `present` only read it. A refused day writes
//! nothing and leaves the totals as they were. The day itself is read from a
//! live study handle, so no per-sample array crosses the boundary on the way
//! in.

use super::Regions;
use super::atlas::publish;
use super::study::{StudyHandle, failure, study};
use crate::boundary::{Failure, call, memory_range};
use tapioca_sunstudy::annual::threshold::{DAILY_BINS, DAILY_EDGES};
use tapioca_sunstudy::annual::{
    AnnualAccumulator, AnnualError, AnnualQuantity, AnnualSampling, CalendarDate, DateRange,
    DayContribution, MONTHS, schedule,
};
use tapioca_sunstudy::summary::SunStudyResultSummary;

/// Opaque accumulator handle.
pub struct AnnualHandle {
    accumulator: AnnualAccumulator,
}

fn refused(error: AnnualError) -> Failure {
    match error {
        AnnualError::InvalidDate => Failure::Argument("AnnualInvalidDate"),
        AnnualError::InvalidRange => Failure::Argument("AnnualInvalidRange"),
        AnnualError::InvalidMonth => Failure::Argument("AnnualInvalidMonth"),
        AnnualError::InvalidWeight => Failure::Argument("AnnualInvalidWeight"),
        AnnualError::InvalidHours => Failure::State("AnnualInvalidHours"),
        AnnualError::ReceptorsChanged => Failure::State("AnnualReceptorsChanged"),
        AnnualError::InconsistentDay => Failure::State("AnnualInconsistentDay"),
        AnnualError::OutOfMemory => Failure::State("AnnualOutOfMemory"),
    }
}

/// What a read or a presentation selects.
#[derive(Clone, Copy)]
struct Selection {
    /// 0 unoccluded, 1 surface facing, 2 obstruction loss, 3 access ratio,
    /// 4 days at or above `DAILY_EDGES[threshold]`.
    quantity: u8,
    month: u8,
    per_day: bool,
    threshold: u8,
}

const DAYS_AT_LEAST: u8 = 4;

fn hours_quantity(code: u8) -> Result<AnnualQuantity, Failure> {
    match code {
        0 => Ok(AnnualQuantity::Unoccluded),
        1 => Ok(AnnualQuantity::SurfaceFacing),
        2 => Ok(AnnualQuantity::ObstructionLoss),
        3 => Ok(AnnualQuantity::AccessRatio),
        _ => Err(Failure::Argument("AnnualQuantity")),
    }
}

/// The selection's ceiling: site daylight (a day's when `per_day`) for the
/// hour quantities, 1 for a ratio or a share of days, the represented days
/// for a day count.
fn ceiling_of(acc: &AnnualAccumulator, sel: Selection) -> Result<f64, Failure> {
    if sel.quantity == DAYS_AT_LEAST {
        if sel.month != 0 {
            return Err(Failure::Argument("AnnualDaysAtLeast: whole period only"));
        }
        return if sel.per_day {
            Ok(1.0)
        } else {
            acc.represented_in(0).map_err(refused)
        };
    }
    if hours_quantity(sel.quantity)? == AnnualQuantity::AccessRatio {
        acc.ceiling(sel.month).map_err(refused)?; // validates the month
        return Ok(1.0);
    }
    if sel.per_day {
        acc.daily_ceiling(sel.month)
    } else {
        acc.ceiling(sel.month)
    }
    .map_err(refused)
}

fn values_of(acc: &AnnualAccumulator, sel: Selection) -> Result<Vec<f64>, Failure> {
    if sel.quantity == DAYS_AT_LEAST {
        ceiling_of(acc, sel)?; // validates the month
        let mut days = acc
            .days_at_least(usize::from(sel.threshold))
            .map_err(refused)?;
        if sel.per_day {
            let total = acc.represented_in(0).map_err(refused)?;
            for value in &mut days {
                *value = if total > 0.0 { *value / total } else { 0.0 };
            }
        }
        return Ok(days);
    }
    let which = hours_quantity(sel.quantity)?;
    if sel.per_day {
        acc.daily_means(which, sel.month)
    } else {
        acc.values(which, sel.month)
    }
    .map_err(refused)
}

/// SAFETY: `handle` must be null or live, not destroyed during the call.
unsafe fn annual<'a>(handle: *const AnnualHandle) -> Result<&'a AnnualAccumulator, Failure> {
    memory_range(handle, 1)?;
    // SAFETY: non-null and aligned (checked); liveness is the caller's contract.
    unsafe { handle.as_ref() }
        .map(|h| &h.accumulator)
        .ok_or(Failure::Argument("AnnualHandle: null"))
}

/// SAFETY: as [`annual`], and no other thread may use the handle.
unsafe fn annual_mut<'a>(handle: *mut AnnualHandle) -> Result<&'a mut AnnualAccumulator, Failure> {
    memory_range(handle.cast_const(), 1)?;
    // SAFETY: non-null and aligned (checked); exclusivity is the caller's
    // contract.
    unsafe { handle.as_mut() }
        .map(|h| &mut h.accumulator)
        .ok_or(Failure::Argument("AnnualHandle: null"))
}

/// Zeroed totals for `sample_count` samples.
///
/// # Safety
/// `out` must be one writable pointer slot, disjoint from the error buffer.
/// Release with `tapioca_sunstudy_annual_destroy`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_annual_create(
    sample_count: usize,
    out: *mut *mut AnnualHandle,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; the output slot is checked first.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.output(out, 1);
            regions.check()?;
            let accumulator = AnnualAccumulator::new(sample_count).map_err(refused)?;
            out.write(Box::into_raw(Box::new(AnnualHandle { accumulator })));
            Ok(())
        })
    }
}

/// Release an accumulator. Null is accepted.
///
/// # Safety
/// `handle` must be null or a live handle no thread is using, never used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_annual_destroy(
    handle: *mut AnnualHandle,
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

/// One scheduled day's place in the period.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct AnnualDayRequest {
    /// Calendar days the day represents.
    pub weight_days: f64,
    /// Month bin, 1-12.
    pub month: u8,
}

/// Add a converged day study's hours. `study` null adds a sunless day (no sun
/// above the floor): its days are represented at zero hours.
///
/// # Safety
/// `handle` live and used by no other thread; `study` null or live; the
/// request readable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_annual_add_day(
    handle: *mut AnnualHandle,
    study_handle: *const StudyHandle,
    request: *const AnnualDayRequest,
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
            let accumulator = annual_mut(handle)?;
            if study_handle.is_null() {
                return accumulator
                    .add_sunless_day(request.month, request.weight_days)
                    .map_err(refused);
            }
            let core = study(study_handle)?;
            let progress = core.session().progress().map_err(|e| failure(e.into()))?;
            if !progress.converged {
                return Err(Failure::State("AnnualDayNotConverged"));
            }
            // A study already showing a period would hand back that period.
            if core.is_presented().map_err(failure)? {
                return Err(Failure::State("AnnualDayPresented"));
            }
            let lit = core.sun_hours().map_err(failure)?;
            accumulator
                .add_day(&DayContribution {
                    month: request.month,
                    weight_days: request.weight_days,
                    steps: core.series().steps(),
                    hours_per_step: core.series().hours_per_step(),
                    samples: core.samples(),
                    lit_hours: &lit,
                })
                .map_err(refused)
        })
    }
}

/// The period so far.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct AnnualInfo {
    /// Samples measured.
    pub sample_count: usize,
    /// Days added, sunless days included.
    pub sampled_days: usize,
    /// Calendar days represented.
    pub represented_days: f64,
    /// Site daylight hours per month.
    pub site_daylight: [f64; MONTHS],
    /// Calendar days represented per month.
    pub month_days: [f64; MONTHS],
    /// Unoccluded hours over the period against its site daylight.
    pub unoccluded: SunStudyResultSummary,
}

/// Read the period's shape and summary.
///
/// # Safety
/// `handle` live; `out` writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_annual_info(
    handle: *const AnnualHandle,
    out: *mut AnnualInfo,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; the output is checked first.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.output(out, 1);
            regions.check()?;
            let accumulator = annual(handle)?;
            let summary = accumulator.summary().map_err(refused)?;
            out.write(AnnualInfo {
                sample_count: accumulator.sample_count(),
                sampled_days: summary.sampled_days,
                represented_days: summary.represented_days,
                site_daylight: accumulator.site_daylight(),
                month_days: accumulator.represented_days(),
                unoccluded: summary.unoccluded,
            });
            Ok(())
        })
    }
}

/// Which values to read, and where.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AnnualValuesRequest {
    /// Destination, one `f64` per sample; capacity 0 reports the count only.
    pub values: *mut f64,
    /// Capacity in samples.
    pub capacity: usize,
    /// 0 unoccluded, 1 surface facing, 2 obstruction loss, 3 access ratio
    /// (0-1), 4 days with at least `DAILY_EDGES[threshold]` hours.
    pub quantity: u8,
    /// 0 the whole period, 1-12 one month (quantity 4: whole period only).
    pub month: u8,
    /// 1 for the mean a represented day (quantity 4: the share of days), 0
    /// for the total. Ignored by quantity 3.
    pub per_day: u8,
    /// Quantity 4: the index into `DAILY_EDGES`.
    pub threshold: u8,
}

/// One value per sample, shape-then-fill; `ceiling` is the site daylight of
/// the same selection.
///
/// # Safety
/// `handle` live; the request readable; `values` writable for its capacity;
/// `count` and `ceiling` writable; all disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_annual_values(
    handle: *const AnnualHandle,
    request: *const AnnualValuesRequest,
    count: *mut usize,
    ceiling: *mut f64,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.output(count, 1);
            regions.output(ceiling, 1);
            regions.check()?;
            let r = regions.context().record(request)?;
            regions.output(r.values, r.capacity);
            regions.check()?;
            let accumulator = annual(handle)?;
            let sel = Selection {
                quantity: r.quantity,
                month: r.month,
                per_day: crate::sunstudy::flag(r.per_day, "AnnualValuesRequest: per_day")?,
                threshold: r.threshold,
            };
            let top = ceiling_of(accumulator, sel)?;
            if r.capacity != 0 {
                if r.capacity < accumulator.sample_count() {
                    return Err(Failure::Argument("AnnualValuesOutputTooSmall"));
                }
                publish(&values_of(accumulator, sel)?, r.values);
            }
            count.write(accumulator.sample_count());
            ceiling.write(top);
            Ok(())
        })
    }
}

/// Draw a selection of the period through `study`'s samples and atlas
/// (`StudyCore::present`), as totals or (`per_day` 1) means a represented day,
/// every value and the ceiling multiplied by `scale` (a ratio drawn on an
/// hours ramp: 100 % at the ramp's top). The study must carry the receptors the days were
/// traced on.
///
/// # Safety
/// Both handles live; `study` not being destroyed during the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_annual_present(
    handle: *const AnnualHandle,
    study_handle: *const StudyHandle,
    quantity_code: u8,
    month: u8,
    per_day: u8,
    threshold: u8,
    scale: f64,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; both handles are checked.
    unsafe {
        call(error, error_capacity, || {
            let accumulator = annual(handle)?;
            let core = study(study_handle)?;
            if !accumulator.same_receptors(&core.samples()) {
                return Err(refused(AnnualError::ReceptorsChanged));
            }
            let sel = Selection {
                quantity: quantity_code,
                month,
                per_day: crate::sunstudy::flag(per_day, "AnnualPresent: per_day")?,
                threshold,
            };
            if !scale.is_finite() || scale <= 0.0 {
                return Err(Failure::Argument("AnnualPresent: scale"));
            }
            let top = ceiling_of(accumulator, sel)? * scale;
            let mut values = values_of(accumulator, sel)?;
            for value in &mut values {
                *value *= scale;
            }
            core.present(values, top).map_err(failure)
        })
    }
}

/// A period to schedule.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AnnualScheduleRequest {
    /// Year of both ends.
    pub year: i32,
    /// First day: month, day.
    pub from: [u8; 2],
    /// Last day, included: month, day.
    pub to: [u8; 2],
    /// 0 preview (one representative day per covered month), 1 every day.
    pub sampling: u8,
}

/// One day to trace.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct AnnualScheduledDay {
    /// Calendar days it represents, all in its own month.
    pub weight_days: f64,
    /// Year.
    pub year: i32,
    /// Month, 1-12.
    pub month: u8,
    /// Day of the month.
    pub day: u8,
}

/// The days a period traces, in date order; shape-then-fill. `represented`
/// is the period's calendar-day count.
///
/// # Safety
/// The request readable; `days` writable for `capacity`; `count` and
/// `represented` writable; all disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_annual_schedule(
    request: *const AnnualScheduleRequest,
    days: *mut AnnualScheduledDay,
    capacity: usize,
    count: *mut usize,
    represented: *mut u32,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.output(days, capacity);
            regions.output(count, 1);
            regions.output(represented, 1);
            regions.check()?;
            let r = regions.context().record(request)?;
            let [from_month, from_day] = r.from;
            let [to_month, to_day] = r.to;
            let range = DateRange::new(
                CalendarDate::new(r.year, from_month, from_day).map_err(refused)?,
                CalendarDate::new(r.year, to_month, to_day).map_err(refused)?,
            )
            .map_err(refused)?;
            let sampling = match r.sampling {
                0 => AnnualSampling::Preview,
                1 => AnnualSampling::DailyResolved,
                _ => return Err(Failure::Argument("AnnualSampling")),
            };
            let planned: Vec<AnnualScheduledDay> = schedule(&range, sampling)
                .iter()
                .map(|d| AnnualScheduledDay {
                    weight_days: d.weight_days,
                    year: d.date.year,
                    month: d.date.month,
                    day: d.date.day,
                })
                .collect();
            if capacity != 0 {
                if capacity < planned.len() {
                    return Err(Failure::Argument("AnnualScheduleOutputTooSmall"));
                }
                publish(&planned, days);
            }
            count.write(planned.len());
            represented.write(range.day_count());
            Ok(())
        })
    }
}

/// One sample's row for a tooltip or a graph, with the bin edges it is
/// binned by (so no caller keeps a second copy of the table).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct AnnualSampleRecord {
    /// Unoccluded hours per month.
    pub unoccluded: [f64; MONTHS],
    /// Surface-facing hours per month.
    pub facing: [f64; MONTHS],
    /// Lower edges of bins 1.., hours.
    pub edges: [f64; DAILY_EDGES.len()],
    /// Represented days per daily-hours bin.
    pub daily: [f32; DAILY_BINS],
    /// 1 when `sample` exists.
    pub found: u8,
}

/// Read one sample's row; `found` 0 (and zeros) past the last sample.
///
/// # Safety
/// `handle` live; `out` writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_annual_sample(
    handle: *const AnnualHandle,
    sample: usize,
    out: *mut AnnualSampleRecord,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; the output is checked first.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.output(out, 1);
            regions.check()?;
            let mut record = AnnualSampleRecord {
                edges: DAILY_EDGES,
                ..AnnualSampleRecord::default()
            };
            if let Some(row) = annual(handle)?.sample_row(sample) {
                record.unoccluded = row.unoccluded;
                record.facing = row.facing;
                record.daily = row.daily;
                record.found = 1;
            }
            out.write(record);
            Ok(())
        })
    }
}
