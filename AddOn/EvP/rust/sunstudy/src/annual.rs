//! Annual direct sun: which days a period samples, what each one weighs, and
//! the per-sample monthly totals they add up to (#152, stage B).
//!
//! The host traces each scheduled day with the ordinary one-day study and hands
//! the finished day here; nothing in this module traces or computes a solar
//! position. A year of step masks is never kept: a day contributes its hours
//! and is dropped.
//!
//! Quantities, per sample and month, from one integration schedule:
//!
//! `0 <= unoccluded <= surface_facing <= site_daylight`
//!
//! - **site daylight**: hours the sun is above the study's altitude floor,
//!   one scalar per month shared by every sample;
//! - **surface facing**: of those, hours the sun is in front of the sample's
//!   surface (incidence > 0, the day engine's own cull);
//! - **unoccluded**: of those, hours with a clear ray to the sun.
//!
//! `obstruction loss = surface facing - unoccluded`. Back-facing time is not
//! obstruction loss and is never credited as shade.
//!
//! Sampling: [`AnnualSampling::Preview`] traces one representative day per
//! covered month and weights it by the covered days it stands for; it is an
//! estimate. [`AnnualSampling::DailyResolved`] traces every covered day at
//! weight one; it still depends on the timestep and the sample grid, so it is
//! not called exact.

use crate::occlusion::SampleSet;
use crate::series::SunStep;
use crate::summary::{SunStudyResultSummary, summarize_sun_hours};
use std::cmp::Ordering;

/// Months in a year: the bins every per-sample array is laid out by.
pub const MONTHS: usize = 12;

/// Why an annual input was refused.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AnnualError {
    /// Not a calendar date (month 1-12, day within the month, year 1-9999).
    InvalidDate,
    /// The range runs backwards or crosses a year boundary.
    InvalidRange,
    /// A month outside 1-12.
    InvalidMonth,
    /// A weight that is not a positive finite number of days.
    InvalidWeight,
    /// A day's hours do not have one finite, non-negative value per sample.
    InvalidHours,
    /// A day's samples are not the samples the first day was traced on.
    ReceptorsChanged,
    /// A sample lit for longer than it faced the sun: the day and its steps
    /// are not one study.
    InconsistentDay,
    /// The per-sample arrays could not be allocated.
    OutOfMemory,
}

/// A day of the proleptic Gregorian calendar.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct CalendarDate {
    /// Year, 1-9999.
    pub year: i32,
    /// Month, 1-12.
    pub month: u8,
    /// Day of the month, 1-31.
    pub day: u8,
}

/// True for a Gregorian leap year.
#[must_use]
pub fn is_leap(year: i32) -> bool {
    (year % 4 == 0 && year % 100 != 0) || year % 400 == 0
}

/// Days in `month` of `year`; `None` for a month outside 1-12.
#[must_use]
pub fn days_in_month(year: i32, month: u8) -> Option<u8> {
    const DAYS: [u8; MONTHS] = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];
    let days = *DAYS.get(usize::from(month).checked_sub(1)?)?;
    Some(if month == 2 && is_leap(year) {
        29
    } else {
        days
    })
}

impl CalendarDate {
    /// A validated date.
    ///
    /// # Errors
    /// [`AnnualError::InvalidDate`] for anything that is not a calendar day.
    pub fn new(year: i32, month: u8, day: u8) -> Result<Self, AnnualError> {
        let last = days_in_month(year, month).ok_or(AnnualError::InvalidDate)?;
        if !(1..=9999).contains(&year) || day == 0 || day > last {
            return Err(AnnualError::InvalidDate);
        }
        Ok(Self { year, month, day })
    }

    /// Zero-based month bin.
    #[must_use]
    pub fn month_index(self) -> usize {
        usize::from(self.month).saturating_sub(1)
    }
}

/// An inclusive span of days inside one calendar year.
///
/// One year, because the per-sample bins are calendar months: a span from
/// December into January would put two Januaries into one bin.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DateRange {
    from: CalendarDate,
    to: CalendarDate,
}

