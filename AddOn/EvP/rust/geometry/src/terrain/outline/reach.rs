//! Which outline edges can move the ground at a point (#149 T4): a grid over
//! the edges, searched nearest first and pruned by bounds, so a height costs
//! the few edges whose faces can still matter rather than the whole outline.
//!
//! **Bound.** A fill face is never above `z_max - grade * (d - rounding)`, `d`
//! the plan distance to its edge (its rounded top never rises above the edge,
//! and the straight face falls at its grade); a cut face mirrors it, negated.
//! A grid cell bounds its edges together: their highest top, slowest grade
//! and largest rounding, at the cell's distance.
//!
//! **Floor.** A fill face at or below `ground - grade * rounding` eases to the
//! ground bit for bit, and the crease lift ([`super::blend`]) is at most a
//! quarter of a knee no larger than `K = 2 * steepest * roundest`. So a face
//! at or below `ground - G - 1.25 K` (`G` the largest toe gap) changes neither
//! the eased faces nor the lifted winner.
//!
//! **Best.** Once a face within the limit reaches `best`, a face below
//! `best - K` changes nothing either: it is not the winner, it is a knee or
//! more below it so lends no lift, and it eases to strictly less than the best
//! does (`K >= 2 G`, so the toe ease cannot close the gap). The nearest edges
//! are searched first to raise `best` early; every other cell and edge whose
//! bound is below the floor or below `best - K` is skipped.
//!
//! **Exactness.** What is skipped cannot change any maximum, and the envelope
//! breaks a tie by the edge's index, not by the order it was found in, so the
//! heights are the full scan's bit for bit.

use std::cell::RefCell;

use super::{Edge, plan};

/// How high one side's faces can reach: the top edge height (negated for
/// cut), the slowest finite grade and the largest rounding. No face: a top of
/// minus infinity.
#[derive(Clone, Copy, Debug)]
struct Side {
    top: f64,
    slowest: f64,
    rounding: f64,
}

impl Side {
    const NONE: Self = Self {
        top: f64::NEG_INFINITY,
        slowest: f64::INFINITY,
        rounding: 0.0,
    };

    fn of(grade: f64, top: f64, rounding: f64) -> Self {
        if grade.is_finite() {
            Self {
                top,
                slowest: grade,
                rounding,
            }
        } else {
            Self::NONE
        }
    }

    fn with(self, other: Self) -> Self {
        Self {
            top: self.top.max(other.top),
            slowest: self.slowest.min(other.slowest),
            rounding: self.rounding.max(other.rounding),
        }
    }

    /// The highest any face can be at plan distance `distance`.
    fn at(self, distance: f64) -> f64 {
        if self.top == f64::NEG_INFINITY {
            return f64::NEG_INFINITY;
        }
        if self.slowest <= 0.0 {
            return self.top;
        }
        self.slowest.mul_add(-(distance - self.rounding), self.top)
    }

    /// The plan distance past which no face reaches `level`.
    fn radius(self, level: f64) -> f64 {
        if self.top == f64::NEG_INFINITY {
            return f64::NEG_INFINITY;
        }
        if self.slowest <= 0.0 {
            return f64::INFINITY;
        }
        self.rounding + ((self.top - level) / self.slowest).max(0.0)
    }
}

/// Both sides' bounds.
#[derive(Clone, Copy, Debug)]
struct Bound {
    fill: Side,
    cut: Side,
}

impl Bound {
    const NONE: Self = Self {
        fill: Side::NONE,
        cut: Side::NONE,
    };

    fn with(self, other: Self) -> Self {
        Self {
            fill: self.fill.with(other.fill),
            cut: self.cut.with(other.cut),
        }
    }
}

/// The levels a face must still reach to matter, per side (fill, cut negated).
#[derive(Clone, Copy)]
struct Levels {
    floor: [f64; 2],
    best: [f64; 2],
    knee: f64,
}

