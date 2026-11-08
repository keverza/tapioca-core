//! The terrain design previewed from its field on a plan grid (#175): its surface, its contour
//! lines and its cut and fill, in milliseconds, with no mesh.
//!
//! The editor's preview needs no triangulated terrain: the mesh (toe, creases, the refining
//! mesher) is what an Archicad Mesh element is built from, and takes seconds. This samples the
//! same field the mesh is refined against once, on a regular grid, and derives the drawable
//! surface, the contours and the volumes from that one sampling.

use super::contour::{Contours, Grid};

/// At most this many surface vertices: a finer grid is drawn every few nodes (the contours and
/// volumes still use every node).
const MAX_SURFACE: usize = 262_144;

/// A field's preview on one grid.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Preview {
    /// The design's contours.
    pub contours: Contours,
    /// The drawable surface: xyz on the design.
    pub points: Vec<f64>,
    /// The surface's triangles, counter-clockwise in plan, over the cells with ground at all
    /// four corners.
    pub triangles: Vec<u32>,
    /// The surface's node spacing, metres (the grid's, or a multiple of it).
    pub surface_step: f64,
    /// Volume where the design is below the base, cubic metres (positive).
    pub cut: f64,
    /// Volume where the design is above the base, cubic metres.
    pub fill: f64,
}

/// The box `lo`-`hi` sampled every `step` metres with `height` (the design and the base at a
/// plan point, `None` where there is no ground), contoured every `interval` metres (`<= 0`:
/// chosen, see [`super::contour::trace`]).
pub fn preview(
    lo: [f64; 2],
    hi: [f64; 2],
    step: f64,
    interval: f64,
    height: &(dyn Fn(f64, f64) -> Option<[f64; 2]> + Sync),
) -> Preview {
    let Some(grid) = Grid::sample(lo, hi, step, height) else {
        return Preview::default();
    };
    let contours = grid.trace(interval);
    let (cut, fill) = volumes(&grid);
    let (points, triangles, surface_step) = surface(&grid);
    Preview {
        contours,
        points,
        triangles,
        surface_step,
        cut,
        fill,
    }
}

/// Cut and fill over every cell with ground at its corners: the cell's area times the mean of
/// its corners' design-minus-base, split by sign at the cell (a preview's resolution, not the
/// mesh's exact volumes).
fn volumes(grid: &Grid) -> (f64, f64) {
    let area = grid.step * grid.step;
    let (mut cut, mut fill) = (0.0, 0.0);
    let difference = |column: usize, row: usize| {
        let at = row * grid.columns + column;
        match (grid.values.get(at), grid.base.get(at)) {
            (Some(design), Some(base)) => design - base,
            _ => f64::NAN,
        }
    };
    for row in 0..grid.rows.saturating_sub(1) {
        for column in 0..grid.columns.saturating_sub(1) {
            let mean = (difference(column, row)
                + difference(column + 1, row)
                + difference(column + 1, row + 1)
                + difference(column, row + 1))
                / 4.0;
            if mean > 0.0 {
                fill += mean * area;
            } else if mean < 0.0 {
                cut -= mean * area;
            }
        }
    }
    (cut, fill)
}

/// The nodes `0, stride, 2 * stride, ...` below `count`, and the last.
fn picks(count: usize, stride: usize) -> Vec<usize> {
    let mut picked: Vec<usize> = (0..count).step_by(stride.max(1)).collect();
    if count > 0 && picked.last() != Some(&(count - 1)) {
        picked.push(count - 1);
    }
    picked
}

/// The grid every `stride` nodes as triangles (two per cell with ground at all four corners),
/// the stride the smallest keeping at most [`MAX_SURFACE`] vertices; the last row and column
/// are kept.
fn surface(grid: &Grid) -> (Vec<f64>, Vec<u32>, f64) {
    let nodes = grid.columns * grid.rows;
    let mut stride = 1_usize;
    while nodes.div_ceil(stride * stride) > MAX_SURFACE {
        stride += 1;
    }
    let (columns, rows) = (picks(grid.columns, stride), picks(grid.rows, stride));
    let mut index = vec![u32::MAX; columns.len() * rows.len()];
    let mut points = Vec::new();
    for (r, &row) in rows.iter().enumerate() {
        for (c, &column) in columns.iter().enumerate() {
            let z = grid.value(column, row);
            if z.is_nan() {
                continue;
            }
            let [x, y] = grid.node(column, row);
            if let (Some(slot), Ok(next)) = (
                index.get_mut(r * columns.len() + c),
                u32::try_from(points.len() / 3),
            ) {
                *slot = next;
                points.extend([x, y, z]);
            }
        }
    }
    let mut triangles = Vec::new();
    let at = |c: usize, r: usize| {
        index
            .get(r * columns.len() + c)
            .copied()
            .unwrap_or(u32::MAX)
    };
    for r in 0..rows.len().saturating_sub(1) {
        for c in 0..columns.len().saturating_sub(1) {
            let corners = [at(c, r), at(c + 1, r), at(c + 1, r + 1), at(c, r + 1)];
            if corners.contains(&u32::MAX) {
                continue;
            }
            let [a, b, d, e] = corners;
            triangles.extend([a, b, d, a, d, e]);
        }
    }
    let spacing = f64::from(u32::try_from(stride).unwrap_or(u32::MAX)) * grid.step;
    (points, triangles, spacing)
}

#[cfg(test)]
mod tests;