impl DateRange {
    /// `from..=to`.
    ///
    /// # Errors
    /// [`AnnualError::InvalidRange`] when `to` precedes `from` or the years differ.
    pub fn new(from: CalendarDate, to: CalendarDate) -> Result<Self, AnnualError> {
        if from.year != to.year || to < from {
            return Err(AnnualError::InvalidRange);
        }
        Ok(Self { from, to })
    }

    /// January 1 to December 31 of `year`.
    ///
    /// # Errors
    /// [`AnnualError::InvalidDate`] for a year outside 1-9999.
    pub fn whole_year(year: i32) -> Result<Self, AnnualError> {
        Self::new(
            CalendarDate::new(year, 1, 1)?,
            CalendarDate::new(year, 12, 31)?,
        )
    }

    /// First day.
    #[must_use]
    pub fn from(&self) -> CalendarDate {
        self.from
    }

    /// Last day, included.
    #[must_use]
    pub fn to(&self) -> CalendarDate {
        self.to
    }

    /// The covered days of `month` (1-12) as `(first, last)`, inclusive;
    /// `None` when the range does not reach that month.
    #[must_use]
    pub fn covered_days(&self, month: u8) -> Option<(u8, u8)> {
        if month < self.from.month || month > self.to.month {
            return None;
        }
        let first = if month == self.from.month {
            self.from.day
        } else {
            1
        };
        let last = if month == self.to.month {
            self.to.day
        } else {
            days_in_month(self.from.year, month)?
        };
        Some((first, last))
    }

    /// Calendar days covered, the range's total weight.
    #[must_use]
    pub fn day_count(&self) -> u32 {
        (1..=12_u8)
            .filter_map(|m| self.covered_days(m))
            .map(|(first, last)| u32::from(last - first) + 1)
            .sum()
    }
}

/// How densely a period is traced.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AnnualSampling {
    /// One representative day per covered month, weighted by its covered
    /// days. An estimate.
    Preview,
    /// Every covered day at weight one.
    DailyResolved,
}

/// One day to trace and the calendar days it stands for.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ScheduledDay {
    /// The day traced.
    pub date: CalendarDate,
    /// Calendar days this day represents, all in its own month.
    pub weight_days: f64,
}

/// The days `sampling` traces over `range`, in date order.
///
/// A preview's representative day is the middle covered day of each month
/// (the earlier of two middles), so it always lies in the interval it
/// represents, and a partial month is weighted by its covered days only.
#[must_use]
pub fn schedule(range: &DateRange, sampling: AnnualSampling) -> Vec<ScheduledDay> {
    let year = range.from.year;
    let mut days = Vec::new();
    for month in 1..=12_u8 {
        let Some((first, last)) = range.covered_days(month) else {
            continue;
        };
        match sampling {
            AnnualSampling::Preview => {
                let middle = first + (last - first) / 2;
                days.push(ScheduledDay {
                    date: CalendarDate {
                        year,
                        month,
                        day: middle,
                    },
                    weight_days: f64::from(last - first) + 1.0,
                });
            }
            AnnualSampling::DailyResolved => {
                days.extend((first..=last).map(|day| ScheduledDay {
                    date: CalendarDate { year, month, day },
                    weight_days: 1.0,
                }));
            }
        }
    }
    days
}

/// One traced day, as the day study left it.
pub struct DayContribution<'a> {
    /// Month bin, 1-12.
    pub month: u8,
    /// Calendar days the day represents.
    pub weight_days: f64,
    /// The day's kept steps (after the horizon filter), unit directions.
    pub steps: &'a [SunStep],
    /// Hours per step.
    pub hours_per_step: f64,
    /// The day's samples.
    pub samples: SampleSet<'a>,
    /// Unoccluded hours per sample that day.
    pub lit_hours: &'a [f64],
}

/// Which per-sample quantity to read.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AnnualQuantity {
    /// Hours with a clear ray to the sun.
    Unoccluded,
    /// Hours the sun was in front of the surface.
    SurfaceFacing,
    /// Facing hours lost to an obstruction.
    ObstructionLoss,
}

