//! How many hours of sun each sample gets: one bit per (sample, timestep).
//!
//! Port of `SunStudy/SunStudyOcclusion.cpp`. The bitset is the design: hours,
//! the shadow at an instant, a fan or a threshold are all reads of it. One step
//! at a time is the unit of progress; scratch buffers live in the accumulator so
//! thousands of small slices do not allocate per step. Back-facing samples are
//! culled before the ray (a factor of two on the whole study).

use crate::series::count_to_f64;
use std::time::Instant;

/// The one thing the analysis asks of a ray tracer. Implementations must be
/// safe for concurrent calls over an immutable scene.
pub trait Traversal {
    /// Many origins, one direction. `origins` is xyz-interleaved, `out` one
    /// byte per origin (1 occluded, 0 clear). `tmax <= 0` is unbounded and
    /// `max_parallel` 0 means "decide". Returns false when cancelled between
    /// packets: the whole timestep is then uncommitted.
    ///
    /// # Errors
    /// [`TraversalError`] when the backend failed (not a cancellation).
    fn occlude_directional(
        &self,
        query: &DirectionalQuery<'_>,
        out: &mut [u8],
    ) -> Result<bool, TraversalError>;

    /// Changes when the geometry does; 0 means "no scene".
    fn scene_version(&self) -> u64;
}

/// A backend failure, distinct from cancellation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TraversalError;

/// One directional occlusion batch.
#[derive(Clone, Copy)]
pub struct DirectionalQuery<'a> {
    /// xyz-interleaved origins.
    pub origins: &'a [f64],
    /// Direction toward the light; need not be unit length.
    pub direction: [f64; 3],
    /// Near distance, metres.
    pub tmin: f64,
    /// Far distance, metres; `<= 0` is unbounded.
    pub tmax: f64,
    /// 0 = decide, 1 = inline.
    pub max_parallel: usize,
    /// Polled between packets; `None` never cancels.
    pub is_cancelled: Option<&'a dyn Fn() -> bool>,
}

/// The analysis points (non-owning).
#[derive(Clone, Copy, Debug)]
pub struct SampleSet<'a> {
    /// xyz positions, already offset off their surface.
    pub positions: &'a [f64],
    /// xyz unit normals, or `None` for "always facing the sun".
    pub normals: Option<&'a [f64]>,
    /// Sample count.
    pub count: usize,
}

/// Per-step trace settings.
#[derive(Clone, Copy)]
pub struct StepOptions<'a> {
    /// Skip the surface a sample sits on, metres (C++ default 0.001).
    pub tmin: f64,
    /// Far distance; `<= 0` is unbounded (C++ default 0).
    pub tmax: f64,
    /// Passed through to the traversal.
    pub max_parallel: usize,
    /// Polled every 4096 samples and around the trace.
    pub is_cancelled: Option<&'a dyn Fn() -> bool>,
}

impl Default for StepOptions<'_> {
    fn default() -> Self {
        Self {
            tmin: 0.001,
            tmax: 0.0,
            max_parallel: 0,
            is_cancelled: None,
        }
    }
}

/// Why a step was not committed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StepError {
    /// Step out of range or a sample set that does not match.
    Refused,
    /// Cancelled; nothing changed.
    Cancelled,
    /// The traversal failed; nothing changed.
    Traversal,
}

/// Sentinel in a reuse map: trace this sample afresh.
pub const NO_REUSE: usize = usize::MAX;

const BITS: usize = 64;

/// The per-(sample, step) lit bitset and its progress.
#[derive(Clone, Debug, Default)]
pub struct OcclusionAccumulator {
    sample_count: usize,
    step_count: usize,
    words_per_sample: usize,
    resolved_count: usize,
    bits: Vec<u64>,
    step_resolved: Vec<bool>,
    selective: bool,
    active_samples: Vec<u32>,
    dirty_steps: Vec<u64>,
    compact_ms: f64,
    trace_ms: f64,
    front_origins: Vec<f64>,
    front_index: Vec<u32>,
    occluded: Vec<u8>,
}

fn cancelled(check: Option<&dyn Fn() -> bool>) -> bool {
    check.is_some_and(|f| f())
}

fn index_u32(value: usize) -> u32 {
    u32::try_from(value).unwrap_or(u32::MAX)
}

