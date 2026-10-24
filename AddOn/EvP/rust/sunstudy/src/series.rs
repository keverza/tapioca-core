//! The day as a list of sun directions: timestep enumeration, the horizon
//! filter, the hours arithmetic and the content-hash version.
//!
//! Port of `SunStudy/SunSeries.cpp`. It computes no solar position and must not:
//! the host's place settings are the authority, sampled once per timestep by the
//! caller. A second formula would be a second answer.

/// Wall-clock time of one timestep.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TimeOfDay {
    /// Hour, 0-23.
    pub hour: i32,
    /// Minute, 0-59.
    pub minute: i32,
}

/// One timestep: when, and where the sun was.
///
/// `repr(C)` because the C ABI passes arrays of it unchanged.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SunStep {
    /// Hour of the timestep.
    pub hour: i32,
    /// Minute of the timestep.
    pub minute: i32,
    /// Vector pointing TO the sun in model space, already north-corrected by the
    /// host. Not necessarily unit length on input; the series normalises it.
    pub direction: [f64; 3],
    /// Solar altitude in degrees above the horizon.
    pub altitude_degrees: f64,
}

/// `(hour, minute)` across the day at the given step; `hour_to` is exclusive.
///
/// The step is at least one minute and the window is clamped to `0..24` hours.
/// Arithmetic is 64-bit, so extreme inputs that overflow `int` in the C++
/// original (undefined behaviour there) produce the clamped answer here.
#[must_use]
pub fn enumerate_timesteps(timestep_minutes: i32, hour_from: i32, hour_to: i32) -> Vec<TimeOfDay> {
    let lo = i64::from(hour_from.max(0)) * 60;
    let hi = i64::from(hour_to.min(24)) * 60;
    let step = i64::from(timestep_minutes.max(1));
    let mut steps = Vec::new();
    let mut total = lo;
    while total < hi {
        // total is in 0..1440, so both parts fit i32.
        steps.push(TimeOfDay {
            hour: minutes_to_i32(total / 60),
            minute: minutes_to_i32(total % 60),
        });
        total += step;
    }
    steps
}

#[expect(
    clippy::cast_possible_truncation,
    reason = "callers pass values derived from a 0..1440 minute counter"
)]
fn minutes_to_i32(value: i64) -> i32 {
    value as i32
}

/// Hours represented by one timestep.
#[must_use]
pub fn hours_per_step(timestep_minutes: i32) -> f64 {
    f64::from(timestep_minutes) / 60.0
}

/// The day's sun directions, above the horizon only.
#[derive(Clone, Debug, PartialEq)]
pub struct SunSeries {
    steps: Vec<SunStep>,
    timestep_minutes: i32,
    source_step_count: usize,
    version: u64,
}

impl Default for SunSeries {
    fn default() -> Self {
        Self {
            steps: Vec::new(),
            timestep_minutes: 60,
            source_step_count: 0,
            version: 0,
        }
    }
}

use std::cmp::Ordering;

const FNV_OFFSET: u64 = 14_695_981_039_346_656_037;
const FNV_PRIME: u64 = 1_099_511_628_211;

fn hash_bytes(hash: &mut u64, bytes: &[u8]) {
    for byte in bytes {
        *hash ^= u64::from(*byte);
        *hash = hash.wrapping_mul(FNV_PRIME);
    }
}

fn hash_int(hash: &mut u64, value: i64) {
    hash_bytes(hash, &value.to_le_bytes());
}

fn hash_double(hash: &mut u64, value: f64) {
    // Negative zero compares equal to zero but has different bytes; normalise it
    // so a sign-only difference does not read as a different day.
    let value = if value == 0.0 { 0.0 } else { value };
    hash_bytes(hash, &value.to_le_bytes());
}

