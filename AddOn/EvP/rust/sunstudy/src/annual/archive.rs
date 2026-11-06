//! Every traced day of a period, kept for playback (#152, opt-in).
//!
//! The accumulator keeps monthly totals and drops each day. A run that asks
//! for playback also keeps, per traced day, one bit per (sample, step) -- lit
//! or not -- and the count of steps the sun was in front of each sample. That
//! is what three play modes read:
//!
//! - **instant**: one step of one day, lit or not -- continuous playback runs
//!   through every step of every day, "fixed hour" through one clock time of
//!   every day;
//! - **day**: one day's hours (direct, in front, lost, access) per sample.
//!
//! Memory is `ceil(steps / 8) + 2` bytes per sample and day: about 11 bytes at
//! 15-minute steps, so 100 000 samples over 365 days is roughly 400 MB. It is
//! off unless the run asks, and [`DayArchive::bytes`] says what it holds.

use super::{AnnualError, AnnualQuantity, faces};
use crate::occlusion::SampleSet;
use crate::series::SunStep;

/// One kept day.
#[derive(Clone, Debug, Default)]
pub struct ArchivedDay {
    /// Month, 1-12.
    pub month: u8,
    /// Day of the month.
    pub day: u8,
    /// Calendar days it represents.
    pub weight_days: f64,
    /// Hours per step.
    pub hours_per_step: f64,
    /// Clock minute of each kept step (`hour * 60 + minute`).
    pub minutes: Vec<u16>,
    stride: usize,
    lit: Vec<u8>,
    facing: Vec<u16>,
}

impl ArchivedDay {
    fn lit(&self, sample: usize, step: usize) -> bool {
        self.lit
            .get(sample * self.stride + step / 8)
            .is_some_and(|byte| byte & (1 << (step % 8)) != 0)
    }

    fn lit_steps(&self, sample: usize) -> u32 {
        self.lit
            .get(sample * self.stride..(sample + 1) * self.stride)
            .map_or(0, |row| row.iter().map(|b| b.count_ones()).sum())
    }

    fn facing_steps(&self, sample: usize) -> u16 {
        self.facing.get(sample).copied().unwrap_or(0)
    }

    /// Hours the sun is up (above the floor, in the time slice) that day.
    #[must_use]
    pub fn daylight(&self) -> f64 {
        crate::series::count_to_f64(self.minutes.len()) * self.hours_per_step
    }
}

/// One traced day to keep.
#[derive(Clone, Copy)]
pub struct KeptDay<'a> {
    /// Month, 1-12.
    pub month: u8,
    /// Day of the month.
    pub day: u8,
    /// Calendar days it represents.
    pub weight_days: f64,
    /// The day's kept steps.
    pub steps: &'a [SunStep],
    /// Hours per step.
    pub hours_per_step: f64,
    /// The day's samples.
    pub samples: &'a SampleSet<'a>,
    /// One byte per (sample, step), sample-major, nonzero when lit
    /// (`StudyCore::step_bits`).
    pub lit: &'a [u8],
}

/// The kept days, in the order they were traced.
#[derive(Clone, Debug, Default)]
pub struct DayArchive {
    sample_count: usize,
    days: Vec<ArchivedDay>,
}

impl DayArchive {
    /// An empty archive for `sample_count` samples.
    #[must_use]
    pub fn new(sample_count: usize) -> Self {
        Self {
            sample_count,
            days: Vec::new(),
        }
    }

    /// The kept days.
    #[must_use]
    pub fn days(&self) -> &[ArchivedDay] {
        &self.days
    }

    /// Bytes held by the per-sample arrays.
    #[must_use]
    pub fn bytes(&self) -> usize {
        self.days
            .iter()
            .map(|d| d.lit.len() + d.facing.len() * 2 + d.minutes.len() * 2)
            .sum()
    }

    /// A day with no sun above the floor.
    pub fn push_sunless(&mut self, month: u8, day: u8, weight_days: f64) {
        self.days.push(ArchivedDay {
            month,
            day,
            weight_days,
            hours_per_step: 0.0,
            ..ArchivedDay::default()
        });
    }