impl OcclusionAccumulator {
    /// A cleared accumulator for `sample_count` samples and `step_count` steps.
    #[must_use]
    pub fn new(sample_count: usize, step_count: usize) -> Self {
        let words_per_sample = step_count.div_ceil(BITS);
        Self {
            sample_count,
            step_count,
            words_per_sample,
            bits: vec![0; sample_count.saturating_mul(words_per_sample)],
            step_resolved: vec![false; step_count],
            ..Self::default()
        }
    }

    /// Samples.
    #[must_use]
    pub fn sample_count(&self) -> usize {
        self.sample_count
    }

    /// Steps.
    #[must_use]
    pub fn step_count(&self) -> usize {
        self.step_count
    }

    /// 64-bit words per sample.
    #[must_use]
    pub fn words_per_sample(&self) -> usize {
        self.words_per_sample
    }

    /// The raw bitset: sample-major, LSB of word 0 is step 0.
    #[must_use]
    pub fn bits(&self) -> &[u64] {
        &self.bits
    }

    /// Per-step resolved flags.
    #[must_use]
    pub fn steps_resolved(&self) -> &[bool] {
        &self.step_resolved
    }

    /// Steps resolved so far.
    #[must_use]
    pub fn resolved_step_count(&self) -> usize {
        self.resolved_count
    }

    /// Every step resolved (or there are none).
    #[must_use]
    pub fn complete(&self) -> bool {
        self.step_count == 0 || self.resolved_count == self.step_count
    }

    /// Samples still traced (all of them unless seeded).
    #[must_use]
    pub fn active_sample_count(&self) -> usize {
        if self.selective {
            self.active_samples.len()
        } else {
            self.sample_count
        }
    }

    /// Rays traced by the last step.
    #[must_use]
    pub fn last_ray_count(&self) -> usize {
        self.front_index.len()
    }

    /// Milliseconds the last step spent compacting.
    #[must_use]
    pub fn last_compact_milliseconds(&self) -> f64 {
        self.compact_ms
    }

    /// Milliseconds the last step spent tracing.
    #[must_use]
    pub fn last_trace_milliseconds(&self) -> f64 {
        self.trace_ms
    }

    /// Clear every bit, every step flag and any seeding.
    pub fn reset(&mut self) {
        self.bits.fill(0);
        self.step_resolved.fill(false);
        self.resolved_count = 0;
        self.selective = false;
        self.active_samples.clear();
        self.dirty_steps.clear();
    }

    fn word_mut(&mut self, sample: usize, word: usize) -> Option<&mut u64> {
        self.bits.get_mut(sample * self.words_per_sample + word)
    }

    fn dirty(&self, sample: usize, word: usize) -> u64 {
        self.dirty_steps
            .get(sample * self.words_per_sample + word)
            .copied()
            .unwrap_or(0)
    }

    fn valid_mask(&self, word: usize) -> u64 {
        let remaining = self.step_count - word * BITS;
        if remaining >= BITS {
            u64::MAX
        } else {
            (1_u64 << remaining) - 1
        }
    }

    fn seed_inputs_valid(&self, source: &Self, map: &[usize], dirty: &[u64]) -> bool {
        if !source.complete()
            || source.step_count != self.step_count
            || map.len() != self.sample_count
            || self.resolved_count != 0
            || (!dirty.is_empty() && dirty.len() != self.bits.len())
        {
            return false;
        }
        map.iter().enumerate().all(|(sample, &from)| {
            (from == NO_REUSE || from < source.sample_count)
                && (dirty.is_empty()
                    || (0..self.words_per_sample).all(|word| {
                        let valid = self.valid_mask(word);
                        let d = dirty
                            .get(sample * self.words_per_sample + word)
                            .copied()
                            .unwrap_or(0);
                        d & !valid == 0 && (from != NO_REUSE || d == valid)
                    }))
        })
    }

