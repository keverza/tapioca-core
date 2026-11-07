//! #149 T2: the toe as breaklines.
//!
//! The toe is where the faces leave the ground: the boundary of the set of plan
//! points the outline design moves. Between offset rings the mesh interpolated
//! across it, so a face's kink at the ground fell inside triangles. Here it is
//! traced from the same faces the mesh is graded with ([`Faces::over`]) and
//! handed to the rebuild as open breaklines. A rounded toe is a curve about a
//! rounding wide: the lines where its lift crosses fractions of the rounding
//! height are traced too, so the mesh follows it across its width.
//!
//! **Tracing.** Each grid node is evaluated once, for every level. A coarse
//! grid (three fine steps) is sampled first; the fine grid is evaluated only in
//! the coarse cells where some level changes, so the cost follows the toe's
//! length, not the region's area. (A toe bump narrower than a coarse cell that
//! enters and leaves through one coarse edge is cut off there.) Node samples
//! and the foot's bisections are independent and evaluated on all cores; every
//! result lands in its own slot, so the lines are the same whatever the count. Marching squares joins
//! each level's crossings into polylines (a saddle cell is decided by its
//! centre): the foot's crossings are bisected on the moved predicate, the inner
//! levels' interpolated from the nodes' lift, which is continuous there. Each
//! piece left after the clearance below is then simplified to the tolerance.
//!
//! **Clearance.** A breakline that grazes an existing vertex or runs along an
//! existing line makes the triangulator settle a crossing onto that vertex in
//! f64, and the rebuild refuses (the T3 attempt's failure). Every toe vertex
//! closer than the clearance to an existing line, spot or the region ring is
//! dropped, and so is every toe segment that passes that close to an existing
//! vertex; the polyline is split there. A crossing at an angle, away from
//! existing vertices, is kept: the rebuild inserts it.
//!
//! Plan metres; the lines' z is zero (a breakline's height is the design's).

use std::collections::{BTreeMap, BTreeSet};

use super::Faces;
use crate::terrain::plan;

/// How the toe is traced.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(in crate::terrain) struct Toe {
    /// Fine grid spacing the faces are sampled at, metres.
    pub step: f64,
    /// Smallest distance kept to an existing line, vertex or the ring, metres.
    pub clear: f64,
    /// Smallest distance a rounded toe's inner lines keep to an existing line, metres. Where a
    /// face is lower than its rounding they would hug the outline; a long strip that thin
    /// across the curved toe tilts its triangles far past the grade.
    pub level_clear: f64,
    /// Largest plan distance a simplified line strays from its traced points, metres.
    pub tolerance: f64,
}

impl Toe {
    /// The editor's toe: a quarter-metre grid, 2 cm clear of other lines (8 cm for a rounded
    /// toe's inner lines), simplified to 5 mm. Measured (#149 T2): at 15 cm the inner lines
    /// thin out and the rounded toe misses 1 cm; at 2 cm they hug a low face's outline and its
    /// triangles tilt past the grade (the 2026-11-05 17:26 path end); 5 and 8 cm do neither.
    pub(in crate::terrain) const DEFAULT: Self = Self {
        step: 0.25,
        clear: 0.02,
        level_clear: 0.08,
        tolerance: 0.005,
    };
}

/// Where a rounded toe's lift crosses these fractions of its rounding height:
/// the foot, then a quadratic toe's quarter points in plan, then its top.
const LEVELS: [f64; 5] = [0.0, 1.0 / 16.0, 0.25, 9.0 / 16.0, 1.0];
/// Fine steps per coarse step.
const COARSE: u32 = 3;
/// Bisection steps on a cell edge: a quarter metre over 2^10, a quarter millimetre.
const BISECT: u32 = 10;
/// Shortest toe piece handed to the rebuild, metres.
const SHORTEST: f64 = 0.1;

/// The faces at one node: whether the design moves the ground there, and by how much.
#[derive(Clone, Copy, Debug)]
struct Sample {
    moved: bool,
    lift: f64,
}

