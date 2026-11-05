//! Which outline edges can move the ground at a point (#149 T4): a grid over
//! the edges and a radius past which none can, so a height costs the edges
//! near it rather than the whole outline.
//!
//! **Bound.** A fill face is never above `z_max - grade * (d - rounding)`, `d`
//! the plan distance to its edge (its rounded top never rises above the edge,
//! and the straight face falls at its grade). A fill face at or below
//! `ground - grade * rounding` eases to the ground bit for bit, and the crease
//! lift ([`super::blend`]) is at most a quarter of a knee no larger than
//! `2 * steepest * roundest`. So a face below `ground - G - 1.25 K` (`G` the
//! largest toe gap, `K` that knee) changes neither the eased faces nor the
//! lifted winner: past the radius where every edge's bound falls below it,
//! no edge matters. A cut face mirrors it. Where an edge's face is vertical
//! (an infinite grade) it has none and does not count.
//!
//! **Exactness.** The candidates are every edge whose bounding box is within
//! the radius, in outline order, so the faces' maxima, the envelope's winner
//! on a tie and every eased move are the full scan's bit for bit.

use super::{Edge, plan};

/// The bound of one side's faces: the extreme edge height (negated for cut),
/// the slowest finite grade and the largest rounding.
#[derive(Clone, Copy, Debug)]
struct Side {
    top: f64,
    slowest: f64,
    rounding: f64,
}

impl Side {
    /// The plan distance past which no face of this side reaches `level`
    /// (in its own sense: a fill face above it, a cut face below it).
    fn radius(self, level: f64) -> f64 {
        if self.slowest <= 0.0 {
            return f64::INFINITY;
        }
        self.rounding + ((self.top - level) / self.slowest).max(0.0)
    }
}

/// A grid of the outline edges' plan bounding boxes.
pub(super) struct Reach {
    fill: Option<Side>,
    cut: Option<Side>,
    /// `G + 1.25 K`: how far below the ground (above it, for cut) a face may
    /// lie and still matter.
    margin: f64,
    origin: [f64; 2],
    cell: f64,
    columns: u32,
    rows: u32,
    cells: Vec<Vec<u32>>,
    boxes: Vec<[f64; 4]>,
}

/// Largest number of grid cells.
const MAX_CELLS: f64 = 262_144.0;

impl Reach {
    pub(super) fn new(edges: &[Edge]) -> Self {
        let side = |pick: fn(&Edge) -> f64, top: fn(&Edge) -> f64| {
            edges.iter().filter(|edge| pick(edge).is_finite()).fold(
                None,
                |side: Option<Side>, edge| {
                    let grade = pick(edge);
                    Some(side.map_or(
                        Side {
                            top: top(edge),
                            slowest: grade,
                            rounding: edge.grade.rounding,
                        },
                        |s| Side {
                            top: s.top.max(top(edge)),
                            slowest: s.slowest.min(grade),
                            rounding: s.rounding.max(edge.grade.rounding),
                        },
                    ))
                },
            )
        };
        let fill = side(
            |edge| edge.grade.fill,
            |edge| edge.segment[0][2].max(edge.segment[1][2]),
        );
        let cut = side(
            |edge| edge.grade.cut,
            |edge| -edge.segment[0][2].min(edge.segment[1][2]),
        );
        let finite = |g: f64| if g.is_finite() { g } else { 0.0 };
        let (gap, steepest, roundest) =
            edges
                .iter()
                .fold((0.0_f64, 0.0_f64, 0.0_f64), |(gap, steep, round), edge| {
                    let grade = edge.grade;
                    let faces = finite(grade.fill).max(finite(grade.cut));
                    (
                        gap.max(faces * grade.rounding),
                        steep.max(faces),
                        round.max(grade.rounding),
                    )
                });
        let margin = 2.5f64.mul_add(steepest * roundest, gap);
        let boxes: Vec<[f64; 4]> = edges
            .iter()
            .map(|edge| {
                let [a, b] = edge.segment;
                [
                    a[0].min(b[0]),
                    a[1].min(b[1]),
                    a[0].max(b[0]),
                    a[1].max(b[1]),
                ]
            })
            .collect();
        let bounds = boxes.iter().fold(
            [
                f64::INFINITY,
                f64::INFINITY,
                f64::NEG_INFINITY,
                f64::NEG_INFINITY,
            ],
            |[x0, y0, x1, y1], &[a, b, c, d]| [x0.min(a), y0.min(b), x1.max(c), y1.max(d)],
        );
        let (width, height) = (
            (bounds[2] - bounds[0]).max(0.0),
            (bounds[3] - bounds[1]).max(0.0),
        );
        // About one edge per cell, no finer than a decimetre, and capped.
        let count = f64::from(u32::try_from(edges.len()).unwrap_or(u32::MAX).max(1));
        let cell = (width * height / count)
            .sqrt()
            .max((width * height / MAX_CELLS).sqrt())
            .max(0.1);
        // MAX_CELLS bounds both counts well inside a u32.
        let (columns, rows) = if width.is_finite() && height.is_finite() {
            (
                plan::clamp_index(width / cell, u32::MAX - 1) + 1,
                plan::clamp_index(height / cell, u32::MAX - 1) + 1,
            )
        } else {
            (1, 1)
        };
        let mut cells = vec![Vec::new(); columns as usize * rows as usize];
        let origin = if bounds[0].is_finite() {
            [bounds[0], bounds[1]]
        } else {
            [0.0, 0.0]
        };
        let at =
            |value: f64, from: f64, count: u32| plan::clamp_index((value - from) / cell, count - 1);
        for (index, &[x0, y0, x1, y1]) in (0_u32..).zip(&boxes) {
            for row in at(y0, origin[1], rows)..=at(y1, origin[1], rows) {
                for column in at(x0, origin[0], columns)..=at(x1, origin[0], columns) {
                    if let Some(list) =
                        cells.get_mut(row as usize * columns as usize + column as usize)
                    {
                        list.push(index);
                    }
                }
            }
        }
        Self {
            fill,
            cut,
            margin,
            origin,
            cell,
            columns,
            rows,
            cells,
            boxes,
        }
    }

