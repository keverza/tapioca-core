//! Contour lines of a height field sampled on a plan grid (#175): the terrain preview's
//! instant lane.
//!
//! The terrain design is a field before it is a mesh: the ground, the region entries composed
//! over it and the outline's faces graded over that are all evaluable at any plan point. The
//! mesh (toe, creases, the refining mesher) samples that field to a tolerance and takes seconds;
//! its contours wait for it. This lane samples the same field on a regular grid and traces its
//! contours by marching squares, in milliseconds, so the preview's contours follow every edit
//! while the mesh is still being built. They are the field's contours to within the grid's
//! bilinear interpolation, not the mesh's: the mesh's replace them once it lands.

use std::collections::HashMap;

/// One contour line at `level`, metres; its points lie on the level.
#[derive(Clone, Debug, PartialEq)]
pub struct Contour {
    /// The level, metres.
    pub level: f64,
    /// Plan points with `z == level`, in order along the line.
    pub points: Vec<[f64; 3]>,
    /// Whether the line closes on itself (its last point joins its first).
    pub closed: bool,
}

/// A field's contours on one grid.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Contours {
    /// The interval between levels, metres: the one asked, or the one chosen when none was.
    pub interval: f64,
    /// The grid spacing sampled, metres.
    pub step: f64,
    /// The lowest and highest heights sampled, metres (`[inf, -inf]` when none was).
    pub range: [f64; 2],
    /// Every level's lines, levels ascending.
    pub lines: Vec<Contour>,
}

/// At most this many grid nodes: a larger area is sampled coarser (2 048 x 2 048, about the
/// detail of a 30 000 m^2 parcel at 0.1 m).
const MAX_NODES: f64 = 4_194_304.0;

/// At most this many levels: a finer interval over a large range is coarsened.
const MAX_LEVELS: f64 = 1000.0;

/// A height within this of a level is on it, metres: a level plateau (a pavement, level ground)
/// carries the noise of interpolating its corners, and must not trace that noise as a level.
const ON_LEVEL: f64 = 1e-6;

/// Points closer than this to the line through their neighbours are dropped, metres.
const SIMPLIFY: f64 = 0.005;

/// The plan box `lo`-`hi` sampled every `step` metres (or coarser, see [`MAX_NODES`]) with
/// `height` (`None` where there is no ground), and its contours traced every `interval` metres;
/// `interval <= 0` asks for a round interval giving about ten levels over the range sampled.
pub fn trace(
    lo: [f64; 2],
    hi: [f64; 2],
    step: f64,
    interval: f64,
    height: &(dyn Fn(f64, f64) -> Option<f64> + Sync),
) -> Contours {
    let Some(grid) = Grid::sample(lo, hi, step, &|x, y| height(x, y).map(|z| [z, z])) else {
        return Contours::default();
    };
    grid.trace(interval)
}

impl Grid {
    /// This grid's contours every `interval` metres (see [`trace`]).
    pub(super) fn trace(&self, interval: f64) -> Contours {
        let range = self.range();
        let asked = if interval.is_finite() && interval > 0.0 {
            interval
        } else {
            nice(range[1] - range[0], 10.0).max(0.1)
        };
        let span = range[1] - range[0];
        let interval = if span.is_finite() && span / asked > MAX_LEVELS {
            nice(span, MAX_LEVELS)
        } else {
            asked
        };
        Contours {
            interval,
            step: self.step,
            range,
            lines: self.contours(interval),
        }
    }
}

/// A round step (1, 2, 2.5 or 5 times a power of ten) giving about `count` steps over `span`: the
/// editor's own rule for the built surface's contours (`edit::NiceStep`), so its levels do not
/// change when the mesh's contours replace these.
fn nice(span: f64, count: f64) -> f64 {
    if !(span.is_finite() && span > 0.0) {
        return 0.0;
    }
    let rough = span / count;
    let power = 10f64.powf(rough.log10().floor());
    let fraction = rough / power;
    let round = if fraction < 1.5 {
        1.0
    } else if fraction < 2.25 {
        2.0
    } else if fraction < 3.5 {
        2.5
    } else if fraction < 7.5 {
        5.0
    } else {
        10.0
    };
    round * power
}

/// Heights at regular plan nodes, `NaN` where there is no ground: the design's (`values`, what
/// is contoured) and the base's it was graded from.
pub(super) struct Grid {
    pub(super) x0: f64,
    pub(super) y0: f64,
    pub(super) step: f64,
    pub(super) columns: usize,
    pub(super) rows: usize,
    pub(super) values: Vec<f64>,
    pub(super) base: Vec<f64>,
}