/// The levels' thresholds, and which a sample exceeds.
#[derive(Clone, Copy)]
struct Levels {
    fill: f64,
    cut: f64,
}

impl Levels {
    const fn count(self) -> usize {
        if self.fill > 0.0 || self.cut > 0.0 {
            LEVELS.len()
        } else {
            1
        }
    }

    /// The signed distance of a sample above level `k` (positive above), continuous in the lift
    /// for the inner levels; `None` where the side it lies on has no rounding.
    fn above(self, k: usize, sample: Sample) -> Option<f64> {
        let fraction = LEVELS.get(k).copied()?;
        if k == 0 {
            return Some(if sample.moved { 1.0 } else { -1.0 });
        }
        if sample.lift >= 0.0 {
            (self.fill > 0.0).then_some(sample.lift - fraction * self.fill)
        } else {
            (self.cut > 0.0).then_some(-sample.lift - fraction * self.cut)
        }
    }

    fn exceeds(self, k: usize, sample: Sample) -> bool {
        self.above(k, sample).is_some_and(|value| value > 0.0)
    }

    /// How many levels a sample exceeds, the coarse grid's class.
    fn class(self, sample: Option<Sample>) -> Option<usize> {
        let sample = sample?;
        Some(
            (0..self.count())
                .filter(|&k| self.exceeds(k, sample))
                .count(),
        )
    }
}

/// The toe of `faces` over `ground` inside `region` (closed xyz ring), clear of
/// `obstacles` (open xyz polylines), `spots` (xyz) and the ring. `rounding` is the
/// vertical height a fill and a cut toe rounds over (grade times rounding).
#[expect(
    clippy::too_many_arguments,
    reason = "the faces, their ground, the ring, everything to keep clear of and the toe settings are one tracing input"
)]
pub(in crate::terrain) fn lines(
    faces: &Faces<'_>,
    ground: &(dyn Fn(f64, f64) -> Option<f64> + Sync),
    region: &[f64],
    obstacles: &[&[f64]],
    spots: &[f64],
    rounding: [f64; 2],
    toe: Toe,
) -> Vec<Vec<f64>> {
    let ring = plan::vertices(region);
    if ring.len() < 3 || toe.step.partial_cmp(&0.0) != Some(std::cmp::Ordering::Greater) {
        return Vec::new();
    }
    let levels = Levels {
        fill: rounding[0],
        cut: rounding[1],
    };
    let evaluate = |x: f64, y: f64| -> Option<Sample> {
        if plan::inside(ring, x, y) != Some(true) {
            return None;
        }
        let base = ground(x, y)?;
        let (design, _) = faces.over(base, x, y);
        Some(Sample {
            moved: design.to_bits() != base.to_bits(),
            lift: design - base,
        })
    };
    let mut grid = Grid::new(ring, toe.step, &evaluate);
    let cells = grid.active(levels);
    let mut out: Vec<Vec<f64>> = Vec::new();
    for k in 0..levels.count() {
        let traced = grid.trace(levels, k, &cells);
        let clear = if k == 0 {
            toe.clear
        } else {
            toe.level_clear.max(toe.clear)
        };
        let mut all: Vec<&[f64]> = obstacles.to_vec();
        all.extend(out.iter().map(Vec::as_slice));
        let clearance = Clearance::new(&all, spots, ring, toe.clear, clear);
        let mut found = Vec::new();
        for line in traced {
            // Clear first on the dense line, then simplify each piece: a simplified chord
            // stays within the tolerance of points that keep the clearance.
            for piece in clearance.split(&line) {
                push_piece(&simplify(&piece, toe.tolerance), toe.clear, &mut found);
            }
        }
        out.extend(found);
    }
    out
}

