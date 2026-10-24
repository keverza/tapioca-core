//! Mirrors `tests/cpp/test_sunstudyfollower.cpp`, including the driver-rate
//! tick that caught the live deadlock and runaway; lockstep parity with the
//! C++ follower is in `tests/cpp/test_sunstudy_rust_follower.cpp`.

use super::*;

fn sig(geometry: u64) -> DependencySignature {
    DependencySignature {
        geometry,
        sun: 7,
        sampling: 11,
    }
}

fn adopted() -> Follower {
    let mut follower = Follower::default();
    follower.adopt("study-a", sig(41), 1000);
    follower
}

/// One tick of the C++ driver, with nothing left out.
fn driver_tick(
    f: &mut Follower,
    world: DependencySignature,
    now: i64,
    counts: &mut (u64, i32, i32),
) {
    f.observe(world, now);
    match f.state() {
        FollowState::DirtyVisible if f.should_start(now) => {
            counts.0 = f.note_started(world);
            counts.1 += 1;
        }
        FollowState::UpdatingVisible => counts.2 += 1,
        _ => {}
    }
}

#[test]
fn navigation_leaves_the_study_current() {
    let mut f = adopted();
    for tick in 0..100 {
        assert!(!f.observe(sig(41), 1000 + tick * 50));
        assert_eq!(f.state(), FollowState::Current);
    }
    assert_eq!(f.reruns(), 0);
    assert!(!f.should_start(101_000));
}

#[test]
fn an_undone_edit_becomes_current_again() {
    let mut f = adopted();
    assert!(f.observe(sig(42), 1100));
    assert_eq!(f.dirty_reason(), DirtyReason::Geometry);
    assert!(f.observe(sig(41), 1200));
    assert_eq!(f.state(), FollowState::Current);
}

#[test]
fn each_input_says_which_moved() {
    for (world, reason) in [
        (sig(42), DirtyReason::Geometry),
        (DependencySignature { sun: 8, ..sig(41) }, DirtyReason::Sun),
        (
            DependencySignature {
                sampling: 12,
                ..sig(41)
            },
            DirtyReason::Sampling,
        ),
    ] {
        let mut f = adopted();
        f.observe(world, 1100);
        assert_eq!(f.dirty_reason(), reason);
    }
}

#[test]
fn a_burst_produces_one_run_and_polls_do_not_postpone_it() {
    let mut f = adopted();
    let mut now = 1000;
    for edit in 0..20 {
        now += 50;
        f.observe(sig(42 + edit), now);
        f.observe(sig(42 + edit), now + 10);
        assert!(!f.should_start(now + 10));
    }
    assert!(!f.should_start(now + DEBOUNCE_MS - 1));
    assert!(f.should_start(now + DEBOUNCE_MS));
    assert_eq!(f.milliseconds_until_start(now + 100), DEBOUNCE_MS - 100);
    f.note_started(sig(61));
    assert_eq!(f.reruns(), 1);
    assert_eq!(f.milliseconds_until_start(now), -1);
}

#[test]
fn an_edit_at_the_timer_rate_starts_one_run_and_lets_it_advance() {
    let mut f = adopted();
    let mut counts = (0, 0, 0);
    for now in (1000..=6000).step_by(200) {
        driver_tick(&mut f, sig(42), now, &mut counts);
    }
    assert_eq!(counts.1, 1, "the follower superseded its own replacement");
    assert!(counts.2 > 20, "the run never got to advance");
    assert!(f.note_completed(counts.0, "study-b", sig(42), 6200));
    assert_eq!(f.state(), FollowState::Current);
    assert_eq!(f.study_id(), "study-b");
}

#[test]
fn a_second_edit_during_the_run_supersedes_it() {
    let mut f = adopted();
    let mut counts = (0, 0, 0);
    for now in (1000..=2000).step_by(200) {
        driver_tick(&mut f, sig(42), now, &mut counts);
    }
    let first = counts.0;
    for now in (2200..=3400).step_by(200) {
        driver_tick(&mut f, sig(43), now, &mut counts);
    }
    assert_eq!(counts.1, 2);
    assert_ne!(counts.0, first);
    assert!(!f.note_completed(first, "study-b", sig(42), 3500));
    assert_eq!(f.discarded(), 1);
}

#[test]
fn wrong_inputs_at_the_right_generation_go_back_to_dirty() {
    let mut f = adopted();
    f.observe(sig(42), 1100);
    let generation = f.note_started(sig(42));
    assert!(f.can_publish_result(generation, sig(42)));
    assert!(!f.can_publish_result(generation, sig(43)));
    assert!(!f.note_completed(generation, "x", sig(43), 1600));
    assert_eq!(f.state(), FollowState::DirtyVisible);
    assert_ne!(f.generation(), generation);
}

#[test]
fn failures_hold_and_superseded_failures_do_not() {
    let mut f = adopted();
    f.observe(sig(42), 1100);
    let generation = f.note_started(sig(42));
    f.note_failed(generation + 1);
    assert_eq!(f.state(), FollowState::UpdatingVisible);
    f.note_failed(generation);
    assert_eq!(f.state(), FollowState::Failed);
    assert!(
        !f.observe(sig(42), 1200),
        "an unchanged poll does not retry"
    );
    assert!(f.observe(sig(43), 1300));
    assert_eq!(f.state(), FollowState::DirtyVisible);
}

#[test]
fn clear_orphans_runs_and_no_study_records_the_world() {
    let mut f = adopted();
    f.observe(sig(42), 1100);
    let generation = f.note_started(sig(42));
    f.clear();
    assert_eq!(f.state(), FollowState::NoStudy);
    assert!(!f.note_completed(generation, "late", sig(42), 1500));
    assert!(!f.observe(sig(44), 1600));
    assert_eq!(f.world_signature(), sig(44));
    assert_eq!(f.describe(), "no sun study is being followed");
}

#[test]
fn the_description_names_both_snapshots_and_the_debounce_is_settable() {
    let mut f = adopted();
    assert_eq!(
        f.describe(),
        "sun study 'study-a' is current for snapshot 41"
    );
    f.observe(sig(42), 1100);
    assert_eq!(
        f.describe(),
        "sun study partly stale (geometry): scene snapshot 42 != study snapshot 41"
    );
    f.set_debounce_ms(50);
    assert_eq!(f.debounce_ms(), 50);
    f.set_debounce_ms(0);
    assert_eq!(f.debounce_ms(), DEBOUNCE_MS);
}
