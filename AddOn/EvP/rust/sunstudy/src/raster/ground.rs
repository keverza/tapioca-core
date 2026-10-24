//! The analysis ground plane around the massing and the coarse-to-fine grid
//! ladder.
//!
//! The study is meaningless without the ground pad: sampling only the model's
//! own footprint puts every sample under a building and reports zero hours.

use super::{Vec3, cpp_max, inverted};
use crate::series::count_to_f64;
use std::cmp::Ordering;

/// How far the ground plane must reach beyond the model: the model's height,
/// but never less than a quarter of the footprint or `min_pad` metres.
#[must_use]
pub fn auto_ground_pad(aabb_min: &Vec3, aabb_max: &Vec3, min_pad: f64) -> f64 {
    let height = aabb_max[2] - aabb_min[2];
    let footprint = cpp_max(aabb_max[0] - aabb_min[0], aabb_max[1] - aabb_min[1]);
    cpp_max(cpp_max(height, 0.25 * footprint), min_pad)
}

/// A regular, centred grid over the padded footprint, described rather than
/// materialised so a caller can size its buffer first.
///
/// Regular where the Python oracle is random, deliberately: deterministic
/// without a seed and even coverage. Totals agree in distribution, not sample
/// for sample.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct GroundGrid {
    /// Pad actually applied, metres.
    pub pad: f64,
    /// Height of every sample, metres (`aabb_min.z + z_offset`).
    pub ground_z: f64,
    /// Samples along x.
    pub columns: usize,
    /// Samples along y.
    pub rows: usize,
    /// Spacing between samples, metres.
    pub spacing: f64,
    x_start: f64,
    y_start: f64,
}

impl GroundGrid {
    /// `columns * rows`; cannot overflow because construction refused that.
    #[must_use]
    pub fn sample_count(&self) -> usize {
        self.columns * self.rows
    }

    /// Row-major xyz positions, `3 * sample_count()` scalars.
    #[must_use]
    pub fn positions(&self) -> Vec<f64> {
        let mut out = vec![0.0; self.sample_count() * 3];
        self.write_positions(&mut out);
        out
    }

    /// Fill `out` with row-major xyz positions; stops at the shorter of `out`
    /// and the grid.
    pub fn write_positions(&self, out: &mut [f64]) {
        let cells =
            (0..self.rows).flat_map(|row| (0..self.columns).map(move |column| (row, column)));
        for (xyz, (row, column)) in out.as_chunks_mut::<3>().0.iter_mut().zip(cells) {
            *xyz = [
                self.x_start + count_to_f64(column) * self.spacing,
                self.y_start + count_to_f64(row) * self.spacing,
                self.ground_z,
            ];
        }
    }

    /// All +Z normals, so the back-face cull still retires a sun below the
    /// horizon.
    #[must_use]
    pub fn normals(&self) -> Vec<f64> {
        [0.0, 0.0, 1.0].repeat(self.sample_count())
    }
}

/// Parameters of [`make_ground_sample_grid`].
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct GroundGridRequest {
    /// Model box minimum corner, metres.
    pub aabb_min: Vec3,
    /// Model box maximum corner, metres.
    pub aabb_max: Vec3,
    /// Sample spacing, metres; must be positive.
    pub spacing: f64,
    /// Pad beyond the box, metres; negative means [`auto_ground_pad`] with its
    /// 2 m floor (the C++ default is -1).
    pub pad: f64,
    /// Lift off the ground so a ray does not start on its own surface, metres
    /// (C++ default 0.10).
    pub z_offset: f64,
    /// Refusal ceiling on `columns * rows` (C++ default 4,000,000).
    pub max_samples: usize,
}

/// Describe the grid over the padded footprint. `None` for non-positive
/// spacing, an inverted box, a non-finite extent (undefined behaviour in the
/// C++ original), or more than `max_samples` samples: refused rather than
/// truncated.
#[must_use]
pub fn make_ground_sample_grid(request: &GroundGridRequest) -> Option<GroundGrid> {
    let GroundGridRequest {
        aabb_min,
        aabb_max,
        spacing,
        pad,
        z_offset,
        max_samples,
    } = *request;
    let (aabb_min, aabb_max) = (&aabb_min, &aabb_max);
    if spacing.partial_cmp(&0.0) != Some(Ordering::Greater) || inverted(aabb_min, aabb_max) {
        return None;
    }
    let pad = if pad < 0.0 {
        auto_ground_pad(aabb_min, aabb_max, 2.0)
    } else {
        pad
    };
    let (x_lo, x_hi) = (aabb_min[0] - pad, aabb_max[0] + pad);
    let (y_lo, y_hi) = (aabb_min[1] - pad, aabb_max[1] + pad);
    let columns = axis_count((x_hi - x_lo) / spacing, max_samples)?;
    let rows = axis_count((y_hi - y_lo) / spacing, max_samples)?;
    if rows > max_samples / columns {
        return None;
    }
    // Centred: the leftover after whole steps is split between both edges.
    let x_start = x_lo + ((x_hi - x_lo) - count_to_f64(columns - 1) * spacing) * 0.5;
    let y_start = y_lo + ((y_hi - y_lo) - count_to_f64(rows - 1) * spacing) * 0.5;
    Some(GroundGrid {
        pad,
        ground_z: aabb_min[2] + z_offset,
        columns,
        rows,
        spacing,
        x_start,
        y_start,
    })
}

/// `floor(steps) + 1`, refused when non-finite, negative or above `max`.
fn axis_count(steps: f64, max: usize) -> Option<usize> {
    let floored = steps.floor();
    if floored.partial_cmp(&0.0).is_none_or(Ordering::is_lt) || floored >= count_to_f64(max) {
        return None;
    }
    Some(floor_to_usize(floored) + 1).filter(|count| *count <= max)
}

#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "caller checked a finite, non-negative, floored value below max"
)]
fn floor_to_usize(value: f64) -> usize {
    value as usize
}

/// Coarse-to-fine analysis grids, coarsest first, always ending at
/// `target_grid`. Refine by sample density, never by timestep: a coarse rung is
/// a complete, correct study, a partial set of timesteps is a wrong number of
/// hours.
#[must_use]
pub fn grid_ladder(
    area_m2: f64,
    target_grid: f64,
    coarse_threshold: f64,
    mid_threshold: f64,
) -> Vec<f64> {
    if target_grid <= 0.0 {
        return vec![target_grid];
    }
    let estimated_samples = area_m2 / (target_grid * target_grid);
    let mut ladder = Vec::with_capacity(3);
    if estimated_samples > coarse_threshold {
        ladder.push(target_grid * 4.0);
    }
    if estimated_samples > mid_threshold {
        ladder.push(target_grid * 2.0);
    }
    ladder.push(target_grid);
    ladder
}