/// The totals so far.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct AnnualSummary {
    /// Days traced (empty days included).
    pub sampled_days: usize,
    /// Calendar days those traced days represent.
    pub represented_days: f64,
    /// Unoccluded hours over the whole period: count, min/mean/max and the
    /// site daylight as the ceiling (`daylight_hours`).
    pub unoccluded: SunStudyResultSummary,
}

const FNV_OFFSET: u64 = 14_695_981_039_346_656_037;
const FNV_PRIME: u64 = 1_099_511_628_211;

/// FNV-1a over the samples' coordinate bits: the receptors' identity across
/// days. Positions and normals both count; an absent normal array hashes as
/// a marker, not as zeros.
fn receptor_fingerprint(samples: &SampleSet<'_>) -> u64 {
    let mut hash = FNV_OFFSET;
    let mut feed = |value: f64| {
        for byte in value.to_bits().to_le_bytes() {
            hash ^= u64::from(byte);
            hash = hash.wrapping_mul(FNV_PRIME);
        }
    };
    samples.positions.iter().copied().for_each(&mut feed);
    match samples.normals {
        Some(normals) => normals.iter().copied().for_each(&mut feed),
        None => feed(f64::NAN),
    }
    if hash == 0 { 1 } else { hash }
}

/// The day engine's cull, `!(incidence <= 0)`: a NaN incidence counts as
/// facing, because the engine traces it.
fn faces(normal: Option<&[f64]>, sun: &[f64; 3]) -> bool {
    match normal {
        None => true,
        Some([x, y, z]) => (x * sun[0] + y * sun[1] + z * sun[2])
            .partial_cmp(&0.0)
            .is_none_or(Ordering::is_gt),
        Some(_) => false,
    }
}

fn zeroed(len: usize) -> Result<Vec<f64>, AnnualError> {
    let mut values = Vec::new();
    values
        .try_reserve_exact(len)
        .map_err(|_| AnnualError::OutOfMemory)?;
    values.resize(len, 0.0);
    Ok(values)
}

/// Per-sample, per-month totals of a period; sample-major `[sample][month]`.
///
/// Memory is two `f64` per sample and month (192 bytes a sample), whatever
/// the period's length. Sums run in day order, so equal inputs in equal order
/// give equal bits.
#[derive(Clone, Debug)]
pub struct AnnualAccumulator {
    sample_count: usize,
    fingerprint: u64,
    unoccluded: Vec<f64>,
    facing: Vec<f64>,
    site_daylight: [f64; MONTHS],
    represented: [f64; MONTHS],
    sampled_days: usize,
}

impl AnnualAccumulator {
    /// Zeroed totals for `sample_count` samples.
    ///
    /// # Errors
    /// [`AnnualError::OutOfMemory`] when the arrays cannot be allocated.
    pub fn new(sample_count: usize) -> Result<Self, AnnualError> {
        let len = sample_count
            .checked_mul(MONTHS)
            .ok_or(AnnualError::OutOfMemory)?;
        Ok(Self {
            sample_count,
            fingerprint: 0,
            unoccluded: zeroed(len)?,
            facing: zeroed(len)?,
            site_daylight: [0.0; MONTHS],
            represented: [0.0; MONTHS],
            sampled_days: 0,
        })
    }

    /// Samples measured.
    #[must_use]
    pub fn sample_count(&self) -> usize {
        self.sample_count
    }

    /// Days added so far, empty days included.
    #[must_use]
    pub fn sampled_days(&self) -> usize {
        self.sampled_days
    }

    /// Calendar days represented in each month bin.
    #[must_use]
    pub fn represented_days(&self) -> [f64; MONTHS] {
        self.represented
    }

    /// Site daylight hours in each month bin.
    #[must_use]
    pub fn site_daylight(&self) -> [f64; MONTHS] {
        self.site_daylight
    }

