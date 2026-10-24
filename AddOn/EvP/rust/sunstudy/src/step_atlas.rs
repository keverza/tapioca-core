//! Per-timestep lit bits laid out texel for texel like the hours atlas, for
//! the shadow views.
//!
//! Port of `SunStudy/SunStudyStepAtlas.cpp`. 32 steps per `u32` word, one array
//! slice per word (slice k holds steps 32k..32k+31, bit `step & 31`). The
//! reads (`lit`, `shadowed_between`, `fan_rank`) mirror the overlay shader line
//! for line, which is why they are pinned here.

use crate::series::SunStep;
use std::collections::BTreeSet;

/// Steps covered by the shadow fan: three words of selection bits.
pub const FAN_WORDS: usize = 3;
/// 96 steps: a whole day at 15 minutes.
pub const FAN_MAX_STEPS: u32 = 96;

/// The packed step masks.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct StepMaskAtlas {
    /// Texels across.
    pub width: u32,
    /// Texels down.
    pub height: u32,
    /// Array slices (at least 1).
    pub words: u32,
    /// Steps packed.
    pub steps: u32,
    /// Slice-major: `masks[word * width * height + texel]`.
    pub masks: Vec<u32>,
}

impl StepMaskAtlas {
    fn plane(&self) -> usize {
        usize::try_from(u64::from(self.width) * u64::from(self.height)).unwrap_or(usize::MAX)
    }

    fn word(&self, word: usize, texel: usize) -> u32 {
        word.checked_mul(self.plane())
            .and_then(|base| base.checked_add(texel))
            .and_then(|i| self.masks.get(i))
            .copied()
            .unwrap_or(0)
    }

    /// Whether the texel was lit at `step`.
    #[must_use]
    pub fn lit(&self, texel: usize, step: usize) -> bool {
        let word = step >> 5;
        let in_range = u32::try_from(step).is_ok_and(|s| s < self.steps)
            && texel < self.plane()
            && u32::try_from(word).is_ok_and(|w| w < self.words);
        in_range && (self.word(word, texel) >> (step & 31)) & 1 != 0
    }

    /// Whether the texel was shadowed at any step in `[first, last)`, a word at
    /// a time (the shader's `ShadowedIn`).
    #[must_use]
    pub fn shadowed_between(&self, texel: usize, first: u32, last: u32) -> bool {
        if first >= last || texel >= self.plane() {
            return false;
        }
        let last_word = (last - 1) >> 5;
        let mut word = first >> 5;
        while word <= last_word && word < self.words {
            let bits = self.word(usize::try_from(word).unwrap_or(usize::MAX), texel);
            let base = word.wrapping_mul(32);
            let lo = first.max(base) - base;
            let hi = last.min(base.wrapping_add(32)) - base;
            let below = if hi >= 32 {
                u32::MAX
            } else {
                (1_u32 << hi) - 1
            };
            let range = below & !((1_u32 << lo) - 1);
            if (!bits) & range != 0 {
                return true;
            }
            word += 1;
        }
        false
    }

    /// Among the `selected` steps, the rank of the LAST one at which the texel
    /// was shadowed, or `None` when it was lit at all of them (the shader's
    /// `FanRank`).
    #[must_use]
    pub fn fan_rank(&self, texel: usize, selected: &[u32; FAN_WORDS]) -> Option<u32> {
        if texel >= self.plane() {
            return None;
        }
        for (word, chosen) in selected.iter().enumerate().rev() {
            if *chosen == 0 {
                continue;
            }
            let bits = if u32::try_from(word).is_ok_and(|w| w < self.words) {
                self.word(word, texel)
            } else {
                0
            };
            let shadowed = !bits & chosen;
            if shadowed == 0 {
                continue;
            }
            let bit = shadowed.ilog2();
            let below = if bit == 0 {
                0
            } else {
                chosen & ((1_u32 << bit) - 1)
            };
            let lower: u32 = selected.iter().take(word).map(|w| w.count_ones()).sum();
            return Some(below.count_ones() + lower);
        }
        None
    }
}