    /// Keep a traced day.
    ///
    /// # Errors
    /// [`AnnualError::InvalidHours`] when the shapes disagree,
    /// [`AnnualError::OutOfMemory`] when the day cannot be kept.
    pub fn push(&mut self, kept: &KeptDay<'_>) -> Result<(), AnnualError> {
        let KeptDay {
            month,
            day,
            weight_days,
            steps,
            hours_per_step,
            samples,
            lit,
        } = *kept;
        let step_count = steps.len();
        if samples.count != self.sample_count
            || lit.len() != self.sample_count.saturating_mul(step_count)
            || u16::try_from(step_count).is_err()
        {
            return Err(AnnualError::InvalidHours);
        }
        let stride = step_count.div_ceil(8);
        let mut bits = Vec::new();
        bits.try_reserve_exact(stride * self.sample_count)
            .map_err(|_| AnnualError::OutOfMemory)?;
        bits.resize(stride * self.sample_count, 0_u8);
        let mut facing = Vec::new();
        facing
            .try_reserve_exact(self.sample_count)
            .map_err(|_| AnnualError::OutOfMemory)?;
        let rows = lit.chunks(step_count.max(1));
        for (sample, (row, out)) in rows.zip(bits.chunks_mut(stride.max(1))).enumerate() {
            for (step, flag) in row.iter().enumerate() {
                if *flag != 0
                    && let Some(byte) = out.get_mut(step / 8)
                {
                    *byte |= 1 << (step % 8);
                }
            }
            let normal = samples
                .normals
                .and_then(|n| n.get(sample * 3..sample * 3 + 3));
            let count = steps.iter().filter(|s| faces(normal, &s.direction)).count();
            facing.push(u16::try_from(count).unwrap_or(u16::MAX));
        }
        if step_count == 0 {
            facing.resize(self.sample_count, 0);
        }
        let minutes = steps
            .iter()
            .map(|s| u16::try_from(s.hour * 60 + s.minute).unwrap_or(0))
            .collect();
        self.days.push(ArchivedDay {
            month,
            day,
            weight_days,
            hours_per_step,
            minutes,
            stride,
            lit: bits,
            facing,
        });
        Ok(())
    }

    /// Drop the last kept day (the totals refused it).
    pub fn discard_last(&mut self) {
        self.days.pop();
    }

    /// One step of one day: 1 lit, 0 not (shaded, behind the surface, or a
    /// step past the day's last).
    ///
    /// # Errors
    /// [`AnnualError::InvalidRange`] past the last day.
    pub fn instant(&self, day: usize, step: usize) -> Result<Vec<f64>, AnnualError> {
        let kept = self.days.get(day).ok_or(AnnualError::InvalidRange)?;
        Ok((0..self.sample_count)
            .map(|s| {
                if step < kept.minutes.len() && kept.lit(s, step) {
                    1.0
                } else {
                    0.0
                }
            })
            .collect())
    }

    /// One day's `quantity` per sample: hours, or a 0-1 ratio for
    /// [`AnnualQuantity::AccessRatio`].
    ///
    /// # Errors
    /// [`AnnualError::InvalidRange`] past the last day.
    pub fn day(&self, day: usize, quantity: AnnualQuantity) -> Result<Vec<f64>, AnnualError> {
        let kept = self.days.get(day).ok_or(AnnualError::InvalidRange)?;
        let hours = |steps: f64| steps * kept.hours_per_step;
        Ok((0..self.sample_count)
            .map(|s| {
                let lit = f64::from(kept.lit_steps(s));
                let facing = f64::from(kept.facing_steps(s));
                match quantity {
                    AnnualQuantity::Unoccluded => hours(lit),
                    AnnualQuantity::SurfaceFacing => hours(facing),
                    AnnualQuantity::ObstructionLoss => hours((facing - lit).max(0.0)),
                    AnnualQuantity::AccessRatio => {
                        if facing > 0.0 {
                            (lit / facing).min(1.0)
                        } else {
                            0.0
                        }
                    }
                }
            })
            .collect())
    }

    /// One sample's lit flags for one day, a byte per step.
    #[must_use]
    pub fn sample_steps(&self, day: usize, sample: usize) -> Vec<u8> {
        self.days.get(day).map_or_else(Vec::new, |kept| {
            if sample >= self.sample_count {
                return Vec::new();
            }
            (0..kept.minutes.len())
                .map(|step| u8::from(kept.lit(sample, step)))
                .collect()
        })
    }
}
