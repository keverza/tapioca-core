//! #149 T2: what tracing the faces' breaklines shares. A lazily evaluated plan
//! grid whose fine cells are visited only inside the coarse cells where a sample
//! class changes, the crossing graph joined into polylines, the clearance that
//! keeps a traced line off existing vertices and lines, and the simplification
//! and loop splitting before a line goes to the rebuild.
//!
//! **Clearance.** A breakline that grazes an existing vertex or runs along an
//! existing line makes the triangulator settle a crossing onto that vertex in
//! f64, and the rebuild refuses (the T3 attempt's failure). Every traced vertex
//! closer than the clearance to an existing line, spot or the region ring is
//! dropped, and so is every traced segment that passes that close to an existing
//! vertex; the polyline is split there. A crossing at an angle, away from
//! existing vertices, is kept: the rebuild inserts it.
//!
//! Plan metres; the lines' z is zero (a breakline's height is the design's).

use std::collections::{BTreeMap, BTreeSet};

use crate::terrain::plan;

/// Fine steps per coarse step.
pub(super) const COARSE: u32 = 3;
/// Bisection steps on a cell edge: a quarter metre over 2^10, a quarter millimetre.
pub(super) const BISECT: u32 = 10;
/// Shortest traced piece handed to the rebuild, metres.
const SHORTEST: f64 = 0.1;

