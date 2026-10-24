//! Mirrors `tests/cpp/test_sunstudysession.cpp`.

use super::*;
use crate::occlusion::{DirectionalQuery, TraversalError};
use crate::series::SunStep;
use std::sync::atomic::AtomicUsize;
use std::time::Duration;

/// Occluded where x < 0; optionally slow, so two threads overlap.
struct Wall {
    delay: Duration,
    calls: AtomicUsize,
}

impl Wall {
    fn new() -> Self {
        Self {
            delay: Duration::ZERO,
            calls: AtomicUsize::new(0),
        }
    }
}

impl Traversal for Wall {
    fn occlude_directional(
        &self,
        query: &DirectionalQuery<'_>,
        out: &mut [u8],
    ) -> Result<bool, TraversalError> {
        self.calls.fetch_add(1, Ordering::SeqCst);
        std::thread::sleep(self.delay);
        for (slot, origin) in out.iter_mut().zip(query.origins.as_chunks::<3>().0) {
            *slot = u8::from(origin[0] < 0.0);
        }
        Ok(true)
    }

    fn scene_version(&self) -> u64 {
        1
    }
}

fn day(steps: usize) -> SunSeries {
    let raw: Vec<SunStep> = (0..steps)
        .map(|i| SunStep {
            hour: i32::try_from(i).unwrap_or(0),
            minute: 0,
            direction: [0.0, 0.0, 1.0],
            altitude_degrees: 30.0,
        })
        .collect();
    SunSeries::from_steps(&raw, 60, 0.0)
}

const POSITIONS: [f64; 6] = [-1.0, 0.0, 0.0, 1.0, 0.0, 0.0];

fn samples() -> SampleSet<'static> {
    SampleSet {
        positions: &POSITIONS,
        normals: None,
        count: 2,
    }
}

fn inputs(geometry: u64, sun: u64, grid: u64) -> StudyInputs {
    StudyInputs {
        geometry_version: geometry,
        sun_version: sun,
        grid_version: grid,
    }
}

fn advance(session: &SunStudySession, wall: &Wall, max_steps: usize) -> usize {
    let options = AdvanceOptions {
        max_steps,
        step: StepOptions::default(),
    };
    session
        .advance(wall, &samples(), &options, None)
        .unwrap_or(usize::MAX)
}

fn progress(session: &SunStudySession) -> StudyProgress {
    session.progress().unwrap_or_default()
}

#[test]
fn advances_in_bounded_slices_and_converges() {
    let session = SunStudySession::default();
    assert_eq!(session.sync(inputs(1, 1, 1), &day(5), 2), Ok(()));
    let wall = Wall::new();
    assert_eq!(advance(&session, &wall, 2), 2);
    assert!(!progress(&session).converged);
    assert_eq!(advance(&session, &wall, 10), 3);
    assert!(progress(&session).converged);
    assert_eq!(advance(&session, &wall, 10), 0);
    assert_eq!(session.sun_hours(), Ok(vec![0.0, 5.0]));
}

#[test]
fn a_zero_budget_and_advancing_before_sync_do_nothing() {
    let session = SunStudySession::default();
    let wall = Wall::new();
    assert_eq!(advance(&session, &wall, 3), 0);
    assert_eq!(session.sync(inputs(1, 1, 1), &day(3), 2), Ok(()));
    assert_eq!(advance(&session, &wall, 0), 0);
}

#[test]
fn unchanged_inputs_keep_every_resolved_step() {
    let session = SunStudySession::default();
    let wall = Wall::new();
    assert_eq!(session.sync(inputs(1, 1, 1), &day(4), 2), Ok(()));
    assert_eq!(advance(&session, &wall, 2), 2);
    let generation = progress(&session).generation;
    assert_eq!(session.sync(inputs(1, 1, 1), &day(4), 2), Ok(()));
    assert_eq!(progress(&session).resolved_steps, 2);
    assert_eq!(progress(&session).generation, generation);
}

