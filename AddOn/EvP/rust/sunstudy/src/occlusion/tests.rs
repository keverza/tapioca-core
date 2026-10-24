//! Mirrors `tests/cpp/test_sunstudyocclusion.cpp` with a scripted traversal.

use super::*;
use std::cell::Cell;

/// Occluded where `x < 0`; optionally cancels (returns `Ok(false)`) on a
/// given call, or fails.
struct Wall {
    calls: Cell<usize>,
    cancel_on: Option<usize>,
    fail: bool,
}

impl Wall {
    fn new() -> Self {
        Self {
            calls: Cell::new(0),
            cancel_on: None,
            fail: false,
        }
    }
}

impl Traversal for Wall {
    fn occlude_directional(
        &self,
        query: &DirectionalQuery<'_>,
        out: &mut [u8],
    ) -> Result<bool, TraversalError> {
        let call = self.calls.get();
        self.calls.set(call + 1);
        if self.fail {
            return Err(TraversalError);
        }
        if self.cancel_on == Some(call) {
            out.fill(1); // partial garbage must never be committed
            return Ok(false);
        }
        for (slot, origin) in out.iter_mut().zip(query.origins.as_chunks::<3>().0) {
            *slot = u8::from(origin[0] < 0.0 || query.direction[2] < 0.0);
        }
        Ok(true)
    }

    fn scene_version(&self) -> u64 {
        1
    }
}

const UP: [f64; 3] = [0.0, 0.0, 1.0];

fn line(count: usize) -> Vec<f64> {
    (0..count)
        .flat_map(|i| [count_to_f64(i) - count_to_f64(count) / 2.0, 0.0, 0.0])
        .collect()
}

fn set<'a>(positions: &'a [f64], normals: Option<&'a [f64]>) -> SampleSet<'a> {
    SampleSet {
        positions,
        normals,
        count: positions.len() / 3,
    }
}

fn step(
    acc: &mut OcclusionAccumulator,
    wall: &Wall,
    samples: &SampleSet<'_>,
    index: usize,
    sun: [f64; 3],
) -> Result<(), StepError> {
    acc.accumulate_step(wall, samples, (index, sun), &StepOptions::default())
}

#[test]
fn shadowed_and_sunlit_samples_are_distinguished() {
    let positions = line(4); // x = -2, -1, 0, 1
    let samples = set(&positions, None);
    let mut acc = OcclusionAccumulator::new(4, 1);
    assert_eq!(step(&mut acc, &Wall::new(), &samples, 0, UP), Ok(()));
    assert_eq!(
        (0..4).map(|s| acc.lit(s, 0)).collect::<Vec<_>>(),
        [false, false, true, true]
    );
}

#[test]
fn back_facing_samples_are_never_lit_and_never_traced() {
    let positions = line(2);
    let normals = [0.0, 0.0, -1.0, 0.0, 0.0, 1.0];
    let samples = set(&positions, Some(&normals));
    let mut acc = OcclusionAccumulator::new(2, 1);
    let positions_far = [5.0, 0.0, 0.0, 6.0, 0.0, 0.0];
    let far = set(&positions_far, Some(&normals));
    assert_eq!(step(&mut acc, &Wall::new(), &far, 0, UP), Ok(()));
    assert!(!acc.lit(0, 0) && acc.lit(1, 0));
    assert_eq!(acc.last_ray_count(), 1);
    let _ = samples;
}

#[test]
fn a_nan_incidence_is_traced_not_culled() {
    let positions = [5.0, 0.0, 0.0];
    let normals = [f64::NAN, 0.0, 0.0];
    let mut acc = OcclusionAccumulator::new(1, 1);
    assert_eq!(
        step(
            &mut acc,
            &Wall::new(),
            &set(&positions, Some(&normals)),
            0,
            UP
        ),
        Ok(())
    );
    assert_eq!(acc.last_ray_count(), 1);
}