/// One step per interval, snapped to the clock; a target is kept only when a
/// step lies within half an interval. A non-positive interval keeps every
/// step; an empty pick falls back to step 0.
#[must_use]
pub fn fan_steps(step_minutes: &[u16], interval_minutes: f64) -> Vec<u32> {
    let (Some(first), Some(last)) = (step_minutes.first(), step_minutes.last()) else {
        return Vec::new();
    };
    let index = |i: usize| u32::try_from(i).unwrap_or(u32::MAX);
    if interval_minutes.partial_cmp(&0.0) != Some(std::cmp::Ordering::Greater) {
        return (0..step_minutes.len()).map(index).collect();
    }
    let (start, end) = (f64::from(*first), f64::from(*last));
    let mut taken = BTreeSet::new();
    let mut picked = Vec::new();
    let mut target = (start / interval_minutes).ceil() * interval_minutes;
    while target <= end + 1e-9 {
        let mut best = 0_usize;
        let mut best_gap = -1.0;
        for (i, minutes) in step_minutes.iter().enumerate() {
            let gap = (f64::from(*minutes) - target).abs();
            if best_gap < 0.0 || gap < best_gap {
                best = i;
                best_gap = gap;
            }
        }
        if best_gap >= 0.0 && !taken.contains(&best) && best_gap <= interval_minutes / 2.0 {
            taken.insert(best);
            picked.push(index(best));
        }
        target += interval_minutes;
    }
    if picked.is_empty() {
        picked.push(0);
    }
    picked
}

/// Pack per-sample lit bits into texels. `step_bits` is the store's wire
/// encoding: sample-major, LSB-first, `step_count` bits per sample.
/// `texel_of_sample[i]` is where the hours atlas put sample i (negative for
/// none). At least one slice even for zero steps.
#[must_use]
pub fn pack_step_masks(
    step_bits: &[u8],
    sample_count: usize,
    step_count: usize,
    texel_of_sample: &[i64],
    size: (u32, u32),
) -> StepMaskAtlas {
    let (width, height) = size;
    let words = u32::try_from(step_count.div_ceil(32))
        .unwrap_or(u32::MAX)
        .max(1);
    let plane = usize::try_from(u64::from(width) * u64::from(height)).unwrap_or(usize::MAX);
    let mut out = StepMaskAtlas {
        width,
        height,
        words,
        steps: u32::try_from(step_count).unwrap_or(u32::MAX),
        masks: vec![0; plane.saturating_mul(usize::try_from(words).unwrap_or(1))],
    };
    let lit = |sample: usize, step: usize| {
        let bit = sample * step_count + step;
        step_bits
            .get(bit >> 3)
            .is_some_and(|byte| (byte >> (bit & 7)) & 1 != 0)
    };
    for (sample, texel) in texel_of_sample.iter().enumerate().take(sample_count) {
        let Some(texel) = usize::try_from(*texel).ok().filter(|t| *t < plane) else {
            continue;
        };
        for step in (0..step_count).filter(|s| lit(sample, *s)) {
            if let Some(slot) = out.masks.get_mut((step >> 5) * plane + texel) {
                *slot |= 1 << (step & 31);
            }
        }
    }
    out
}

/// The step at which the sun stands highest (solar noon as the project
/// reports it, not clock noon); the first on ties, 0 for an empty series.
#[must_use]
pub fn solar_noon_step(steps: &[SunStep]) -> u32 {
    let mut best = 0_usize;
    for (step, s) in steps.iter().enumerate().skip(1) {
        let best_altitude = steps.get(best).map_or(f64::NAN, |b| b.altitude_degrees);
        if s.altitude_degrees > best_altitude {
            best = step;
        }
    }
    u32::try_from(best).unwrap_or(u32::MAX)
}

/// Each step's time of day in minutes, wrapped to 16 bits as the C++ cast.
#[must_use]
pub fn step_minutes(steps: &[SunStep]) -> Vec<u16> {
    steps
        .iter()
        .map(|s| wrap16(s.hour.wrapping_mul(60).wrapping_add(s.minute)))
        .collect()
}

#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "deliberate 16-bit wrap, matching static_cast<uint16_t>(int)"
)]
fn wrap16(value: i32) -> u16 {
    value as u16
}

#[cfg(test)]
mod tests;