    fn check_weight(month: u8, weight_days: f64) -> Result<usize, AnnualError> {
        if !(1..=12).contains(&month) {
            return Err(AnnualError::InvalidMonth);
        }
        if !weight_days.is_finite() || weight_days <= 0.0 {
            return Err(AnnualError::InvalidWeight);
        }
        Ok(usize::from(month) - 1)
    }

    /// A day with no sun above the floor: it represents its days and adds
    /// zero hours. Not an error, and not a missing day.
    ///
    /// # Errors
    /// [`AnnualError::InvalidMonth`] or [`AnnualError::InvalidWeight`].
    pub fn add_sunless_day(&mut self, month: u8, weight_days: f64) -> Result<(), AnnualError> {
        let bin = Self::check_weight(month, weight_days)?;
        if let Some(days) = self.represented.get_mut(bin) {
            *days += weight_days;
        }
        self.sampled_days += 1;
        Ok(())
    }

    /// Add one traced day. Validates everything before changing anything, so
    /// a refused day leaves the totals as they were.
    ///
    /// # Errors
    /// [`AnnualError::InvalidMonth`], [`AnnualError::InvalidWeight`],
    /// [`AnnualError::InvalidHours`], [`AnnualError::ReceptorsChanged`] or
    /// [`AnnualError::InconsistentDay`].
    pub fn add_day(&mut self, day: &DayContribution<'_>) -> Result<(), AnnualError> {
        let bin = Self::check_weight(day.month, day.weight_days)?;
        let samples = &day.samples;
        if samples.count != self.sample_count
            || day.lit_hours.len() != self.sample_count
            || !day.hours_per_step.is_finite()
            || day.hours_per_step <= 0.0
            || day.lit_hours.iter().any(|h| !h.is_finite() || *h < 0.0)
        {
            return Err(AnnualError::InvalidHours);
        }
        let fingerprint = receptor_fingerprint(samples);
        if self.fingerprint != 0 && fingerprint != self.fingerprint {
            return Err(AnnualError::ReceptorsChanged);
        }
        // Facing time in the engine's own units, then the consistency check:
        // the engine never traces a back-facing step, so a lit total above
        // the facing total means the hours came from another day.
        let mut facing = Vec::new();
        facing
            .try_reserve_exact(self.sample_count)
            .map_err(|_| AnnualError::OutOfMemory)?;
        for (sample, lit) in day.lit_hours.iter().enumerate() {
            let normal = samples
                .normals
                .and_then(|n| n.get(sample * 3..sample * 3 + 3));
            let steps = day
                .steps
                .iter()
                .filter(|s| faces(normal, &s.direction))
                .count();
            let hours = crate::series::count_to_f64(steps) * day.hours_per_step;
            if *lit > hours + 1e-9 {
                return Err(AnnualError::InconsistentDay);
            }
            facing.push(hours);
        }
        self.fingerprint = fingerprint;
        let weight = day.weight_days;
        for (sample, (lit, face)) in day.lit_hours.iter().zip(&facing).enumerate() {
            let slot = sample * MONTHS + bin;
            if let Some(total) = self.unoccluded.get_mut(slot) {
                *total += weight * lit;
            }
            if let Some(total) = self.facing.get_mut(slot) {
                *total += weight * face;
            }
        }
        if let Some(total) = self.site_daylight.get_mut(bin) {
            *total += weight * crate::series::count_to_f64(day.steps.len()) * day.hours_per_step;
        }
        if let Some(days) = self.represented.get_mut(bin) {
            *days += weight;
        }
        self.sampled_days += 1;
        Ok(())
    }

    fn months(month: u8) -> Result<std::ops::Range<usize>, AnnualError> {
        match month {
            0 => Ok(0..MONTHS),
            1..=12 => {
                let bin = usize::from(month) - 1;
                Ok(bin..bin + 1)
            }
            _ => Err(AnnualError::InvalidMonth),
        }
    }