impl Levels {
    /// Which sides of a bound at `distance` may still matter: above the floor
    /// and no less than the best less a knee.
    fn sides(&self, bound: &Bound, distance: f64) -> [bool; 2] {
        let [fill_floor, cut_floor] = self.floor;
        let [fill_best, cut_best] = self.best;
        [
            (bound.fill, fill_floor, fill_best),
            (bound.cut, cut_floor, cut_best),
        ]
        .map(|(side, floor, best)| {
            let high = side.at(distance);
            high > floor && high >= best - self.knee
        })
    }

    fn reach(&self, bound: &Bound, distance: f64) -> bool {
        self.sides(bound, distance).contains(&true)
    }

    fn radius(&self, all: &Bound) -> f64 {
        let [fill_floor, cut_floor] = self.floor;
        let [fill_best, cut_best] = self.best;
        let fill = all.fill.radius(fill_floor.max(fill_best - self.knee));
        fill.max(all.cut.radius(cut_floor.max(cut_best - self.knee)))
    }
}

/// A grid of the outline edges' plan bounding boxes, with bounds per edge and
/// per cell.
pub(super) struct Reach {
    all: Bound,
    edges: Vec<Bound>,
    boxes: Vec<[f64; 4]>,
    cells: Vec<Vec<u32>>,
    cell_bounds: Vec<Bound>,
    /// `G + 1.25 K`: how far below the ground (above it, for cut) a face may
    /// lie and still matter.
    margin: f64,
    /// `K`.
    knee: f64,
    origin: [f64; 2],
    cell: f64,
    columns: u32,
    rows: u32,
    /// Visit every edge, pruning nothing (the full scan, for tests).
    exhaustive: bool,
}

/// Largest number of grid cells.
const MAX_CELLS: f64 = 262_144.0;

thread_local! {
    /// Per edge, the search that last visited it, so an edge in several cells
    /// is visited once without sorting.
    static SEEN: RefCell<(Vec<u32>, u32)> = const { RefCell::new((Vec::new(), 0)) };
}