#[test]
fn any_version_or_count_change_discards_the_accumulator() {
    for (next, count) in [
        ((2, 1, 1), 2),
        ((1, 2, 1), 2),
        ((1, 1, 2), 2),
        ((1, 1, 1), 3),
    ] {
        let session = SunStudySession::default();
        let wall = Wall::new();
        assert_eq!(session.sync(inputs(1, 1, 1), &day(4), 2), Ok(()));
        assert_eq!(advance(&session, &wall, 2), 2);
        let generation = progress(&session).generation;
        let (g, s, d) = next;
        assert_eq!(session.sync(inputs(g, s, d), &day(6), count), Ok(()));
        let p = progress(&session);
        assert_eq!(
            (p.resolved_steps, p.total_steps, p.sample_count),
            (0, 6, count)
        );
        assert_eq!(p.generation, generation + 1);
    }
}

#[test]
fn an_empty_study_is_not_a_converged_one() {
    let session = SunStudySession::default();
    assert_eq!(session.sync(inputs(1, 1, 1), &day(0), 2), Ok(()));
    let p = progress(&session);
    assert!(p.empty && !p.converged);
    let none = SunStudySession::default();
    assert_eq!(none.sync(inputs(1, 1, 1), &day(3), 0), Ok(()));
    assert!(progress(&none).empty && !progress(&none).converged);
}

#[test]
fn slice_size_does_not_change_the_answer() {
    let wall = Wall::new();
    let whole = SunStudySession::default();
    assert_eq!(whole.sync(inputs(1, 1, 1), &day(9), 2), Ok(()));
    assert_eq!(advance(&whole, &wall, 100), 9);
    let sliced = SunStudySession::default();
    assert_eq!(sliced.sync(inputs(1, 1, 1), &day(9), 2), Ok(()));
    while advance(&sliced, &wall, 2) > 0 {}
    assert_eq!(
        whole.accumulator().map(|a| a.bits().to_vec()),
        sliced.accumulator().map(|a| a.bits().to_vec())
    );
}

#[test]
fn a_mismatched_sample_set_resolves_nothing() {
    let session = SunStudySession::default();
    assert_eq!(session.sync(inputs(1, 1, 1), &day(3), 3), Ok(()));
    assert_eq!(advance(&session, &Wall::new(), 3), 0);
}

#[test]
fn concurrent_advances_do_not_both_claim_the_same_steps() {
    let session = SunStudySession::default();
    assert_eq!(session.sync(inputs(1, 1, 1), &day(6), 2), Ok(()));
    let wall = Wall {
        delay: Duration::from_millis(20),
        calls: AtomicUsize::new(0),
    };
    let (first, second) = std::thread::scope(|scope| {
        let a = scope.spawn(|| advance(&session, &wall, 3));
        std::thread::sleep(Duration::from_millis(5));
        let b = scope.spawn(|| advance(&session, &wall, 3));
        (a.join().unwrap_or(99), b.join().unwrap_or(99))
    });
    assert_eq!(first + second, 3, "the loser is refused, not queued");
    assert_eq!(progress(&session).resolved_steps, 3);
    assert_eq!(wall.calls.load(Ordering::SeqCst), 3);
    assert!(!session.advancing(), "the guard releases");
    assert_eq!(advance(&session, &wall, 3), 3);
}

#[test]
fn the_observer_sees_every_step() {
    let session = SunStudySession::default();
    assert_eq!(session.sync(inputs(1, 1, 1), &day(4), 2), Ok(()));
    let mut seen = Vec::new();
    let mut observe = |step: usize, acc: &OcclusionAccumulator, _: f64| {
        seen.push((step, acc.resolved_step_count()));
    };
    let options = AdvanceOptions {
        max_steps: 3,
        step: StepOptions::default(),
    };
    assert_eq!(
        session.advance(&Wall::new(), &samples(), &options, Some(&mut observe)),
        Ok(3)
    );
    assert_eq!(seen, [(0, 1), (1, 2), (2, 3)]);
}

#[test]
fn seeding_before_the_first_advance_and_not_from_itself() {
    let wall = Wall::new();
    let source = SunStudySession::default();
    assert_eq!(source.sync(inputs(1, 1, 1), &day(3), 2), Ok(()));
    assert_eq!(advance(&source, &wall, 3), 3);
    let target = SunStudySession::default();
    assert_eq!(target.sync(inputs(2, 1, 1), &day(3), 2), Ok(()));
    assert_eq!(target.seed_reusable(&target, &[0, 1], &[]), Ok(false));
    assert_eq!(target.seed_reusable(&source, &[0, 1], &[]), Ok(true));
    assert!(
        progress(&target).converged,
        "a fully reused study is complete"
    );
    assert_eq!(advance(&target, &wall, 3), 0);
}
