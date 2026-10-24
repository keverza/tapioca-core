//! A study that is advanced, not awaited.
//!
//! Port of `SunStudy/SunStudySession.cpp`. Partial results are the normal
//! state: `converged` is the only honest gate on an hours figure, and an empty
//! study (no samples or no sun) is not a converged one. `sync` resets only what
//! a changed version invalidates:
//!
//! | change   | resets                         |
//! |----------|--------------------------------|
//! | geometry | everything                     |
//! | sun      | the accumulator, adopts the day |
//! | grid     | the samples and accumulator    |
//!
//! One advance at a time is enforced here: a concurrent advance is REFUSED
//! (returns 0, which already means "nothing to do"), never queued. Unlike the
//! C++ session, which keeps non-owning sample pointers between calls, the Rust
//! session takes the samples on every `advance` and checks their count
//! against the one `sync` adopted.

use crate::occlusion::{OcclusionAccumulator, SampleSet, StepOptions, Traversal};
use crate::series::SunSeries;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, MutexGuard};
use std::time::Instant;

/// What a caller needs to draw progress and decide whether to publish.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct StudyProgress {
    /// Bumped whenever a reset discards accumulated work.
    pub generation: u64,
    /// Steps resolved.
    pub resolved_steps: usize,
    /// Steps in the day.
    pub total_steps: usize,
    /// Samples.
    pub sample_count: usize,
    /// Complete and non-empty: the only honest gate on an hours figure.
    pub converged: bool,
    /// No samples or no sun above the horizon.
    pub empty: bool,
}

/// The three versions `sync` compares; 0 means "no such input".
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct StudyInputs {
    /// Scene geometry version.
    pub geometry_version: u64,
    /// Sun series version.
    pub sun_version: u64,
    /// Sample grid version.
    pub grid_version: u64,
}

/// The session's mutex was poisoned by a panic; its contents are unspecified.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Poisoned;

/// Called after each completed step with the step index, the accumulator and
/// the step's wall time in milliseconds. Must not call back into the session.
pub type StepObserver<'a> = &'a mut dyn FnMut(usize, &OcclusionAccumulator, f64);

#[derive(Clone, Debug, Default)]
struct Inner {
    inputs: StudyInputs,
    series: SunSeries,
    sample_count: usize,
    accumulator: OcclusionAccumulator,
    generation: u64,
    next_step: usize,
    initialised: bool,
}

impl Inner {
    fn reset(&mut self) {
        self.accumulator = OcclusionAccumulator::new(self.sample_count, self.series.step_count());
        self.next_step = 0;
        self.generation += 1;
    }

    fn progress(&self) -> StudyProgress {
        let empty = self.sample_count == 0 || self.series.is_empty();
        StudyProgress {
            generation: self.generation,
            resolved_steps: self.accumulator.resolved_step_count(),
            total_steps: self.series.step_count(),
            sample_count: self.sample_count,
            converged: !empty && self.accumulator.complete(),
            empty,
        }
    }
}

/// Releases the advance guard when an advance ends, however it ends.
struct Release<'a>(&'a AtomicBool);

impl Drop for Release<'_> {
    fn drop(&mut self) {
        self.0.store(false, Ordering::Release);
    }
}

/// One study's day, samples count and accumulator.
#[derive(Debug, Default)]
pub struct SunStudySession {
    inner: Mutex<Inner>,
    advancing: AtomicBool,
}

/// Trace settings for an advance.
#[derive(Clone, Copy)]
pub struct AdvanceOptions<'a> {
    /// Most steps to resolve: the caller's budget, never second-guessed.
    pub max_steps: usize,
    /// Per-step trace settings and cancellation.
    pub step: StepOptions<'a>,
}