impl Reach {
    pub(super) fn new(edges: &[Edge]) -> Self {
        let bounds: Vec<Bound> = edges
            .iter()
            .map(|edge| {
                let [a, b] = edge.segment;
                Bound {
                    fill: Side::of(edge.grade.fill, a[2].max(b[2]), edge.grade.rounding),
                    cut: Side::of(edge.grade.cut, -a[2].min(b[2]), edge.grade.rounding),
                }
            })
            .collect();
        let all = bounds
            .iter()
            .fold(Bound::NONE, |all, bound| all.with(*bound));
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
        let knee = 2.0 * steepest * roundest;
        let margin = 1.25f64.mul_add(knee, gap);
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
        let extent = boxes.iter().fold(
            [
                f64::INFINITY,
                f64::INFINITY,
                f64::NEG_INFINITY,
                f64::NEG_INFINITY,
            ],
            |[x0, y0, x1, y1], &[a, b, c, d]| [x0.min(a), y0.min(b), x1.max(c), y1.max(d)],
        );
        let (width, height) = (
            (extent[2] - extent[0]).max(0.0),
            (extent[3] - extent[1]).max(0.0),
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
        let origin = if extent[0].is_finite() {
            [extent[0], extent[1]]
        } else {
            [0.0, 0.0]
        };
        let size = columns as usize * rows as usize;
        let (mut cells, mut cell_bounds) = (vec![Vec::new(); size], vec![Bound::NONE; size]);
        let at =
            |value: f64, from: f64, count: u32| plan::clamp_index((value - from) / cell, count - 1);
        for ((index, &[x0, y0, x1, y1]), bound) in (0_u32..).zip(&boxes).zip(&bounds) {
            for row in at(y0, origin[1], rows)..=at(y1, origin[1], rows) {
                for column in at(x0, origin[0], columns)..=at(x1, origin[0], columns) {
                    let slot = row as usize * columns as usize + column as usize;
                    if let (Some(list), Some(cell)) =
                        (cells.get_mut(slot), cell_bounds.get_mut(slot))
                    {
                        list.push(index);
                        *cell = cell.with(*bound);
                    }
                }
            }
        }
        Self {
            all,
            edges: bounds,
            boxes,
            cells,
            cell_bounds,
            margin,
            knee,
            origin,
            cell,
            columns,
            rows,
            exhaustive: false,
        }
    }

    /// Every edge at every point: the full scan the search must equal.
    #[cfg(test)]
    pub(super) fn everything(edges: &[Edge]) -> Self {
        let mut reach = Self::new(edges);
        reach.exhaustive = true;
        reach
    }

    /// Visit, once each, every edge whose faces may move the ground at height
    /// `base` at `(x, y)`, with which of its sides (fill, cut) may. `visit`
    /// grades those and returns its fill face's height and its cut face's
    /// negated height where they are within the limit, which raise the levels
    /// the rest must reach. A side once skipped stays irrelevant: the levels
    /// only rise.
    pub(super) fn search(
        &self,
        x: f64,
        y: f64,
        base: f64,
        mut visit: impl FnMut(u32, [bool; 2]) -> [Option<f64>; 2],
    ) {
        let mut levels = Levels {
            floor: [base - self.margin, -(base + self.margin)],
            best: [f64::NEG_INFINITY; 2],
            knee: self.knee,
        };
        let distance = |[x0, y0, x1, y1]: [f64; 4]| {
            let dx = (x0 - x).max(x - x1).max(0.0);
            let dy = (y0 - y).max(y - y1).max(0.0);
            dx.hypot(dy)
        };
        SEEN.with_borrow_mut(|(seen, stamp)| {
            if seen.len() != self.boxes.len() || *stamp == u32::MAX {
                *seen = vec![0; self.boxes.len()];
                *stamp = 0;
            }
            *stamp += 1;
            let mut cells = |radius: f64, levels: &mut Levels| {
                let whole = !radius.is_finite() || self.exhaustive || {
                    let span = 2.0 * radius / self.cell;
                    span * span >= f64::from(self.columns) * f64::from(self.rows)
                };
                let at = |value: f64, from: f64, count: u32| {
                    plan::clamp_index((value - from) / self.cell, count - 1)
                };
                let (c0, c1, r0, r1) = if whole {
                    (0, self.columns - 1, 0, self.rows - 1)
                } else {
                    (
                        at(x - radius, self.origin[0], self.columns),
                        at(x + radius, self.origin[0], self.columns),
                        at(y - radius, self.origin[1], self.rows),
                        at(y + radius, self.origin[1], self.rows),
                    )
                };
                for row in r0..=r1 {
                    for column in c0..=c1 {
                        let slot = row as usize * self.columns as usize + column as usize;
                        let (Some(list), Some(bound)) =
                            (self.cells.get(slot), self.cell_bounds.get(slot))
                        else {
                            continue;
                        };
                        let near = [
                            f64::from(column).mul_add(self.cell, self.origin[0]),
                            f64::from(row).mul_add(self.cell, self.origin[1]),
                            f64::from(column + 1).mul_add(self.cell, self.origin[0]),
                            f64::from(row + 1).mul_add(self.cell, self.origin[1]),
                        ];
                        if !self.exhaustive && !levels.reach(bound, distance(near)) {
                            continue;
                        }
                        for &edge in list {
                            let Some(mark) = seen.get_mut(edge as usize) else {
                                continue;
                            };
                            if *mark == *stamp {
                                continue;
                            }
                            let (Some(&bound), Some(&plan)) =
                                (self.edges.get(edge as usize), self.boxes.get(edge as usize))
                            else {
                                continue;
                            };
                            let sides = if self.exhaustive {
                                [true; 2]
                            } else {
                                levels.sides(&bound, distance(plan))
                            };
                            if sides == [false; 2] {
                                continue;
                            }
                            *mark = *stamp;
                            for (k, height) in visit(edge, sides).into_iter().enumerate() {
                                if let (Some(height), Some(best)) = (height, levels.best.get_mut(k))
                                {
                                    *best = best.max(height);
                                }
                            }
                        }
                    }
                }
            };
            // The nearest cells first, to raise the best faces; then out to
            // where nothing can reach what they set.
            cells(2.0 * self.cell, &mut levels);
            let radius = levels.radius(&self.all);
            if radius >= 0.0 {
                cells(radius, &mut levels);
            }
        });
    }
}