    /// One value per sample: `quantity` summed over `month` (1-12), or over
    /// the whole period for `month` 0. Months are summed in calendar order.
    ///
    /// # Errors
    /// [`AnnualError::InvalidMonth`] or [`AnnualError::OutOfMemory`].
    pub fn values(&self, quantity: AnnualQuantity, month: u8) -> Result<Vec<f64>, AnnualError> {
        let bins = Self::months(month)?;
        let mut out = Vec::new();
        out.try_reserve_exact(self.sample_count)
            .map_err(|_| AnnualError::OutOfMemory)?;
        let rows = self
            .unoccluded
            .as_chunks::<MONTHS>()
            .0
            .iter()
            .zip(self.facing.as_chunks::<MONTHS>().0);
        for (lit, facing) in rows {
            let sum = |row: &[f64]| row.get(bins.clone()).map_or(0.0, |r| r.iter().sum());
            out.push(match quantity {
                AnnualQuantity::Unoccluded => sum(lit),
                AnnualQuantity::SurfaceFacing => sum(facing),
                AnnualQuantity::ObstructionLoss => (sum(facing) - sum(lit)).max(0.0),
            });
        }
        Ok(out)
    }

    /// The site daylight over `month` (1-12) or the whole period (0): the
    /// ceiling every per-sample value of that selection stays under.
    ///
    /// # Errors
    /// [`AnnualError::InvalidMonth`].
    pub fn ceiling(&self, month: u8) -> Result<f64, AnnualError> {
        let bins = Self::months(month)?;
        Ok(self.site_daylight.get(bins).map_or(0.0, |b| b.iter().sum()))
    }

    /// Calendar days represented in `month` (1-12) or the whole period (0).
    ///
    /// # Errors
    /// [`AnnualError::InvalidMonth`].
    pub fn represented_in(&self, month: u8) -> Result<f64, AnnualError> {
        let bins = Self::months(month)?;
        Ok(self.represented.get(bins).map_or(0.0, |b| b.iter().sum()))
    }

    /// [`Self::values`] divided by the represented days of the selection: the
    /// mean hours a day, comparable with a one-day study and with periods of
    /// other lengths. All zero when the selection represents no day.
    ///
    /// # Errors
    /// [`AnnualError::InvalidMonth`] or [`AnnualError::OutOfMemory`].
    pub fn daily_means(
        &self,
        quantity: AnnualQuantity,
        month: u8,
    ) -> Result<Vec<f64>, AnnualError> {
        let days = self.represented_in(month)?;
        let mut values = self.values(quantity, month)?;
        if days > 0.0 {
            for v in &mut values {
                *v /= days;
            }
        } else {
            values.fill(0.0);
        }
        Ok(values)
    }

    /// [`Self::ceiling`] a day: the mean site daylight of the selection.
    ///
    /// # Errors
    /// [`AnnualError::InvalidMonth`].
    pub fn daily_ceiling(&self, month: u8) -> Result<f64, AnnualError> {
        let days = self.represented_in(month)?;
        Ok(if days > 0.0 {
            self.ceiling(month)? / days
        } else {
            0.0
        })
    }

    /// Sampled and represented days, and the whole period's unoccluded
    /// statistics against its site daylight.
    ///
    /// # Errors
    /// [`AnnualError::OutOfMemory`].
    pub fn summary(&self) -> Result<AnnualSummary, AnnualError> {
        let ceiling = self.ceiling(0)?;
        Ok(AnnualSummary {
            sampled_days: self.sampled_days,
            represented_days: self.represented.iter().sum(),
            unoccluded: summarize_sun_hours(&self.values(AnnualQuantity::Unoccluded, 0)?, ceiling),
        })
    }

    /// True when `samples` are the receptors the days were traced on (always
    /// true before the first traced day): the precondition for drawing these
    /// totals through another study's samples.
    #[must_use]
    pub fn same_receptors(&self, samples: &SampleSet<'_>) -> bool {
        samples.count == self.sample_count
            && (self.fingerprint == 0 || receptor_fingerprint(samples) == self.fingerprint)
    }

    /// The raw sample-major arrays: unoccluded, then surface facing.
    #[must_use]
    pub fn monthly(&self) -> (&[f64], &[f64]) {
        (&self.unoccluded, &self.facing)
    }
}

#[cfg(test)]
mod tests;