impl SunStudySession {
    fn lock(&self) -> Result<MutexGuard<'_, Inner>, Poisoned> {
        self.inner.lock().map_err(|_| Poisoned)
    }

    /// Point the session at a scene, a day and a sample count, resetting only
    /// what changed versions (or a changed count) invalidate. Cheap when
    /// nothing changed.
    ///
    /// # Errors
    /// [`Poisoned`] after a panic inside the session.
    pub fn sync(
        &self,
        inputs: StudyInputs,
        series: &SunSeries,
        sample_count: usize,
    ) -> Result<(), Poisoned> {
        let mut inner = self.lock()?;
        let changed = !inner.initialised
            || inputs.geometry_version != inner.inputs.geometry_version
            || inputs.sun_version != inner.inputs.sun_version
            || inputs.grid_version != inner.inputs.grid_version
            || sample_count != inner.sample_count;
        if changed {
            inner.inputs = inputs;
            inner.series = series.clone();
            inner.sample_count = sample_count;
            inner.initialised = true;
            inner.reset();
        }
        Ok(())
    }

    /// Seed reusable complete-day bits before the first advance. The source is
    /// copied first, so the two sessions are never locked together. A session
    /// cannot seed from itself.
    ///
    /// # Errors
    /// [`Poisoned`] after a panic inside either session.
    pub fn seed_reusable(
        &self,
        source: &Self,
        map: &[usize],
        dirty: &[u64],
    ) -> Result<bool, Poisoned> {
        if std::ptr::eq(self, source) {
            return Ok(false);
        }
        let source = source.accumulator()?;
        let mut inner = self.lock()?;
        if !inner.initialised
            || inner.next_step != 0
            || !inner.accumulator.seed_reusable(&source, map, dirty)
        {
            return Ok(false);
        }
        if inner.accumulator.complete() {
            inner.next_step = inner.series.step_count();
        }
        Ok(true)
    }

    /// Resolve up to `options.max_steps` more steps; returns how many. 0 means
    /// converged, nothing to do, a mismatched sample set, cancellation before
    /// the first step, or a concurrent advance that lost the race.
    ///
    /// # Errors
    /// [`Poisoned`] after a panic inside the session.
    pub fn advance(
        &self,
        traversal: &dyn Traversal,
        samples: &SampleSet<'_>,
        options: &AdvanceOptions<'_>,
        mut observer: Option<StepObserver<'_>>,
    ) -> Result<usize, Poisoned> {
        if self
            .advancing
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return Ok(0);
        }
        let _release = Release(&self.advancing);
        let mut inner = self.lock()?;
        if !inner.initialised
            || options.max_steps == 0
            || inner.sample_count == 0
            || inner.series.is_empty()
            || samples.count != inner.sample_count
        {
            return Ok(0);
        }
        let directions: Vec<[f64; 3]> = inner.series.steps().iter().map(|s| s.direction).collect();
        let mut resolved = 0;
        while resolved < options.max_steps && inner.next_step < inner.series.step_count() {
            if options.step.is_cancelled.is_some_and(|f| f()) {
                break;
            }
            let started = Instant::now();
            let step = inner.next_step;
            let done = inner.accumulator.accumulate_range(
                traversal,
                samples,
                (&directions, step, 1),
                &options.step,
            );
            if done == 0 {
                break;
            }
            inner.next_step += 1;
            resolved += 1;
            if let Some(observe) = observer.as_mut() {
                observe(
                    step,
                    &inner.accumulator,
                    started.elapsed().as_secs_f64() * 1000.0,
                );
            }
        }
        Ok(resolved)
    }

    /// Whether an advance is running right now (diagnostics only).
    #[must_use]
    pub fn advancing(&self) -> bool {
        self.advancing.load(Ordering::Acquire)
    }

    /// Progress and the publish gate.
    ///
    /// # Errors
    /// [`Poisoned`] after a panic inside the session.
    pub fn progress(&self) -> Result<StudyProgress, Poisoned> {
        Ok(self.lock()?.progress())
    }

    /// Sun hours per sample; too low until `progress().converged`.
    ///
    /// # Errors
    /// [`Poisoned`] after a panic inside the session.
    pub fn sun_hours(&self) -> Result<Vec<f64>, Poisoned> {
        let inner = self.lock()?;
        Ok(inner.accumulator.sun_hours(inner.series.hours_per_step()))
    }

    /// A copy of the accumulator (the resident result every display reads).
    ///
    /// # Errors
    /// [`Poisoned`] after a panic inside the session.
    pub fn accumulator(&self) -> Result<OcclusionAccumulator, Poisoned> {
        Ok(self.lock()?.accumulator.clone())
    }

    /// Run `read` against the accumulator without copying it.
    ///
    /// # Errors
    /// [`Poisoned`] after a panic inside the session.
    pub fn with_accumulator<R>(
        &self,
        read: impl FnOnce(&OcclusionAccumulator) -> R,
    ) -> Result<R, Poisoned> {
        Ok(read(&self.lock()?.accumulator))
    }

    /// The adopted day.
    ///
    /// # Errors
    /// [`Poisoned`] after a panic inside the session.
    pub fn series(&self) -> Result<SunSeries, Poisoned> {
        Ok(self.lock()?.series.clone())
    }

    /// Bumped whenever a reset discards work; stamps retire stale slices.
    ///
    /// # Errors
    /// [`Poisoned`] after a panic inside the session.
    pub fn generation(&self) -> Result<u64, Poisoned> {
        Ok(self.lock()?.generation)
    }
}

#[cfg(test)]
mod tests;