#[test]
fn hours_count_resolved_steps_only_and_incomplete_reports_itself() {
    let positions = [5.0, 0.0, 0.0];
    let samples = set(&positions, None);
    let mut acc = OcclusionAccumulator::new(1, 4);
    let wall = Wall::new();
    for index in 0..2 {
        assert_eq!(step(&mut acc, &wall, &samples, index, UP), Ok(()));
    }
    assert!(!acc.complete());
    assert_eq!(acc.resolved_step_count(), 2);
    assert_eq!(acc.sun_hours(0.5), [1.0]);
}

#[test]
fn re_resolving_replaces_the_bit_with_the_new_answer() {
    let positions = [5.0, 0.0, 0.0];
    let samples = set(&positions, None);
    let mut acc = OcclusionAccumulator::new(1, 1);
    let wall = Wall::new();
    assert_eq!(step(&mut acc, &wall, &samples, 0, UP), Ok(()));
    assert_eq!(step(&mut acc, &wall, &samples, 0, UP), Ok(()));
    assert_eq!(acc.resolved_step_count(), 1);
    assert_eq!(step(&mut acc, &wall, &samples, 0, [0.0, 0.0, -1.0]), Ok(()));
    assert!(!acc.lit(0, 0));
}

#[test]
fn cancellation_during_traversal_preserves_previously_resolved_bits() {
    let positions = [5.0, 0.0, 0.0];
    let samples = set(&positions, None);
    let mut acc = OcclusionAccumulator::new(1, 2);
    assert_eq!(step(&mut acc, &Wall::new(), &samples, 0, UP), Ok(()));
    let cancelling = Wall {
        cancel_on: Some(0),
        ..Wall::new()
    };
    assert_eq!(
        step(&mut acc, &cancelling, &samples, 0, [0.0, 0.0, -1.0]),
        Err(StepError::Cancelled)
    );
    assert!(
        acc.lit(0, 0),
        "the cancelled re-resolution must not clear the bit"
    );
    assert_eq!(step(&mut acc, &cancelling, &samples, 1, UP), Ok(()));
    assert_eq!(acc.resolved_step_count(), 2);
}

#[test]
fn a_failed_traversal_changes_nothing() {
    let positions = [5.0, 0.0, 0.0];
    let samples = set(&positions, None);
    let mut acc = OcclusionAccumulator::new(1, 1);
    let failing = Wall {
        fail: true,
        ..Wall::new()
    };
    assert_eq!(
        step(&mut acc, &failing, &samples, 0, UP),
        Err(StepError::Traversal)
    );
    assert_eq!(acc.resolved_step_count(), 0);
}

#[test]
fn a_cancelled_empty_step_does_not_resolve() {
    let mut acc = OcclusionAccumulator::new(0, 1);
    let yes = || true;
    let options = StepOptions {
        is_cancelled: Some(&yes),
        ..StepOptions::default()
    };
    let samples = set(&[], None);
    assert_eq!(
        acc.accumulate_step(&Wall::new(), &samples, (0, UP), &options),
        Err(StepError::Cancelled)
    );
    assert!(!acc.step_resolved(0));
}

#[test]
fn range_accumulation_walks_the_series_in_slices() {
    let positions = [5.0, 0.0, 0.0];
    let samples = set(&positions, None);
    let directions = [UP; 5];
    let mut acc = OcclusionAccumulator::new(1, 5);
    let wall = Wall::new();
    assert_eq!(
        acc.accumulate_range(
            &wall,
            &samples,
            (&directions, 0, 2),
            &StepOptions::default()
        ),
        2
    );
    assert_eq!(
        acc.accumulate_range(
            &wall,
            &samples,
            (&directions, 2, 10),
            &StepOptions::default()
        ),
        3
    );
    assert!(acc.complete());
}

#[test]
fn more_than_sixty_four_steps_span_multiple_words() {
    let positions = [5.0, 0.0, 0.0];
    let samples = set(&positions, None);
    let mut acc = OcclusionAccumulator::new(1, 130);
    assert_eq!(acc.words_per_sample(), 3);
    let wall = Wall::new();
    for index in [0, 63, 64, 129] {
        assert_eq!(step(&mut acc, &wall, &samples, index, UP), Ok(()));
    }
    assert!([0, 63, 64, 129].iter().all(|s| acc.lit(0, *s)));
    assert_eq!(acc.lit_step_count(0), 4);
}

