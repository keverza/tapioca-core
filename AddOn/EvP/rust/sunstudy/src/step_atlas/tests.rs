//! Mirrors `tests/cpp/test_sunstudystepatlas.cpp`.

use super::*;
use crate::series::{SunSeries, SunStep};

fn bits(samples: usize, steps: usize, lit: impl Fn(usize, usize) -> bool) -> Vec<u8> {
    let mut out = vec![0_u8; (samples * steps).div_ceil(8)];
    let set = (0..samples * steps).filter(|bit| lit(bit / steps.max(1), bit % steps.max(1)));
    for bit in set {
        if let Some(byte) = out.get_mut(bit >> 3) {
            *byte |= 1 << (bit & 7);
        }
    }
    out
}

fn pattern(sample: usize, step: usize) -> bool {
    (sample * 7 + step * 3) % 5 < 2
}

#[test]
fn every_sample_day_lands_on_its_own_texel() {
    let (samples, steps) = (6, 70);
    let texels: Vec<i64> = vec![3, 0, 5, -1, 1, 4];
    let atlas = pack_step_masks(
        &bits(samples, steps, pattern),
        samples,
        steps,
        &texels,
        (3, 2),
    );
    assert_eq!(atlas.words, 3);
    for (sample, texel) in texels.iter().enumerate() {
        let Ok(texel) = usize::try_from(*texel) else {
            continue;
        };
        for step in 0..steps {
            assert_eq!(atlas.lit(texel, step), pattern(sample, step));
        }
    }
    assert!(
        !(0..steps).any(|s| atlas.lit(2, s)),
        "an unused texel stays dark"
    );
}

#[test]
fn shadowed_between_agrees_with_a_step_by_step_answer_over_every_range() {
    let (samples, steps) = (4, 70);
    let texels: Vec<i64> = vec![0, 1, 2, 3];
    let atlas = pack_step_masks(
        &bits(samples, steps, pattern),
        samples,
        steps,
        &texels,
        (2, 2),
    );
    let step_count = u32::try_from(steps).unwrap_or(0);
    for texel in 0..4 {
        for first in 0..=step_count {
            // C++ does not bound ranges by the packed count (unset bits read as
            // shadowed); the shader clamps, so ranges stay within the steps.
            for last in first..=step_count {
                let brute = (first..last.min(step_count))
                    .any(|s| !atlas.lit(texel, usize::try_from(s).unwrap_or(0)));
                assert_eq!(
                    atlas.shadowed_between(texel, first, last),
                    brute,
                    "{texel} [{first},{last})"
                );
            }
        }
    }
}

#[test]
fn a_study_of_no_steps_still_has_one_slice() {
    let atlas = pack_step_masks(&[], 0, 0, &[], (4, 4));
    assert_eq!((atlas.words, atlas.masks.len()), (1, 16));
}

#[test]
fn solar_noon_is_the_highest_sun_not_twelve_oclock() {
    let step = |hour, altitude| SunStep {
        hour,
        minute: 0,
        direction: [0.0, 0.0, 1.0],
        altitude_degrees: altitude,
    };
    let series = SunSeries::from_steps(
        &[
            step(11, 40.0),
            step(12, 52.0),
            step(13, 55.0),
            step(14, 50.0),
        ],
        60,
        0.0,
    );
    assert_eq!(solar_noon_step(series.steps()), 2);
    assert_eq!(step_minutes(series.steps()), [660, 720, 780, 840]);
    assert_eq!(solar_noon_step(&[]), 0);
}

#[test]
fn fan_steps_snap_to_the_clock() {
    let minutes: Vec<u16> = (0..40).map(|i| 480 + 15 * i).collect();
    let picked = fan_steps(&minutes, 60.0);
    assert!(picked.iter().all(|i| {
        minutes
            .get(usize::try_from(*i).unwrap_or(99))
            .is_some_and(|m| m % 60 == 0)
    }));
    assert_eq!(fan_steps(&minutes, 0.0).len(), 40);
    assert_eq!(fan_steps(&[], 60.0), Vec::<u32>::new());
    assert_eq!(fan_steps(&[485], 60.0), [0]);
}

#[test]
fn fan_rank_is_the_last_shadowed_selected_step_against_a_brute_force_answer() {
    let (samples, steps) = (5, 96);
    let texels: Vec<i64> = (0..5).collect();
    let atlas = pack_step_masks(
        &bits(samples, steps, pattern),
        samples,
        steps,
        &texels,
        (5, 1),
    );
    for selection in [
        [0b1011_u32, 0, 0],
        [0, 0xF0F0, 1],
        [u32::MAX, 0, 0x8000_0001],
        [0, 0, 0],
    ] {
        for texel in 0..5 {
            let chosen: Vec<usize> = (0..96)
                .filter(|s| {
                    selection
                        .get(s / 32)
                        .is_some_and(|w| (w >> (s % 32)) & 1 != 0)
                })
                .collect();
            let brute = chosen
                .iter()
                .rposition(|s| !atlas.lit(texel, *s))
                .map(|r| u32::try_from(r).unwrap_or(0));
            assert_eq!(atlas.fan_rank(texel, &selection), brute);
        }
    }
}