/// A clear piece as one or (a closed loop) two open xyz breaklines.
fn push_piece(piece: &[[f64; 2]], clear: f64, out: &mut Vec<Vec<f64>>) {
    let length: f64 = piece
        .windows(2)
        .filter_map(|w| match w {
            [a, b] => Some((b[0] - a[0]).hypot(b[1] - a[1])),
            _ => None,
        })
        .sum();
    let (Some(&first), Some(&last)) = (piece.first(), piece.last()) else {
        return;
    };
    if piece.len() < 2 || length < SHORTEST {
        return;
    }
    let flat = |points: &[[f64; 2]]| {
        points
            .iter()
            .flat_map(|&[x, y]| [x, y, 0.0])
            .collect::<Vec<_>>()
    };
    let closed = (first[0] - last[0]).hypot(first[1] - last[1]) < clear;
    if let (true, Some((_, ring))) = (piece.len() > 3 && closed, piece.split_last()) {
        // A closed loop goes in as two open halves: a breakline is an open line.
        let (head, tail) = ring.split_at(ring.len() / 2);
        let mut first_half = head.to_vec();
        if let Some(&joint) = tail.first() {
            first_half.push(joint);
        }
        let mut second = tail.to_vec();
        second.push(first);
        out.push(flat(&first_half));
        out.push(flat(&second));
    } else {
        out.push(flat(piece));
    }
}

/// Douglas-Peucker: the fewest of `line`'s points within `tolerance` of it.
fn simplify(line: &[[f64; 2]], tolerance: f64) -> Vec<[f64; 2]> {
    fn keep(line: &[[f64; 2]], from: usize, to: usize, tolerance: f64, kept: &mut Vec<bool>) {
        let (Some(&a), Some(&b)) = (line.get(from), line.get(to)) else {
            return;
        };
        let mut worst = (0.0, from);
        for (index, &p) in line.iter().enumerate().take(to).skip(from + 1) {
            let distance = segment_distance(p, a, b);
            if distance > worst.0 {
                worst = (distance, index);
            }
        }
        if worst.0 > tolerance {
            if let Some(flag) = kept.get_mut(worst.1) {
                *flag = true;
            }
            keep(line, from, worst.1, tolerance, kept);
            keep(line, worst.1, to, tolerance, kept);
        }
    }
    if line.len() < 3 {
        return line.to_vec();
    }
    let mut kept = vec![false; line.len()];
    let last = line.len() - 1;
    if let Some(flag) = kept.first_mut() {
        *flag = true;
    }
    if let Some(flag) = kept.last_mut() {
        *flag = true;
    }
    keep(line, 0, last, tolerance, &mut kept);
    line.iter()
        .zip(&kept)
        .filter(|(_, keep)| **keep)
        .map(|(&p, _)| p)
        .collect()
}

/// One grid edge: horizontal from node (i, j) to (i + 1, j), or vertical to (i, j + 1).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
enum Edge {
    H(u32, u32),
    V(u32, u32),
}

/// A cached grid node: not evaluated yet, evaluated off the ring or the ground, or sampled.
#[derive(Clone, Copy, Debug, Default)]
enum Node {
    #[default]
    Unknown,
    Off,
    On(Sample),
}

impl From<Option<Sample>> for Node {
    fn from(sample: Option<Sample>) -> Self {
        sample.map_or(Self::Off, Self::On)
    }
}

/// The fine grid over the region's bounding box, evaluated lazily.
struct Grid<'a> {
    x0: f64,
    y0: f64,
    step: f64,
    nx: u32,
    ny: u32,
    evaluate: &'a (dyn Fn(f64, f64) -> Option<Sample> + Sync),
    nodes: Vec<Node>,
}

/// `f` over `items` on all cores, in input order.
fn parallel<T: Sync, U: Send + Clone + Default>(
    items: &[T],
    f: &(dyn Fn(&T) -> U + Sync),
) -> Vec<U> {
    let threads = std::thread::available_parallelism()
        .map_or(1, std::num::NonZero::get)
        .clamp(1, 32);
    if threads == 1 || items.len() < 512 {
        return items.iter().map(f).collect();
    }
    let mut out = vec![U::default(); items.len()];
    let chunk = items.len().div_ceil(threads);
    std::thread::scope(|scope| {
        for (input, output) in items.chunks(chunk).zip(out.chunks_mut(chunk)) {
            scope.spawn(move || {
                for (item, slot) in input.iter().zip(output.iter_mut()) {
                    *slot = f(item);
                }
            });
        }
    });
    out
}