impl Grid {
    /// The box `lo`-`hi` every `step` metres (coarser past [`MAX_NODES`]); `height` gives the
    /// design and the base height at a plan point, `None` where there is no ground.
    pub(super) fn sample(
        lo: [f64; 2],
        hi: [f64; 2],
        step: f64,
        height: &(dyn Fn(f64, f64) -> Option<[f64; 2]> + Sync),
    ) -> Option<Self> {
        let (width, depth) = (hi[0] - lo[0], hi[1] - lo[1]);
        if !(width.is_finite() && depth.is_finite() && width > 0.0 && depth > 0.0) {
            return None;
        }
        let step = if step.is_finite() && step > 0.0 {
            step
        } else {
            0.25
        }
        .max((width * depth / MAX_NODES).sqrt());
        let count = |extent: f64| {
            let cells = (extent / step).ceil();
            // Bounded by MAX_NODES above: each side has at most MAX_NODES / 2 nodes.
            #[expect(
                clippy::cast_possible_truncation,
                clippy::cast_sign_loss,
                reason = "a finite, non-negative cell count bounded by MAX_NODES"
            )]
            let cells = cells.clamp(1.0, MAX_NODES) as usize;
            cells + 1
        };
        let (columns, rows) = (count(width), count(depth));
        let at = |index: usize| f64::from(u32::try_from(index).unwrap_or(u32::MAX));
        let mut values = vec![f64::NAN; 2 * columns * rows];
        let threads = std::thread::available_parallelism()
            .map_or(1, std::num::NonZero::get)
            .clamp(1, 32);
        let rows_per = rows.div_ceil(threads).max(1);
        std::thread::scope(|scope| {
            for (chunk, slab) in values.chunks_mut(2 * rows_per * columns).enumerate() {
                scope.spawn(move || {
                    for (offset, row) in slab.chunks_mut(2 * columns).enumerate() {
                        let y = step.mul_add(at(chunk * rows_per + offset), lo[1]);
                        for (column, slot) in row.chunks_mut(2).enumerate() {
                            let x = step.mul_add(at(column), lo[0]);
                            if let Some(pair) =
                                height(x, y).filter(|pair| pair.iter().all(|z| z.is_finite()))
                            {
                                slot.copy_from_slice(&pair);
                            }
                        }
                    }
                });
            }
        });
        let (values, base) = values
            .chunks(2)
            .map(|pair| {
                (
                    pair.first().copied().unwrap_or(f64::NAN),
                    pair.get(1).copied().unwrap_or(f64::NAN),
                )
            })
            .unzip();
        Some(Self {
            x0: lo[0],
            y0: lo[1],
            step,
            columns,
            rows,
            values,
            base,
        })
    }

    fn range(&self) -> [f64; 2] {
        self.values
            .iter()
            .filter(|z| !z.is_nan())
            .fold([f64::INFINITY, f64::NEG_INFINITY], |[lo, hi], &z| {
                [lo.min(z), hi.max(z)]
            })
    }

    pub(super) fn value(&self, column: usize, row: usize) -> f64 {
        self.values
            .get(row * self.columns + column)
            .copied()
            .unwrap_or(f64::NAN)
    }

    /// The node's plan position.
    pub(super) fn node(&self, column: usize, row: usize) -> [f64; 2] {
        let at = |index: usize| f64::from(u32::try_from(index).unwrap_or(u32::MAX));
        [
            self.step.mul_add(at(column), self.x0),
            self.step.mul_add(at(row), self.y0),
        ]
    }

    /// Where the grid edge `edge` crosses `level`: horizontal edges are `2 * node`, from the
    /// node to its right neighbour; vertical ones `2 * node + 1`, to the node above.
    fn crossing(&self, edge: usize, level: f64) -> [f64; 3] {
        let node = edge / 2;
        let (column, row) = (node % self.columns, node / self.columns);
        let (other_column, other_row) = if edge.is_multiple_of(2) {
            (column + 1, row)
        } else {
            (column, row + 1)
        };
        let (low, high) = (self.value(column, row), self.value(other_column, other_row));
        let along = ((level - low) / (high - low)).clamp(0.0, 1.0);
        let (from, to) = (self.node(column, row), self.node(other_column, other_row));
        [
            (to[0] - from[0]).mul_add(along, from[0]),
            (to[1] - from[1]).mul_add(along, from[1]),
            level,
        ]
    }

    /// Every level's lines, by marching squares over the cells whose four corners have ground.
    fn contours(&self, interval: f64) -> Vec<Contour> {
        let range = self.range();
        if range[0]
            .partial_cmp(&range[1])
            .is_none_or(std::cmp::Ordering::is_gt)
            || interval.partial_cmp(&0.0) != Some(std::cmp::Ordering::Greater)
        {
            return Vec::new();
        }
        // A level crosses where some corner is above it and some is not (`z > level`). The level
        // ranges here are one wider each way than that: a height on a level divides to just
        // past it in floating point, and the corner test below decides.
        let first = (range[0] / interval).floor();
        let last = (range[1] / interval).ceil();
        if first
            .partial_cmp(&last)
            .is_none_or(std::cmp::Ordering::is_gt)
        {
            return Vec::new();
        }
        // Bounded by MAX_LEVELS in `trace` (plus the rounding of both ends).
        #[expect(
            clippy::cast_possible_truncation,
            clippy::cast_sign_loss,
            reason = "a finite level count bounded by MAX_LEVELS"
        )]
        let count = (last - first).clamp(0.0, MAX_LEVELS + 2.0) as usize + 1;
        let level_of =
            |index: usize| interval * (first + f64::from(u32::try_from(index).unwrap_or(0)));
        let mut segments: Vec<Vec<[usize; 2]>> = vec![Vec::new(); count];
        let columns = self.columns;
        for row in 0..self.rows.saturating_sub(1) {
            for column in 0..columns.saturating_sub(1) {
                let corners = [
                    self.value(column, row),
                    self.value(column + 1, row),
                    self.value(column + 1, row + 1),
                    self.value(column, row + 1),
                ];
                if corners.iter().any(|z| z.is_nan()) {
                    continue;
                }
                let lo = corners.iter().copied().fold(f64::INFINITY, f64::min);
                let hi = corners.iter().copied().fold(f64::NEG_INFINITY, f64::max);
                let node = row * columns + column;
                // Bottom, right, top, left.
                let edges = [
                    2 * node,
                    2 * (node + 1) + 1,
                    2 * (node + columns),
                    2 * node + 1,
                ];
                let from = ((lo / interval).floor() - first).max(0.0);
                let to = ((hi / interval).ceil() - first).min(last - first);
                if from.partial_cmp(&to).is_none_or(std::cmp::Ordering::is_gt) {
                    continue;
                }
                #[expect(
                    clippy::cast_possible_truncation,
                    clippy::cast_sign_loss,
                    reason = "non-negative level indices below `count`"
                )]
                let (from, to) = (from as usize, to as usize);
                for (index, list) in segments.iter_mut().enumerate().take(to + 1).skip(from) {
                    let level = level_of(index);
                    let above = corners.map(|z| z > level + ON_LEVEL);
                    let case = above
                        .iter()
                        .enumerate()
                        .fold(0_usize, |bits, (k, &up)| bits | (usize::from(up) << k));
                    let centre_above = corners.iter().sum::<f64>() / 4.0 > level + ON_LEVEL;
                    for [a, b] in cell_segments(case, centre_above) {
                        if let (Some(&p), Some(&q)) = (edges.get(a), edges.get(b)) {
                            list.push([p, q]);
                        }
                    }
                }
            }
        }
        let mut lines = Vec::new();
        for (index, list) in segments.iter().enumerate() {
            let level = level_of(index);
            for (edges, closed) in chain(list) {
                let points: Vec<[f64; 3]> = edges
                    .iter()
                    .map(|&edge| self.crossing(edge, level))
                    .collect();
                let points = simplify(&points);
                if points.len() >= 2 {
                    lines.push(Contour {
                        level,
                        points,
                        closed,
                    });
                }
            }
        }
        lines
    }
}