#[test]
fn reset_clears_everything_and_bad_calls_change_nothing() {
    let positions = [5.0, 0.0, 0.0];
    let samples = set(&positions, None);
    let mut acc = OcclusionAccumulator::new(1, 2);
    let wall = Wall::new();
    assert_eq!(step(&mut acc, &wall, &samples, 0, UP), Ok(()));
    assert_eq!(
        step(&mut acc, &wall, &samples, 2, UP),
        Err(StepError::Refused)
    );
    let two = [5.0, 0.0, 0.0, 6.0, 0.0, 0.0];
    assert_eq!(
        step(&mut acc, &wall, &set(&two, None), 1, UP),
        Err(StepError::Refused)
    );
    assert_eq!(acc.resolved_step_count(), 1);
    acc.reset();
    assert_eq!((acc.resolved_step_count(), acc.lit(0, 0)), (0, false));
}

#[test]
fn selective_reuse_traces_only_unmapped_samples_and_dirty_steps() {
    let positions = line(4);
    let samples = set(&positions, None);
    let wall = Wall::new();
    let mut source = OcclusionAccumulator::new(4, 70);
    let directions = [UP; 70];
    assert_eq!(
        source.accumulate_range(
            &wall,
            &samples,
            (&directions, 0, 70),
            &StepOptions::default()
        ),
        70
    );
    let mut seeded = OcclusionAccumulator::new(4, 70);
    // Sample 3 is new; sample 1 is dirty at step 65 only.
    let mut dirty = vec![0_u64; 8];
    if let Some(word) = dirty.get_mut(3) {
        *word = 1 << 1; // sample 1, word 1, step 65
    }
    // Sample 3 unmapped: fully dirty.
    for (index, word) in [(6, u64::MAX), (7, (1 << 6) - 1)] {
        if let Some(slot) = dirty.get_mut(index) {
            *slot = word;
        }
    }
    assert!(seeded.seed_reusable(&source, &[0, 1, 2, NO_REUSE], &dirty));
    assert_eq!(seeded.active_sample_count(), 2);
    assert!(seeded.lit(2, 3), "clean bits are copied");
    assert!(!seeded.lit(1, 65), "dirty bits are cleared until re-traced");
    assert_eq!(
        seeded.accumulate_range(
            &wall,
            &samples,
            (&directions, 0, 70),
            &StepOptions::default()
        ),
        70
    );
    assert_eq!(seeded.bits(), source.bits());
}

#[test]
fn seeding_rejects_bad_inputs_without_changing_bits() {
    let positions = line(2);
    let samples = set(&positions, None);
    let wall = Wall::new();
    let mut source = OcclusionAccumulator::new(2, 3);
    let _ = source.accumulate_range(&wall, &samples, (&[UP; 3], 0, 3), &StepOptions::default());
    let mut target = OcclusionAccumulator::new(2, 3);
    assert!(!target.seed_reusable(&source, &[0], &[]), "short map");
    assert!(
        !target.seed_reusable(&source, &[0, 9], &[]),
        "map out of range"
    );
    assert!(
        !target.seed_reusable(&source, &[0, NO_REUSE], &[0, 0b011]),
        "unmapped but partly clean"
    );
    assert!(
        !target.seed_reusable(&source, &[0, 1], &[0b1000, 0]),
        "dirty beyond the steps"
    );
    let mut incomplete = OcclusionAccumulator::new(2, 3);
    let _ = incomplete.accumulate_step(&wall, &samples, (0, UP), &StepOptions::default());
    assert!(
        !target.seed_reusable(&incomplete, &[0, 1], &[]),
        "incomplete source"
    );
    assert_eq!(target.bits(), [0, 0]);
    assert!(target.seed_reusable(&source, &[1, 0], &[]));
    assert!(target.complete(), "nothing active resolves every step");
}