    /// Copy complete-day bits for proven unaffected samples; unmapped samples
    /// (and dirty steps, when given) alone are traced later. Returns false,
    /// changing nothing, for an incomplete or mismatched source, a started
    /// accumulator, or dirty words that are malformed or leave an unmapped
    /// sample partly clean.
    pub fn seed_reusable(&mut self, source: &Self, map: &[usize], dirty: &[u64]) -> bool {
        if !self.seed_inputs_valid(source, map, dirty) {
            return false;
        }
        dirty.clone_into(&mut self.dirty_steps);
        self.active_samples.clear();
        self.bits.fill(0);
        let words = self.words_per_sample;
        for (sample, &from) in map.iter().enumerate() {
            let source_words = (from != NO_REUSE)
                .then(|| source.bits.get(from * words..from * words + words))
                .flatten();
            let own = self.bits.get_mut(sample * words..sample * words + words);
            if let (Some(src), Some(dst)) = (source_words, own) {
                dst.copy_from_slice(src);
            }
            if self.clear_dirty(sample) || from == NO_REUSE {
                self.active_samples.push(index_u32(sample));
            }
        }
        self.selective = true;
        if self.active_samples.is_empty() {
            self.step_resolved.fill(true);
            self.resolved_count = self.step_count;
        }
        true
    }

    /// Clear a sample's dirty steps; true when any were dirty.
    fn clear_dirty(&mut self, sample: usize) -> bool {
        let mut any = false;
        for word in 0..self.words_per_sample {
            let d = self.dirty(sample, word);
            if let Some(bits) = self.word_mut(sample, word) {
                *bits &= !d;
            }
            any = any || d != 0;
        }
        any
    }

    fn active(&self, i: usize) -> usize {
        if self.selective {
            self.active_samples
                .get(i)
                .and_then(|s| usize::try_from(*s).ok())
                .unwrap_or(usize::MAX)
        } else {
            i
        }
    }

    fn mark_resolved(&mut self, step: usize) {
        if let Some(flag) = self.step_resolved.get_mut(step).filter(|f| !**f) {
            *flag = true;
            self.resolved_count += 1;
        }
    }

    /// Gather front-facing, step-dirty samples; false when cancelled.
    fn compact(
        &mut self,
        samples: &SampleSet<'_>,
        step: usize,
        sun: [f64; 3],
        check: Option<&dyn Fn() -> bool>,
    ) -> bool {
        let (word, bit) = (step / BITS, 1_u64 << (step % BITS));
        let mut origins = std::mem::take(&mut self.front_origins);
        let mut index = std::mem::take(&mut self.front_index);
        origins.clear();
        index.clear();
        let mut complete = true;
        for i in 0..self.active_sample_count() {
            if i % 4096 == 0 && cancelled(check) {
                complete = false;
                break;
            }
            let s = self.active(i);
            if !self.dirty_steps.is_empty() && self.dirty(s, word) & bit == 0 {
                continue;
            }
            // `!(incidence <= 0)`, as C++: a NaN incidence is traced, not culled.
            let facing = samples.normals.is_none_or(|normals| {
                normals.get(s * 3..s * 3 + 3).is_some_and(|n| match n {
                    [x, y, z] => (x * sun[0] + y * sun[1] + z * sun[2])
                        .partial_cmp(&0.0)
                        .is_none_or(std::cmp::Ordering::is_gt),
                    _ => false,
                })
            });
            if !facing {
                continue; // self-shadowed: bit stays clear
            }
            if let Some(p) = samples.positions.get(s * 3..s * 3 + 3) {
                origins.extend_from_slice(p);
                index.push(index_u32(s));
            }
        }
        self.front_origins = origins;
        self.front_index = index;
        complete
    }

    /// Clear this step's bit on every traced sample, then set it where clear.
    fn commit(&mut self, step: usize) {
        let (word, bit) = (step / BITS, 1_u64 << (step % BITS));
        for i in 0..self.active_sample_count() {
            let s = self.active(i);
            if !self.dirty_steps.is_empty() && self.dirty(s, word) & bit == 0 {
                continue;
            }
            if let Some(bits) = self.word_mut(s, word) {
                *bits &= !bit;
            }
        }
        let lit: Vec<usize> = self
            .front_index
            .iter()
            .zip(&self.occluded)
            .filter(|(_, occluded)| **occluded == 0)
            .filter_map(|(s, _)| usize::try_from(*s).ok())
            .collect();
        for s in lit {
            if let Some(bits) = self.word_mut(s, word) {
                *bits |= bit;
            }
        }
    }