/// The cell's segments as pairs of its edges (0 bottom, 1 right, 2 top, 3 left) for corner case
/// `case` (bit k set where corner k -- bottom-left, bottom-right, top-right, top-left -- is
/// above the level); a saddle is split by its centre.
fn cell_segments(case: usize, centre_above: bool) -> Vec<[usize; 2]> {
    match case {
        1 | 14 => vec![[3, 0]],
        2 | 13 => vec![[0, 1]],
        3 | 12 => vec![[3, 1]],
        4 | 11 => vec![[1, 2]],
        6 | 9 => vec![[0, 2]],
        7 | 8 => vec![[3, 2]],
        5 if centre_above => vec![[3, 2], [0, 1]],
        5 => vec![[3, 0], [1, 2]],
        10 if centre_above => vec![[3, 0], [1, 2]],
        10 => vec![[3, 2], [0, 1]],
        _ => Vec::new(),
    }
}

/// Segments joined at their shared grid edges into lines, each as its edges in order, and
/// whether it closes. Every grid edge meets at most two segments of one level.
fn chain(segments: &[[usize; 2]]) -> Vec<(Vec<usize>, bool)> {
    let mut at: HashMap<usize, [Option<usize>; 2]> = HashMap::with_capacity(segments.len() * 2);
    for (index, &[p, q]) in segments.iter().enumerate() {
        for edge in [p, q] {
            let slot = at.entry(edge).or_insert([None, None]);
            if slot[0].is_none() {
                slot[0] = Some(index);
            } else {
                slot[1] = Some(index);
            }
        }
    }
    let other = |edge: usize, segment: usize| -> Option<usize> {
        let slot = at.get(&edge)?;
        slot.iter()
            .flatten()
            .copied()
            .find(|&candidate| candidate != segment)
    };
    let far = |segment: usize, edge: usize| -> Option<usize> {
        let [p, q] = *segments.get(segment)?;
        Some(if p == edge { q } else { p })
    };
    let mut used = vec![false; segments.len()];
    let mut lines = Vec::new();
    // Open lines first, from an end (an edge only one segment meets), then the closed loops.
    let ends: Vec<(usize, usize)> = segments
        .iter()
        .enumerate()
        .flat_map(|(index, &[p, q])| [(index, p), (index, q)])
        .filter(|&(index, edge)| other(edge, index).is_none())
        .collect();
    let starts = ends.into_iter().chain(
        segments
            .iter()
            .enumerate()
            .map(|(index, &[p, _])| (index, p)),
    );
    for (start, edge) in starts {
        if used.get(start).copied().unwrap_or(true) {
            continue;
        }
        let mut line = vec![edge];
        let (mut segment, mut edge) = (start, edge);
        let mut closed = false;
        loop {
            if let Some(flag) = used.get_mut(segment) {
                *flag = true;
            }
            let Some(next_edge) = far(segment, edge) else {
                break;
            };
            line.push(next_edge);
            match other(next_edge, segment) {
                Some(next) if next == start => {
                    closed = true;
                    break;
                }
                Some(next) if !used.get(next).copied().unwrap_or(true) => {
                    (segment, edge) = (next, next_edge);
                }
                _ => break,
            }
        }
        if closed {
            line.pop();
        }
        lines.push((line, closed));
    }
    lines
}

