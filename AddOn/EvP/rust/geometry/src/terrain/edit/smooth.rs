//! Gaussian relaxation of edited heights over the vertices near a moved one.
//!
//! Kernel `exp(-d^2 / 2 sigma^2)` with `sigma = radius / 3`, truncated at
//! `radius` and renormalized over the vertices present, so a constant field stays
//! constant and edges need no padding. Neighbours come from a plan grid of
//! `radius` cells, sorted by cell, so every scan has a fixed order.

use crate::terrain::plan::clamp_index;

/// Vertices sorted by plan cell, and the cell of each.
struct Grid {
    origin: [f64; 2],
    radius: f64,
    sorted: Vec<((u32, u32), usize)>,
}

impl Grid {
    fn new(vertices: &[[f64; 3]], radius: f64) -> Self {
        let origin = vertices
            .iter()
            .fold([f64::INFINITY; 2], |[x0, y0], &[x, y, _]| {
                [x0.min(x), y0.min(y)]
            });
        let mut grid = Self {
            origin,
            radius,
            sorted: Vec::with_capacity(vertices.len()),
        };
        for (index, &[x, y, _]) in vertices.iter().enumerate() {
            let cell = grid.cell(x, y);
            grid.sorted.push((cell, index));
        }
        grid.sorted.sort_unstable();
        grid
    }

    fn cell(&self, x: f64, y: f64) -> (u32, u32) {
        let limit = u32::MAX - 1;
        (
            clamp_index((x - self.origin[0]) / self.radius, limit),
            clamp_index((y - self.origin[1]) / self.radius, limit),
        )
    }

    /// Vertices within `radius` of `(x, y)` with their squared distance, in
    /// ascending (cell, vertex) order.
    fn near(&self, vertices: &[[f64; 3]], x: f64, y: f64) -> Vec<(usize, f64)> {
        let (cx, cy) = self.cell(x, y);
        let mut out = Vec::new();
        for column in cx.saturating_sub(1)..=cx.saturating_add(1) {
            for row in cy.saturating_sub(1)..=cy.saturating_add(1) {
                let start = self
                    .sorted
                    .partition_point(|&(cell, _)| cell < (column, row));
                for &(cell, index) in self.sorted.iter().skip(start) {
                    if cell != (column, row) {
                        break;
                    }
                    if let Some(&[px, py, _]) = vertices.get(index) {
                        let squared = (px - x).powi(2) + (py - y).powi(2);
                        if squared <= self.radius * self.radius {
                            out.push((index, squared));
                        }
                    }
                }
            }
        }
        out
    }
}

/// Relax every movable vertex within `radius` of a moved one.
pub(super) fn relax(
    vertices: &[[f64; 3]],
    movable: &[bool],
    radius: f64,
    heights: &mut [f64],
    owners: &mut [Option<u32>],
) {
    let grid = Grid::new(vertices, radius);
    let moved = |index: usize| {
        heights
            .get(index)
            .zip(vertices.get(index))
            .is_some_and(|(new, old)| (new - old[2]).abs() > 0.0)
    };
    // Region: movable vertices near a moved one, with that nearest moved vertex.
    let mut nearest: Vec<Option<(f64, usize)>> = vec![None; vertices.len()];
    for (index, &[x, y, _]) in vertices.iter().enumerate() {
        if !moved(index) {
            continue;
        }
        for (other, squared) in grid.near(vertices, x, y) {
            if !movable.get(other).copied().unwrap_or(false) {
                continue;
            }
            if let Some(slot) = nearest.get_mut(other)
                && slot.is_none_or(|(best, at)| squared < best || (squared <= best && index < at))
            {
                *slot = Some((squared, index));
            }
        }
    }
    let sigma = radius / 3.0;
    let before = heights.to_vec();
    for (index, near) in nearest.iter().enumerate() {
        let (Some(&(_, source)), Some(&[x, y, _])) = (near.as_ref(), vertices.get(index)) else {
            continue;
        };
        let (mut weighted, mut total) = (0.0, 0.0);
        for (other, squared) in grid.near(vertices, x, y) {
            let weight = (-squared / (2.0 * sigma * sigma)).exp();
            weighted += weight * before.get(other).copied().unwrap_or(0.0);
            total += weight;
        }
        if total > 0.0
            && let Some(slot) = heights.get_mut(index)
        {
            *slot = weighted / total;
        }
        if owners.get(index).is_some_and(Option::is_none) {
            let inherited = owners.get(source).copied().flatten();
            if let Some(slot) = owners.get_mut(index) {
                *slot = inherited;
            }
        }
    }
}