impl SunSeries {
    /// Keep the steps whose altitude exceeds `min_altitude_degrees`, in order,
    /// with unit directions. A zero-length direction is dropped even above the
    /// horizon: normalising it would yield NaN and silence a whole timestep.
    ///
    /// The cutoff defaults to 0 degrees at the call sites; a higher floor biases
    /// every total down.
    #[must_use]
    pub fn from_steps(steps: &[SunStep], timestep_minutes: i32, min_altitude_degrees: f64) -> Self {
        let mut series = Self {
            steps: Vec::new(),
            timestep_minutes: timestep_minutes.max(1),
            source_step_count: steps.len(),
            version: 0,
        };
        for step in steps {
            // NaN altitude or floor is dropped, as `!(a > b)` does in C++.
            if step.altitude_degrees.partial_cmp(&min_altitude_degrees) != Some(Ordering::Greater) {
                continue;
            }
            let [x, y, z] = step.direction;
            let length_squared = x * x + y * y + z * z;
            if length_squared <= 0.0 {
                continue;
            }
            let length = length_squared.sqrt();
            series.steps.push(SunStep {
                direction: [x / length, y / length, z / length],
                ..*step
            });
        }
        if series.steps.is_empty() {
            return series; // version stays 0: "no sun above the horizon"
        }

        let mut hash = FNV_OFFSET;
        hash_int(&mut hash, i64::from(series.timestep_minutes));
        hash_int(&mut hash, count_to_i64(series.steps.len()));
        for step in &series.steps {
            hash_int(&mut hash, i64::from(step.hour));
            hash_int(&mut hash, i64::from(step.minute));
            for component in step.direction {
                hash_double(&mut hash, component);
            }
        }
        // Zero is reserved for "empty".
        series.version = if hash == 0 { 1 } else { hash };
        series
    }

    /// Rebuild a series that was already filtered (for example by the C++
    /// `SunSeries` or [`Self::from_steps`]) without filtering or renormalising
    /// its steps again, so their bits are kept exactly. `timestep_minutes` is
    /// raised to 1 as in `from_steps`.
    #[must_use]
    pub fn from_parts(
        steps: Vec<SunStep>,
        timestep_minutes: i32,
        source_step_count: usize,
        version: u64,
    ) -> Self {
        Self {
            steps,
            timestep_minutes: timestep_minutes.max(1),
            source_step_count,
            version,
        }
    }

    /// Steps kept after the horizon filter.
    #[must_use]
    pub fn steps(&self) -> &[SunStep] {
        &self.steps
    }

    /// Number of kept steps.
    #[must_use]
    pub fn step_count(&self) -> usize {
        self.steps.len()
    }

    /// True when no step survived the horizon filter.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.steps.is_empty()
    }

    /// Minutes per step, at least 1.
    #[must_use]
    pub fn timestep_minutes(&self) -> i32 {
        self.timestep_minutes
    }

    /// Hours represented by one step.
    #[must_use]
    pub fn hours_per_step(&self) -> f64 {
        hours_per_step(self.timestep_minutes)
    }

    /// What a fully sunlit sample would score: the denominator of every
    /// percentage a report prints.
    #[must_use]
    pub fn daylight_hours(&self) -> f64 {
        count_to_f64(self.steps.len()) * self.hours_per_step()
    }

    /// Steps the source had before the horizon filter.
    #[must_use]
    pub fn source_step_count(&self) -> usize {
        self.source_step_count
    }

    /// FNV-1a hash of the kept contents; 0 only for an empty series. A content
    /// hash cannot drift the way a manually bumped counter can.
    #[must_use]
    pub fn version(&self) -> u64 {
        self.version
    }
}

#[expect(
    clippy::cast_possible_wrap,
    reason = "mirrors the C++ static_cast<int64_t>(size()); a Vec length never exceeds isize::MAX"
)]
fn count_to_i64(count: usize) -> i64 {
    count as i64
}

#[expect(
    clippy::cast_precision_loss,
    reason = "mirrors the C++ static_cast<double>(size()); step counts are tiny"
)]
pub(crate) fn count_to_f64(count: usize) -> f64 {
    count as f64
}

#[cfg(test)]
mod tests;