    /// Resolve one timestep for every (active) sample. Re-resolving a step is
    /// allowed. Nothing changes unless the whole step completes.
    ///
    /// # Errors
    /// [`StepError`] for a refused, cancelled or failed step.
    pub fn accumulate_step(
        &mut self,
        traversal: &dyn Traversal,
        samples: &SampleSet<'_>,
        (step, sun): (usize, [f64; 3]),
        options: &StepOptions<'_>,
    ) -> Result<(), StepError> {
        if step >= self.step_count || samples.count != self.sample_count {
            return Err(StepError::Refused);
        }
        if cancelled(options.is_cancelled) {
            return Err(StepError::Cancelled);
        }
        if self.sample_count == 0 {
            self.mark_resolved(step);
            return Ok(());
        }
        if samples.positions.is_empty() {
            return Err(StepError::Refused);
        }
        let started = Instant::now();
        self.trace_ms = 0.0;
        if !self.compact(samples, step, sun, options.is_cancelled) {
            return Err(StepError::Cancelled);
        }
        let compacted = Instant::now();
        self.compact_ms = compacted.duration_since(started).as_secs_f64() * 1000.0;
        let traced = self.front_index.len();
        if traced > 0 {
            self.occluded.clear();
            self.occluded.resize(traced, 0);
            let query = DirectionalQuery {
                origins: &self.front_origins,
                direction: sun,
                tmin: options.tmin,
                tmax: options.tmax,
                max_parallel: options.max_parallel,
                is_cancelled: options.is_cancelled,
            };
            match traversal.occlude_directional(&query, &mut self.occluded) {
                Ok(true) => {}
                Ok(false) => return Err(StepError::Cancelled),
                Err(TraversalError) => return Err(StepError::Traversal),
            }
            self.trace_ms = compacted.elapsed().as_secs_f64() * 1000.0;
        } else {
            self.occluded.clear();
        }
        if cancelled(options.is_cancelled) {
            return Err(StepError::Cancelled);
        }
        self.commit(step);
        self.mark_resolved(step);
        Ok(())
    }

    /// Resolve `[first, first + max_steps)` of `directions` (unit sun vectors
    /// per series step), stopping at the first step that does not commit.
    /// Returns how many were resolved.
    pub fn accumulate_range(
        &mut self,
        traversal: &dyn Traversal,
        samples: &SampleSet<'_>,
        (directions, first, max_steps): (&[[f64; 3]], usize, usize),
        options: &StepOptions<'_>,
    ) -> usize {
        let limit = directions.len().min(self.step_count);
        let mut done = 0;
        for step in first..limit {
            if done >= max_steps {
                break;
            }
            let Some(sun) = directions.get(step).copied() else {
                break;
            };
            if self
                .accumulate_step(traversal, samples, (step, sun), options)
                .is_err()
            {
                break;
            }
            done += 1;
        }
        done
    }

    /// Whether `step` is resolved.
    #[must_use]
    pub fn step_resolved(&self, step: usize) -> bool {
        self.step_resolved.get(step).copied().unwrap_or(false)
    }

    /// Whether the sample saw the sun at `step`.
    #[must_use]
    pub fn lit(&self, sample: usize, step: usize) -> bool {
        if sample >= self.sample_count || step >= self.step_count {
            return false;
        }
        self.bits
            .get(sample * self.words_per_sample + step / BITS)
            .is_some_and(|w| w & (1 << (step % BITS)) != 0)
    }

    /// Resolved steps this sample was sunlit for.
    #[must_use]
    pub fn lit_step_count(&self, sample: usize) -> usize {
        if sample >= self.sample_count {
            return 0;
        }
        let base = sample * self.words_per_sample;
        self.bits
            .get(base..base + self.words_per_sample)
            .map_or(0, |words| {
                words
                    .iter()
                    .map(|w| usize::try_from(w.count_ones()).unwrap_or(0))
                    .sum()
            })
    }

    /// Sun hours per sample from the steps resolved so far. Too low until
    /// [`Self::complete`]: publish only complete results.
    #[must_use]
    pub fn sun_hours(&self, hours_per_step: f64) -> Vec<f64> {
        (0..self.sample_count)
            .map(|s| count_to_f64(self.lit_step_count(s)) * hours_per_step)
            .collect()
    }
}

#[cfg(test)]
mod tests;
