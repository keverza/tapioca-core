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
//! piece left after the clearance is then simplified to the tolerance.
//!
//! **Clearance** is [`super::trace`]'s: a traced point too close to an existing
//! vertex or line is dropped and the line split there.
//!
//! Plan metres; the lines' z is zero (a breakline's height is the design's).

use std::collections::{BTreeMap, BTreeSet};

use super::Faces;
use super::trace::{Clearance, Edge, Grid, Node, chains, parallel, push_piece, simplify};
use crate::terrain::plan;

/// How the toe (and the creases) are traced.
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
    /// Whether the faces' creases (ridges and valleys, [`super::crease`]) are traced too,
    /// after the toe and clear of it.
    pub creases: bool,
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
        creases: true,
    };
}

/// Where a rounded toe's lift crosses these fractions of its rounding height:
/// the foot, then a quadratic toe's quarter points in plan, then its top.
const LEVELS: [f64; 5] = [0.0, 1.0 / 16.0, 0.25, 9.0 / 16.0, 1.0];

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
    let cells = grid.active(&|corners| {
        let known: Vec<usize> = corners.iter().filter_map(|&s| levels.class(s)).collect();
        known.windows(2).any(|w| w.first() != w.get(1))
    });
    let mut out: Vec<Vec<f64>> = Vec::new();
    for k in 0..levels.count() {
        let traced = trace(&grid, levels, k, &cells);
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
    if toe.creases {
        let creases = {
            let mut all: Vec<&[f64]> = obstacles.to_vec();
            all.extend(out.iter().map(Vec::as_slice));
            // The toe's nodes are the creases' nodes: where it found the ground kept (most of a
            // parcel), no face wins and the creases need not ask.
            let kept = |x: f64, y: f64| match grid.cached(x, y) {
                Node::On(sample) => Some(!sample.moved),
                Node::Off => Some(true),
                Node::Unknown => None,
            };
            super::crease::lines(faces, ground, region, &all, spots, &kept, toe)
        };
        out.extend(creases);
    }
    out
}

/// Where level `k` crosses `edge`: bisected on the moved predicate for the foot,
/// interpolated from the lift for an inner level whose ends lie on one side.
fn crossing(grid: &Grid<'_, Sample>, levels: Levels, k: usize, edge: Edge) -> Option<[f64; 2]> {
    let ((ia, ja), (ib, jb)) = edge.ends();
    let (sa, sb) = (grid.at(ia, ja)?, grid.at(ib, jb)?);
    let (a, b) = (grid.position(ia, ja), grid.position(ib, jb));
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
    let [inner, outer] = grid.bracket(edge, &|_, mid| {
        mid.map(|s| levels.exceeds(k, s)) == Some(at_a)
    })?;
    if k == 0 {
        // The foot's vertex is the bracket's end where the design keeps the ground: its height
        // is the ground's bit for bit, so a triangle from it out to the remote ground stays the
        // ground (#65). The midpoint lifted it a few hundredths of a millimetre at the grade.
        return Some(if at_a { outer } else { inner });
    }
    Some([inner[0].midpoint(outer[0]), inner[1].midpoint(outer[1])])
}

/// Marching squares for level `k` over `cells`, joined into polylines.
fn trace(
    grid: &Grid<'_, Sample>,
    levels: Levels,
    k: usize,
    cells: &BTreeSet<(u32, u32)>,
) -> Vec<Vec<[f64; 2]>> {
    let mut pairs: Vec<[Edge; 2]> = Vec::new();
    for &(i, j) in cells {
        let corners = [
            grid.at(i, j),
            grid.at(i + 1, j),
            grid.at(i + 1, j + 1),
            grid.at(i, j + 1),
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
                let [cx, cy] = grid.position(i, j);
                let centre = (grid.evaluate)(cx + grid.step / 2.0, cy + grid.step / 2.0)
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
    let found = parallel(&edges, &|&edge| crossing(grid, levels, k, edge));
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