impl<'a> Grid<'a> {
    fn new(
        ring: &[[f64; 3]],
        step: f64,
        evaluate: &'a (dyn Fn(f64, f64) -> Option<Sample> + Sync),
    ) -> Self {
        let (mut x0, mut y0, mut x1, mut y1) = (
            f64::INFINITY,
            f64::INFINITY,
            f64::NEG_INFINITY,
            f64::NEG_INFINITY,
        );
        for &[x, y, _] in ring {
            (x0, y0, x1, y1) = (x0.min(x), y0.min(y), x1.max(x), y1.max(y));
        }
        // Whole coarse cells, at most 2^13 fine steps a side.
        let count = |span: f64| {
            let fine = plan::clamp_index((span / step).ceil(), 8192).max(1);
            fine.div_ceil(COARSE) * COARSE
        };
        let (nx, ny) = (count(x1 - x0), count(y1 - y0));
        Self {
            x0,
            y0,
            step,
            nx,
            ny,
            evaluate,
            nodes: vec![Node::Unknown; ((nx + 1) * (ny + 1)) as usize],
        }
    }

    fn position(&self, i: u32, j: u32) -> [f64; 2] {
        [
            f64::from(i).mul_add(self.step, self.x0),
            f64::from(j).mul_add(self.step, self.y0),
        ]
    }

    fn index(&self, i: u32, j: u32) -> usize {
        (j * (self.nx + 1) + i) as usize
    }

    /// A node's sample: cached, or evaluated now.
    fn at(&self, i: u32, j: u32) -> Option<Sample> {
        match self.nodes.get(self.index(i, j)) {
            Some(Node::On(sample)) => return Some(*sample),
            Some(Node::Off) => return None,
            _ => {}
        }
        let [x, y] = self.position(i, j);
        (self.evaluate)(x, y)
    }

    /// Evaluate and cache every node of `nodes` not yet cached, on all cores.
    fn prefetch(&mut self, nodes: impl IntoIterator<Item = (u32, u32)>) {
        let missing: Vec<(u32, u32)> = nodes
            .into_iter()
            .filter(|&(i, j)| matches!(self.nodes.get(self.index(i, j)), Some(Node::Unknown)))
            .collect::<BTreeSet<_>>()
            .into_iter()
            .collect();
        let positions: Vec<[f64; 2]> = missing.iter().map(|&(i, j)| self.position(i, j)).collect();
        let evaluate = self.evaluate;
        let samples = parallel(&positions, &|&[x, y]| Node::from(evaluate(x, y)));
        for (&(i, j), sample) in missing.iter().zip(samples) {
            let index = self.index(i, j);
            if let Some(slot) = self.nodes.get_mut(index) {
                *slot = sample;
            }
        }
    }

    /// The fine cells (lower-left node indices) of every coarse cell where some level changes.
    fn active(&mut self, levels: Levels) -> BTreeSet<(u32, u32)> {
        let (cx, cy) = (self.nx / COARSE, self.ny / COARSE);
        self.prefetch((0..=cy).flat_map(|cj| (0..=cx).map(move |ci| (ci * COARSE, cj * COARSE))));
        let mut marked = BTreeSet::new();
        for cj in 0..cy {
            for ci in 0..cx {
                let classes: Vec<Option<usize>> = [(0, 0), (1, 0), (1, 1), (0, 1)]
                    .iter()
                    .map(|&(di, dj)| {
                        let sample = self.at((ci + di) * COARSE, (cj + dj) * COARSE);
                        levels.class(sample)
                    })
                    .collect();
                let known: Vec<usize> = classes.iter().flatten().copied().collect();
                // A change between the corners on the ring: look closer. A contour leaving the
                // cell crosses an edge whose ends differ, so the cell across it is marked too;
                // only a bump narrower than a coarse cell, in and out through one edge, is not.
                if known.windows(2).any(|w| w.first() != w.get(1)) {
                    marked.insert((ci, cj));
                }
            }
        }
        let mut cells = BTreeSet::new();
        for (ci, cj) in marked {
            for j in cj * COARSE..(cj + 1) * COARSE {
                for i in ci * COARSE..(ci + 1) * COARSE {
                    cells.insert((i, j));
                }
            }
        }
        let corners: Vec<(u32, u32)> = cells
            .iter()
            .flat_map(|&(i, j)| [(i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1)])
            .collect();
        self.prefetch(corners);
        cells
    }

