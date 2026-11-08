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
    /// The drawable surface: xyz on the design, where it moves the base and a cell around.
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
    /// Grid nodes where an outline's faces ran out before they met the ground, at `reach`
    /// (set by the unified preview; zero elsewhere).
    pub unreached: u32,
    /// Property-ring samples where an outline's faces end in a step (as `unreached`).
    pub clipped: u32,
    /// The outline faces' reach used, metres (as `unreached`).
    pub reach: f64,
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
        unreached: 0,
        clipped: 0,
        reach: 0.0,
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

/// A design within this of the base does not move it, metres.
const MOVED: f64 = 1e-3;

/// The grid every `stride` nodes as triangles (two per cell with ground at all four corners), the
/// stride the smallest keeping at most [`MAX_SURFACE`] vertices, the last row and column kept --
/// only the cells where the design moves the base at a corner, and the cells around them. The
/// rest is the base itself, which the caller already shows; drawing it again would only cost.
fn surface(grid: &Grid) -> (Vec<f64>, Vec<u32>, f64) {
    let nodes = grid.columns * grid.rows;
    let mut stride = 1_usize;
    while nodes.div_ceil(stride * stride) > MAX_SURFACE {
        stride += 1;
    }
    let (columns, rows) = (picks(grid.columns, stride), picks(grid.rows, stride));
    let moved = |column: usize, row: usize| {
        let at = row * grid.columns + column;
        match (grid.values.get(at), grid.base.get(at)) {
            (Some(design), Some(base)) => (design - base).abs() > MOVED,
            _ => false,
        }
    };
    // Cells (between picked nodes) the design moves, then grown by one cell each way.
    let (cells_x, cells_y) = (
        columns.len().saturating_sub(1),
        rows.len().saturating_sub(1),
    );
    let mut keep = vec![false; cells_x * cells_y];
    for r in 0..cells_y {
        for c in 0..cells_x {
            let (Some(&x0), Some(&x1), Some(&y0), Some(&y1)) = (
                columns.get(c),
                columns.get(c + 1),
                rows.get(r),
                rows.get(r + 1),
            ) else {
                continue;
            };
            if [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
                .iter()
                .any(|&(column, row)| moved(column, row))
            {
                for dr in r.saturating_sub(1)..=(r + 1).min(cells_y.saturating_sub(1)) {
                    for dc in c.saturating_sub(1)..=(c + 1).min(cells_x.saturating_sub(1)) {
                        if let Some(flag) = keep.get_mut(dr * cells_x + dc) {
                            *flag = true;
                        }
                    }
                }
            }
        }
    }
    let mut index = vec![u32::MAX; columns.len() * rows.len()];
    let mut points = Vec::new();
    let mut vertex = |c: usize, r: usize, points: &mut Vec<f64>| -> u32 {
        let slot = r * columns.len() + c;
        if let Some(&known) = index.get(slot)
            && known != u32::MAX
        {
            return known;
        }
        let (Some(&column), Some(&row)) = (columns.get(c), rows.get(r)) else {
            return u32::MAX;
        };
        let z = grid.value(column, row);
        if z.is_nan() {
            return u32::MAX;
        }
        let [x, y] = grid.node(column, row);
        let Ok(next) = u32::try_from(points.len() / 3) else {
            return u32::MAX;
        };
        points.extend([x, y, z]);
        if let Some(entry) = index.get_mut(slot) {
            *entry = next;
        }
        next
    };
    let mut triangles = Vec::new();
    for r in 0..cells_y {
        for c in 0..cells_x {
            if !keep.get(r * cells_x + c).copied().unwrap_or(false) {
                continue;
            }
            let corners = [
                vertex(c, r, &mut points),
                vertex(c + 1, r, &mut points),
                vertex(c + 1, r + 1, &mut points),
                vertex(c, r + 1, &mut points),
            ];
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