    /// Every edge at every point: the full scan the index must equal.
    #[cfg(test)]
    pub(super) fn everything(edges: &[Edge]) -> Self {
        let mut reach = Self::new(edges);
        reach.fill = Some(Side {
            top: f64::INFINITY,
            slowest: 0.0,
            rounding: 0.0,
        });
        reach
    }

    /// The plan distance past which no edge's face moves ground at `base`.
    fn radius(&self, base: f64) -> f64 {
        let fill = self
            .fill
            .map_or(f64::NEG_INFINITY, |side| side.radius(base - self.margin));
        let cut = self
            .cut
            .map_or(f64::NEG_INFINITY, |side| side.radius(-(base + self.margin)));
        fill.max(cut)
    }

    /// Into `out`, ascending, every edge that may move ground at height
    /// `base` at `(x, y)`.
    pub(super) fn candidates(&self, x: f64, y: f64, base: f64, out: &mut Vec<u32>) {
        out.clear();
        let radius = self.radius(base);
        if radius < 0.0 {
            return;
        }
        let near = |&[x0, y0, x1, y1]: &[f64; 4]| {
            let dx = (x0 - x).max(x - x1).max(0.0);
            let dy = (y0 - y).max(y - y1).max(0.0);
            dx.hypot(dy) <= radius
        };
        let span = 2.0 * radius / self.cell;
        let cells = f64::from(self.columns) * f64::from(self.rows);
        if !radius.is_finite() || span * span >= cells {
            out.extend(
                (0_u32..)
                    .zip(&self.boxes)
                    .filter(|(_, b)| near(b))
                    .map(|(i, _)| i),
            );
            return;
        }
        let at = |value: f64, from: f64, count: u32| {
            plan::clamp_index((value - from) / self.cell, count - 1)
        };
        let (c0, c1) = (
            at(x - radius, self.origin[0], self.columns),
            at(x + radius, self.origin[0], self.columns),
        );
        let (r0, r1) = (
            at(y - radius, self.origin[1], self.rows),
            at(y + radius, self.origin[1], self.rows),
        );
        for row in r0..=r1 {
            for column in c0..=c1 {
                if let Some(list) = self
                    .cells
                    .get(row as usize * self.columns as usize + column as usize)
                {
                    out.extend(
                        list.iter()
                            .copied()
                            .filter(|&i| self.boxes.get(i as usize).is_some_and(near)),
                    );
                }
            }
        }
        out.sort_unstable();
        out.dedup();
    }
}
