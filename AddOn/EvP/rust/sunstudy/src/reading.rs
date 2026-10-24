//! The study's value at one point on one face: what the hover inspector
//! reports under the cursor.
//!
//! Port of `SunStudy/SunStudyReading.cpp`. It maps the point through the same
//! lattice the tint shader uses, with the shader's float-rounded origin, axes
//! and reciprocal spacing, so the reading agrees with the colour on screen at
//! every cell boundary. Nearest sample of the same surface is only the
//! fallback for a cell no sample covers.

use crate::patch_sampler::{NO_PATCH, PatchSampleGrid};
use crate::raster::Vec3;
use crate::sampler::SampleGrid;

/// One reading; `measured` false means the face carries no measurement.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SunStudyReading {
    /// False for a context, ignored, degenerate or unclaimed face.
    pub measured: bool,
    /// Sun hours of the chosen sample.
    pub hours: f64,
    /// The chosen sample.
    pub sample: usize,
    /// Cell column under the point.
    pub column: u32,
    /// Cell row under the point.
    pub row: u32,
    /// The cell held no sample; the nearest one of the surface was used.
    pub nearest: bool,
}

/// The arrays a reading chooses among.
struct Candidates<'a> {
    columns: &'a [u32],
    rows: &'a [u32],
    positions: &'a [f64],
    hours: &'a [f64],
}

fn gpu(value: f64) -> f64 {
    f64::from(crate::atlas::f64_to_f32(value))
}

/// Mirrors `static_cast<uint32_t>(std::clamp(cell, 0.0, last))`. The C++
/// cast of a NaN cell is undefined; here it saturates to 0 (issue #21).
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "the value is clamped to [0, count - 1] first"
)]
fn clamp_cell(cell: f64, last: f64) -> u32 {
    let clamped = if cell < 0.0 {
        0.0
    } else if last < cell {
        last
    } else {
        cell
    };
    clamped as u32
}

#[expect(
    clippy::cast_precision_loss,
    reason = "mirrors static_cast<double>(long) for a start cell"
)]
fn start_cell(value: i64) -> f64 {
    value as f64
}

/// One axis of a lattice as the shader sees it.
struct LatticeAxis<'a> {
    origin: &'a Vec3,
    direction: &'a Vec3,
    start: i64,
    count: u32,
}

/// The shader's cell arithmetic: floor of the lattice coordinate, minus the
/// start cell, clamped to the tile. Keep in step with `kArchVizSunTintPS`.
fn cell(point: &Vec3, lattice: &LatticeAxis<'_>, spacing: f64) -> u32 {
    let (origin, axis) = (lattice.origin, lattice.direction);
    let relative = [
        point[0] - gpu(origin[0]),
        point[1] - gpu(origin[1]),
        point[2] - gpu(origin[2]),
    ];
    let axis = [gpu(axis[0]), gpu(axis[1]), gpu(axis[2])];
    let inverse = gpu(1.0 / spacing);
    let dot = relative[0] * axis[0] + relative[1] * axis[1] + relative[2] * axis[2];
    let value = (dot * inverse).floor() - start_cell(lattice.start);
    let last = if lattice.count > 0 {
        f64::from(lattice.count - 1)
    } else {
        0.0
    };
    clamp_cell(value, last)
}

fn distance_squared(positions: &[f64], sample: usize, point: &Vec3) -> Option<f64> {
    let at = positions.get(sample * 3..sample * 3 + 3)?;
    let mut sum = 0.0;
    for (p, q) in at.iter().zip(point) {
        let d = p - q;
        sum += d * d;
    }
    Some(sum)
}

/// Among `samples`, the one in (column, row); failing that the nearest.
fn choose(
    samples: impl Iterator<Item = usize>,
    arrays: &Candidates<'_>,
    cell: (u32, u32),
    point: &Vec3,
) -> SunStudyReading {
    let (column, row) = cell;
    let mut out = SunStudyReading {
        column,
        row,
        ..SunStudyReading::default()
    };
    let mut found = false;
    let mut best = f64::MAX;
    for sample in samples {
        let (Some(_), Some(c), Some(r)) = (
            arrays.hours.get(sample),
            arrays.columns.get(sample),
            arrays.rows.get(sample),
        ) else {
            continue;
        };
        if *c == column && *r == row {
            if !found || out.nearest {
                out.sample = sample;
                out.nearest = false;
                found = true;
            }
            continue;
        }
        if found && !out.nearest {
            continue;
        }
        // The C++ reads positions unchecked; a short array skips the sample.
        let Some(d) = distance_squared(arrays.positions, sample, point) else {
            continue;
        };
        if d < best {
            best = d;
            out.sample = sample;
            out.nearest = true;
            found = true;
        }
    }
    match arrays.hours.get(out.sample) {
        Some(hours) if found => SunStudyReading {
            measured: true,
            hours: *hours,
            ..out
        },
        _ => SunStudyReading::default(),
    }
}

/// Triangle domain; `grid` must carry layouts.
#[must_use]
pub fn read_triangle_study_at(
    grid: &SampleGrid,
    spacing: f64,
    hours: &[f64],
    face: usize,
    point: &Vec3,
) -> SunStudyReading {
    let Some(layout) = grid.layouts.get(face).filter(|_| spacing > 0.0) else {
        return SunStudyReading::default();
    };
    // A face below the grid carries one centroid sample in cell (0,0).
    let at = if layout.gridded {
        (
            cell(
                point,
                &LatticeAxis {
                    origin: &layout.origin,
                    direction: &layout.u_axis,
                    start: layout.u_start,
                    count: layout.columns,
                },
                spacing,
            ),
            cell(
                point,
                &LatticeAxis {
                    origin: &layout.origin,
                    direction: &layout.v_axis,
                    start: layout.v_start,
                    count: layout.rows,
                },
                spacing,
            ),
        )
    } else {
        (0, 0)
    };
    // Not grouped by face, so the face's samples are found by scanning.
    let samples = grid
        .faces
        .iter()
        .enumerate()
        .filter(|(_, f)| usize::try_from(**f).ok() == Some(face))
        .map(|(i, _)| i);
    let arrays = Candidates {
        columns: &grid.cell_columns,
        rows: &grid.cell_rows,
        positions: &grid.positions,
        hours,
    };
    choose(samples, &arrays, at, point)
}

/// Patch domain.
#[must_use]
pub fn read_patch_study_at(
    grid: &PatchSampleGrid,
    spacing: f64,
    hours: &[f64],
    face: usize,
    point: &Vec3,
) -> SunStudyReading {
    let span = grid
        .patch_of_triangle
        .get(face)
        .filter(|s| spacing > 0.0 && **s != NO_PATCH)
        .and_then(|s| grid.spans.get(usize::try_from(*s).ok()?));
    let Some(span) = span else {
        return SunStudyReading::default();
    };
    let at = (
        cell(
            point,
            &LatticeAxis {
                origin: &span.origin,
                direction: &span.u_axis,
                start: 0,
                count: span.columns,
            },
            spacing,
        ),
        cell(
            point,
            &LatticeAxis {
                origin: &span.origin,
                direction: &span.v_axis,
                start: 0,
                count: span.rows,
            },
            spacing,
        ),
    );
    // A patch's samples are one contiguous span.
    let arrays = Candidates {
        columns: &grid.cell_columns,
        rows: &grid.cell_rows,
        positions: &grid.positions,
        hours,
    };
    choose(span.first..span.first + span.count, &arrays, at, point)
}

#[cfg(test)]
mod tests;
