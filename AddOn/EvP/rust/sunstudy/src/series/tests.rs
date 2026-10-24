//! Mirrors `tests/cpp/test_sunseries.cpp` case for case.

use super::*;

fn step(hour: i32, minute: i32, altitude: f64, direction: [f64; 3]) -> SunStep {
    SunStep {
        hour,
        minute,
        direction,
        altitude_degrees: altitude,
    }
}

fn up(hour: i32, altitude: f64) -> SunStep {
    step(hour, 0, altitude, [0.0, 0.0, 1.0])
}

fn hours(series: &SunSeries) -> Vec<i32> {
    series.steps().iter().map(|s| s.hour).collect()
}

#[test]
fn whole_day_is_the_default_window() {
    assert_eq!(enumerate_timesteps(60, 0, 24).len(), 24);
    assert_eq!(enumerate_timesteps(30, 0, 24).len(), 48);
    assert_eq!(enumerate_timesteps(15, 0, 24).len(), 96);
    let hourly = enumerate_timesteps(60, 0, 24);
    assert_eq!(hourly.first(), Some(&TimeOfDay { hour: 0, minute: 0 }));
    assert_eq!(hourly.last().map(|t| t.hour), Some(23));
}

#[test]
fn the_window_upper_bound_is_exclusive() {
    let window = enumerate_timesteps(60, 8, 20);
    assert_eq!(window.len(), 12);
    assert_eq!(window.first().map(|t| t.hour), Some(8));
    assert_eq!(window.last().map(|t| t.hour), Some(19));
}

#[test]
fn minutes_land_on_the_step() {
    let minutes: Vec<i32> = enumerate_timesteps(15, 9, 10)
        .iter()
        .map(|t| t.minute)
        .collect();
    assert_eq!(minutes, [0, 15, 30, 45]);
}

#[test]
fn degenerate_windows_and_steps_are_clamped_not_crashed() {
    assert_eq!(enumerate_timesteps(60, 20, 8), Vec::new());
    assert_eq!(enumerate_timesteps(60, -5, 30).len(), 24);
    assert_eq!(enumerate_timesteps(0, 0, 24).len(), 1440);
    assert_eq!(enumerate_timesteps(-10, 0, 24).len(), 1440);
    // Undefined behaviour in the C++ original; well defined and clamped here.
    assert_eq!(enumerate_timesteps(i32::MAX, i32::MIN, i32::MAX).len(), 1);
}

#[test]
fn steps_at_or_below_the_floor_are_dropped() {
    let steps = [
        up(5, -10.0),
        up(6, 0.0),
        up(7, 0.5),
        up(12, 45.0),
        up(20, -1.0),
    ];
    let series = SunSeries::from_steps(&steps, 60, 0.0);
    assert_eq!(hours(&series), [7, 12]);
    assert_eq!(series.source_step_count(), 5);
}

#[test]
fn a_raised_floor_discards_the_ends_of_the_day() {
    let steps = [up(6, 1.0), up(7, 2.5), up(12, 45.0), up(18, 2.0)];
    assert_eq!(SunSeries::from_steps(&steps, 60, 0.0).step_count(), 4);
    assert_eq!(SunSeries::from_steps(&steps, 60, 3.0).step_count(), 1);
}

#[test]
fn a_zero_direction_is_dropped_rather_than_normalised_to_nan() {
    let steps = [step(12, 0, 45.0, [0.0; 3]), up(13, 40.0)];
    let series = SunSeries::from_steps(&steps, 60, 0.0);
    assert_eq!(hours(&series), [13]);
    assert!(series.steps().iter().all(|s| !s.direction[2].is_nan()));
}

#[test]
fn directions_come_back_normalised() {
    let series = SunSeries::from_steps(&[step(12, 0, 45.0, [3.0, 0.0, 4.0])], 60, 0.0);
    let d = series.steps().first().map(|s| s.direction);
    assert!(d.is_some_and(|[x, y, z]| {
        ((x * x + y * y + z * z).sqrt() - 1.0).abs() < 1e-12
            && (x - 0.6).abs() < 1e-12
            && (z - 0.8).abs() < 1e-12
    }));
}

#[test]
fn daylight_hours_is_the_denominator_of_every_percentage() {
    let steps: Vec<SunStep> = (0..46)
        .map(|i| step(i / 4, (i % 4) * 15, 10.0, [0.0, 0.0, 1.0]))
        .collect();
    let series = SunSeries::from_steps(&steps, 15, 0.0);
    assert_eq!(series.step_count(), 46);
    assert!((series.hours_per_step() - 0.25).abs() < 1e-12);
    assert!((series.daylight_hours() - 11.5).abs() < 1e-12);
}

#[test]
fn version_changes_with_everything_that_changes_the_answer() {
    let base = [step(9, 0, 20.0, [1.0, 0.0, 1.0]), up(12, 45.0)];
    let reference = SunSeries::from_steps(&base, 60, 0.0).version();
    assert_ne!(reference, 0);
    assert_eq!(SunSeries::from_steps(&base, 60, 0.0).version(), reference);
    assert_ne!(SunSeries::from_steps(&base, 30, 0.0).version(), reference);

    let mut moved = base;
    if let Some(s) = moved.get_mut(1) {
        s.direction[0] = 0.3;
    }
    assert_ne!(SunSeries::from_steps(&moved, 60, 0.0).version(), reference);

    let mut shifted = base;
    if let Some(s) = shifted.first_mut() {
        s.minute = 30;
    }
    assert_ne!(
        SunSeries::from_steps(&shifted, 60, 0.0).version(),
        reference
    );
    assert_ne!(SunSeries::from_steps(&base, 60, 30.0).version(), reference);
}

#[test]
fn empty_is_version_zero_so_it_is_not_mistaken_for_unchanged() {
    let series = SunSeries::from_steps(&[up(2, -30.0)], 60, 0.0);
    assert!(series.is_empty());
    assert_eq!(series.version(), 0);
    assert!(series.daylight_hours().abs() < 1e-12);
    assert_eq!(series.source_step_count(), 1);
}

#[test]
fn negative_zero_does_not_change_the_version() {
    let positive = SunSeries::from_steps(&[step(12, 0, 45.0, [0.0, 0.0, 1.0])], 60, 0.0);
    let negative = SunSeries::from_steps(&[step(12, 0, 45.0, [-0.0, -0.0, 1.0])], 60, 0.0);
    assert_eq!(positive.version(), negative.version());
}

#[test]
fn altitude_is_not_part_of_the_version() {
    // Matches the C++ hash: only time, direction, step and count are hashed.
    let low = SunSeries::from_steps(&[up(12, 10.0)], 60, 0.0);
    let high = SunSeries::from_steps(&[up(12, 70.0)], 60, 0.0);
    assert_eq!(low.version(), high.version());
}