    /// Where level `k` crosses `edge`: bisected on the moved predicate for the foot,
    /// interpolated from the lift for an inner level whose ends lie on one side.
    fn crossing(&self, levels: Levels, k: usize, edge: Edge) -> Option<[f64; 2]> {
        let ((ia, ja), (ib, jb)) = match edge {
            Edge::H(i, j) => ((i, j), (i + 1, j)),
            Edge::V(i, j) => ((i, j), (i, j + 1)),
        };
        let (sa, sb) = (self.at(ia, ja)?, self.at(ib, jb)?);
        let (a, b) = (self.position(ia, ja), self.position(ib, jb));
        if k > 0
            && (sa.lift >= 0.0) == (sb.lift >= 0.0)
            && let (Some(va), Some(vb)) = (levels.above(k, sa), levels.above(k, sb))
            && (va > 0.0) != (vb > 0.0)
        {
            let t = (va / (va - vb)).clamp(0.0, 1.0);
            return Some([
                (b[0] - a[0]).mul_add(t, a[0]),
                (b[1] - a[1]).mul_add(t, a[1]),
            ]);
        }
        let at_a = levels.exceeds(k, sa);
        let (mut lo, mut hi) = (a, b);
        for _ in 0..BISECT {
            let mid = [lo[0].midpoint(hi[0]), lo[1].midpoint(hi[1])];
            let inside = (self.evaluate)(mid[0], mid[1]).map(|s| levels.exceeds(k, s));
            if inside == Some(at_a) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        Some([lo[0].midpoint(hi[0]), lo[1].midpoint(hi[1])])
    }

    /// Marching squares for level `k` over `cells`, joined into polylines.
    fn trace(&self, levels: Levels, k: usize, cells: &BTreeSet<(u32, u32)>) -> Vec<Vec<[f64; 2]>> {
        let mut pairs: Vec<[Edge; 2]> = Vec::new();
        for &(i, j) in cells {
            let corners = [
                self.at(i, j),
                self.at(i + 1, j),
                self.at(i + 1, j + 1),
                self.at(i, j + 1),
            ];
            let [Some(sw), Some(se), Some(ne), Some(nw)] =
                corners.map(|s| s.map(|s| levels.exceeds(k, s)))
            else {
                continue;
            };
            // Corners counter-clockwise from (i, j); edges bottom, right, top, left.
            let sides = [
                (Edge::H(i, j), sw != se),
                (Edge::V(i + 1, j), se != ne),
                (Edge::H(i, j + 1), ne != nw),
                (Edge::V(i, j), nw != sw),
            ];
            let cut: Vec<Edge> = sides.iter().filter(|(_, c)| *c).map(|(e, _)| *e).collect();
            match cut.as_slice() {
                [p, q] => pairs.push([*p, *q]),
                [bottom, right, top, left] => {
                    // A saddle: the centre decides which corners connect.
                    let [cx, cy] = self.position(i, j);
                    let centre = (self.evaluate)(cx + self.step / 2.0, cy + self.step / 2.0)
                        .map(|s| levels.exceeds(k, s));
                    if centre == Some(sw) {
                        pairs.extend([[*bottom, *right], [*top, *left]]);
                    } else {
                        pairs.extend([[*left, *bottom], [*right, *top]]);
                    }
                }
                _ => {}
            }
        }
        // Every crossing once, on all cores.
        let edges: Vec<Edge> = pairs
            .iter()
            .flatten()
            .copied()
            .collect::<BTreeSet<_>>()
            .into_iter()
            .collect();
        let found = parallel(&edges, &|&edge| self.crossing(levels, k, edge));
        let points: BTreeMap<Edge, [f64; 2]> = edges
            .iter()
            .zip(found)
            .filter_map(|(&edge, point)| point.map(|p| (edge, p)))
            .collect();
        let mut links: BTreeMap<Edge, Vec<Edge>> = BTreeMap::new();
        for [p, q] in pairs {
            if points.contains_key(&p) && points.contains_key(&q) {
                links.entry(p).or_default().push(q);
                links.entry(q).or_default().push(p);
            }
        }
        chains(&points, &links)
    }
}

/// Polylines from the crossing graph: ends (one link) first, then the remaining loops.
fn chains(
    points: &BTreeMap<Edge, [f64; 2]>,
    links: &BTreeMap<Edge, Vec<Edge>>,
) -> Vec<Vec<[f64; 2]>> {
    let mut used: BTreeSet<Edge> = BTreeSet::new();
    let mut out = Vec::new();
    let starts: Vec<Edge> = links
        .iter()
        .filter(|(_, n)| n.len() == 1)
        .map(|(&e, _)| e)
        .chain(links.keys().copied())
        .collect();
    for start in starts {
        if !used.insert(start) {
            continue;
        }
        let mut chain = vec![start];
        let mut current = start;
        while let Some(next) = links
            .get(&current)
            .and_then(|n| n.iter().find(|e| !used.contains(e)).copied())
        {
            used.insert(next);
            chain.push(next);
            current = next;
        }
        // A closed loop: back to its start.
        if chain.len() > 2 && links.get(&current).is_some_and(|n| n.contains(&start)) {
            chain.push(start);
        }
        let line: Vec<[f64; 2]> = chain
            .iter()
            .filter_map(|e| points.get(e).copied())
            .collect();
        if line.len() >= 2 {
            out.push(line);
        }
    }
    out
}

fn segment_distance(p: [f64; 2], a: [f64; 2], b: [f64; 2]) -> f64 {
    let (dx, dy) = (b[0] - a[0], b[1] - a[1]);
    let l2 = dx.mul_add(dx, dy * dy);
    let t = if l2 > 0.0 {
        ((p[0] - a[0]).mul_add(dx, (p[1] - a[1]) * dy) / l2).clamp(0.0, 1.0)
    } else {
        0.0
    };
    (p[0] - dx.mul_add(t, a[0])).hypot(p[1] - dy.mul_add(t, a[1]))
}

/// Existing segments and vertices in a uniform plan grid.
struct Clearance {
    cell: f64,
    clear: f64,
    line_clear: f64,
    segments: Vec<[[f64; 2]; 2]>,
    vertices: Vec<[f64; 2]>,
    bins: BTreeMap<(i64, i64), (Vec<u32>, Vec<u32>)>,
}

impl Clearance {
    fn new(
        obstacles: &[&[f64]],
        spots: &[f64],
        ring: &[[f64; 3]],
        clear: f64,
        line_clear: f64,
    ) -> Self {
        let mut this = Self {
            cell: 1.0,
            clear,
            line_clear,
            segments: Vec::new(),
            vertices: Vec::new(),
            bins: BTreeMap::new(),
        };
        for line in obstacles {
            let points = plan::vertices(line);
            for pair in points.windows(2) {
                if let [a, b] = pair {
                    this.segment([a[0], a[1]], [b[0], b[1]]);
                }
            }
            for p in points {
                this.vertex([p[0], p[1]]);
            }
        }
        for (k, a) in ring.iter().enumerate() {
            let b = ring.get((k + 1) % ring.len()).copied().unwrap_or(*a);
            this.segment([a[0], a[1]], [b[0], b[1]]);
            this.vertex([a[0], a[1]]);
        }
        for p in plan::vertices(spots) {
            this.vertex([p[0], p[1]]);
        }
        this
    }