/// A clear piece as one or (a closed loop) two open xyz breaklines.
pub(super) fn push_piece(piece: &[[f64; 2]], clear: f64, out: &mut Vec<Vec<f64>>) {
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
pub(super) fn simplify(line: &[[f64; 2]], tolerance: f64) -> Vec<[f64; 2]> {
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
pub(super) enum Edge {
    H(u32, u32),
    V(u32, u32),
}

impl Edge {
    /// The edge's two nodes.
    pub(super) const fn ends(self) -> ((u32, u32), (u32, u32)) {
        match self {
            Self::H(i, j) => ((i, j), (i + 1, j)),
            Self::V(i, j) => ((i, j), (i, j + 1)),
        }
    }
}

/// A cached grid node: not evaluated yet, evaluated off the ring or the ground, or sampled.
#[derive(Clone, Copy, Debug, Default)]
pub(super) enum Node<T> {
    #[default]
    Unknown,
    Off,
    On(T),
}

impl<T> From<Option<T>> for Node<T> {
    fn from(sample: Option<T>) -> Self {
        sample.map_or(Self::Off, Self::On)
    }
}

/// The fine grid over the region's bounding box, evaluated lazily.
pub(super) struct Grid<'a, T> {
    x0: f64,
    y0: f64,
    pub(super) step: f64,
    nx: u32,
    ny: u32,
    pub(super) evaluate: &'a (dyn Fn(f64, f64) -> Option<T> + Sync),
    nodes: Vec<Node<T>>,
}

/// `f` over `items` on all cores, in input order.
pub(super) fn parallel<T: Sync, U: Send + Clone + Default>(
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

impl<'a, T: Copy + Send + Sync> Grid<'a, T> {
    pub(super) fn new(
        ring: &[[f64; 3]],
        step: f64,
        evaluate: &'a (dyn Fn(f64, f64) -> Option<T> + Sync),
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

    pub(super) fn position(&self, i: u32, j: u32) -> [f64; 2] {
        [
            f64::from(i).mul_add(self.step, self.x0),
            f64::from(j).mul_add(self.step, self.y0),
        ]
    }

    fn index(&self, i: u32, j: u32) -> usize {
        (j * (self.nx + 1) + i) as usize
    }

    /// The cached node at plan point `(x, y)`; [`Node::Unknown`] unless it is exactly a node
    /// of this grid and was evaluated. A grid over the same ring and step
    /// has the same nodes, so a second tracer reads the first's samples here.
    #[expect(
        clippy::cast_possible_truncation,
        clippy::cast_sign_loss,
        reason = "rounded, checked non-negative and within the grid, then confirmed bit for bit"
    )]
    pub(super) fn cached(&self, x: f64, y: f64) -> Node<T> {
        let (fi, fj) = (
            ((x - self.x0) / self.step).round(),
            ((y - self.y0) / self.step).round(),
        );
        if !(fi >= 0.0 && fj >= 0.0 && fi <= f64::from(self.nx) && fj <= f64::from(self.ny)) {
            return Node::Unknown;
        }
        let (i, j) = (fi as u32, fj as u32);
        let [px, py] = self.position(i, j);
        if px.to_bits() != x.to_bits() || py.to_bits() != y.to_bits() {
            return Node::Unknown;
        }
        self.nodes
            .get(self.index(i, j))
            .copied()
            .unwrap_or_default()
    }

    /// A node's sample: cached, or evaluated now.
    pub(super) fn at(&self, i: u32, j: u32) -> Option<T> {
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

    /// The fine cells (lower-left node indices) of every coarse cell whose corners
    /// `changes`, all their nodes evaluated. A line leaving a cell crosses an edge whose
    /// ends differ, so the cell across it is marked too; only a feature narrower than a
    /// coarse cell, in and out through one edge, is missed.
    pub(super) fn active(
        &mut self,
        changes: &dyn Fn([Option<T>; 4]) -> bool,
    ) -> BTreeSet<(u32, u32)> {
        let (cx, cy) = (self.nx / COARSE, self.ny / COARSE);
        self.prefetch((0..=cy).flat_map(|cj| (0..=cx).map(move |ci| (ci * COARSE, cj * COARSE))));
        let mut marked = BTreeSet::new();
        for cj in 0..cy {
            for ci in 0..cx {
                let corners = [(0, 0), (1, 0), (1, 1), (0, 1)]
                    .map(|(di, dj)| self.at((ci + di) * COARSE, (cj + dj) * COARSE));
                if changes(corners) {
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

    /// The point between `edge`'s ends where `same` stops holding of the sample at the first
    /// end, bisected; `None` off the grid's samples.
    pub(super) fn bisect(
        &self,
        edge: Edge,
        same: &dyn Fn(T, Option<T>) -> bool,
    ) -> Option<[f64; 2]> {
        let ((ia, ja), (ib, jb)) = edge.ends();
        let start = self.at(ia, ja)?;
        let (mut lo, mut hi) = (self.position(ia, ja), self.position(ib, jb));
        for _ in 0..BISECT {
            let mid = [lo[0].midpoint(hi[0]), lo[1].midpoint(hi[1])];
            if same(start, (self.evaluate)(mid[0], mid[1])) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        Some([lo[0].midpoint(hi[0]), lo[1].midpoint(hi[1])])
    }
}

/// Polylines from a crossing graph: ends (one link) first, then the remaining loops.
pub(super) fn chains<K: Copy + Ord>(
    points: &BTreeMap<K, [f64; 2]>,
    links: &BTreeMap<K, Vec<K>>,
) -> Vec<Vec<[f64; 2]>> {
    let mut used: BTreeSet<K> = BTreeSet::new();
    let mut out = Vec::new();
    let starts: Vec<K> = links
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

pub(super) fn segment_distance(p: [f64; 2], a: [f64; 2], b: [f64; 2]) -> f64 {
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
pub(super) struct Clearance {
    cell: f64,
    clear: f64,
    line_clear: f64,
    segments: Vec<[[f64; 2]; 2]>,
    vertices: Vec<[f64; 2]>,
    bins: BTreeMap<(i64, i64), (Vec<u32>, Vec<u32>)>,
}

impl Clearance {
    pub(super) fn new(
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
            this.add(line);
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

    /// Index one more open xyz polyline, its segments and vertices.
    pub(super) fn add(&mut self, line: &[f64]) {
        let points = plan::vertices(line);
        for pair in points.windows(2) {
            if let [a, b] = pair {
                self.segment([a[0], a[1]], [b[0], b[1]]);
            }
        }
        for p in points {
            self.vertex([p[0], p[1]]);
        }
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
    pub(super) fn split(&self, line: &[[f64; 2]]) -> Vec<Vec<[f64; 2]>> {
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