/// `points` without those within [`SIMPLIFY`] of the line through the kept neighbours
/// (Ramer-Douglas-Peucker); both ends are kept, so a closed line keeps its seam.
fn simplify(points: &[[f64; 3]]) -> Vec<[f64; 3]> {
    if points.len() < 3 {
        return points.to_vec();
    }
    let mut keep = vec![false; points.len()];
    let last = points.len() - 1;
    if let Some(first) = keep.first_mut() {
        *first = true;
    }
    if let Some(end) = keep.get_mut(last) {
        *end = true;
    }
    let mut stack = vec![(0_usize, last)];
    while let Some((a, b)) = stack.pop() {
        if b <= a + 1 {
            continue;
        }
        let (Some(pa), Some(pb)) = (points.get(a), points.get(b)) else {
            continue;
        };
        let (dx, dy) = (pb[0] - pa[0], pb[1] - pa[1]);
        let length = dx.hypot(dy);
        let mut worst = (0.0_f64, a);
        for (k, p) in points.iter().enumerate().take(b).skip(a + 1) {
            let distance = if length > 0.0 {
                (dx.mul_add(p[1] - pa[1], -dy * (p[0] - pa[0]))).abs() / length
            } else {
                (p[0] - pa[0]).hypot(p[1] - pa[1])
            };
            if distance > worst.0 {
                worst = (distance, k);
            }
        }
        if worst.0 > SIMPLIFY {
            if let Some(flag) = keep.get_mut(worst.1) {
                *flag = true;
            }
            stack.push((a, worst.1));
            stack.push((worst.1, b));
        }
    }
    points
        .iter()
        .zip(&keep)
        .filter(|(_, kept)| **kept)
        .map(|(p, _)| *p)
        .collect()
}

#[cfg(test)]
mod tests;