    #[expect(
        clippy::cast_possible_truncation,
        reason = "plan metres over a 1 m grid: far inside i64, and a saturated key only widens a bin"
    )]
    fn key(&self, x: f64, y: f64) -> (i64, i64) {
        (
            (x / self.cell).floor() as i64,
            (y / self.cell).floor() as i64,
        )
    }

    fn cells(&self, lo: [f64; 2], hi: [f64; 2]) -> impl Iterator<Item = (i64, i64)> + use<> {
        let reach = self.clear.max(self.line_clear);
        let (a, b) = (
            self.key(lo[0] - reach, lo[1] - reach),
            self.key(hi[0] + reach, hi[1] + reach),
        );
        (a.0..=b.0).flat_map(move |i| (a.1..=b.1).map(move |j| (i, j)))
    }

    fn segment(&mut self, a: [f64; 2], b: [f64; 2]) {
        let index = u32::try_from(self.segments.len()).unwrap_or(u32::MAX);
        self.segments.push([a, b]);
        let cells: Vec<_> = self
            .cells(
                [a[0].min(b[0]), a[1].min(b[1])],
                [a[0].max(b[0]), a[1].max(b[1])],
            )
            .collect();
        for cell in cells {
            self.bins.entry(cell).or_default().0.push(index);
        }
    }

    fn vertex(&mut self, p: [f64; 2]) {
        let index = u32::try_from(self.vertices.len()).unwrap_or(u32::MAX);
        self.vertices.push(p);
        let cells: Vec<_> = self.cells(p, p).collect();
        for cell in cells {
            self.bins.entry(cell).or_default().1.push(index);
        }
    }

    /// Whether `p` is closer than the clearance to an existing segment or vertex.
    fn crowds_point(&self, p: [f64; 2]) -> bool {
        let Some((segments, vertices)) = self.bins.get(&self.key(p[0], p[1])) else {
            return false;
        };
        segments.iter().any(|&s| {
            self.segments
                .get(s as usize)
                .is_some_and(|&[a, b]| segment_distance(p, a, b) < self.line_clear)
        }) || vertices.iter().any(|&v| {
            self.vertices
                .get(v as usize)
                .is_some_and(|q| (q[0] - p[0]).hypot(q[1] - p[1]) < self.clear)
        })
    }

    /// Whether the segment `a`-`b` passes closer than the clearance to an existing vertex.
    fn crowds_segment(&self, a: [f64; 2], b: [f64; 2]) -> bool {
        let lo = [a[0].min(b[0]), a[1].min(b[1])];
        let hi = [a[0].max(b[0]), a[1].max(b[1])];
        let (k0, k1) = (self.key(lo[0], lo[1]), self.key(hi[0], hi[1]));
        for i in k0.0..=k1.0 {
            for j in k0.1..=k1.1 {
                let Some((_, vertices)) = self.bins.get(&(i, j)) else {
                    continue;
                };
                if vertices.iter().any(|&v| {
                    self.vertices
                        .get(v as usize)
                        .is_some_and(|&q| segment_distance(q, a, b) < self.clear)
                }) {
                    return true;
                }
            }
        }
        false
    }

    /// `line` cut into the pieces that keep clear of everything existing.
    fn split(&self, line: &[[f64; 2]]) -> Vec<Vec<[f64; 2]>> {
        let mut pieces = Vec::new();
        let mut piece: Vec<[f64; 2]> = Vec::new();
        for &p in line {
            if self.crowds_point(p) {
                pieces.push(std::mem::take(&mut piece));
                continue;
            }
            if let Some(&last) = piece.last() {
                if (last[0] - p[0]).hypot(last[1] - p[1]) < self.clear {
                    continue; // a duplicate (a closed loop's end) or a sliver
                }
                if self.crowds_segment(last, p) {
                    pieces.push(std::mem::replace(&mut piece, vec![p]));
                    continue;
                }
            }
            piece.push(p);
        }
        pieces.push(piece);
        pieces.retain(|piece| piece.len() >= 2);
        pieces
    }
}
