//! #149 T3: the graded region meshed on one constrained Delaunay triangulation.
//!
//! The region's rebuild ([`crate::terrain::region`]) gives the inside's vertices
//! and its constraints: the ring (the stitch to the untouched outside), the
//! lines (the caller's, the toe and creases, the supports) and the base edges
//! between different base triangles (which keep the ground affine under every
//! output triangle, so the volumes stay exact). They go into one `spade`
//! triangulation, and round after round every inside triangle still at least
//! [`super::refine::Refine::shortest`] across that
//!
//! - misses the faces by more than the tolerance at a sample (and by no more
//!   than a continuous field could: a larger gap is a step, reported and not
//!   refined),
//! - straddles the toe wider than its band, or
//! - has an angle under the limit
//!
//! gets a vertex at its circumcentre, Ruppert's way. Where that point would
//! cross a constrained side or fall in its diametral circle, the side is split
//! at its midpoint instead: the constraint is lifted, the midpoint inserted and
//! the two halves constrained again, so no point is ever left beside a
//! constraint in f64 (the bisection lane's failure). The ring is never split; a
//! triangle whose point would encroach it takes its centroid when its error is
//! the reason, and is left when only its angle is. A triangle touching a ring
//! vertex where the faces end in a step is refined whatever its step, down to
//! the shortest side, so the step narrows to a strip along the ring. A thin
//! triangle whose smallest angle lies between two constrained sides, and one of
//! collinear corners, are left.
//!
//! Nothing is rebuilt between rounds: the triangulation is kept and only the
//! new vertices' heights are evaluated. The result is assembled straight into a
//! region mesh: the outside triangles as they were, the inside from the
//! triangulation, each added vertex a [`RegionVertex::Refined`], and every line
//! edge still a path along its segment.

use std::collections::{BTreeMap, BTreeSet, HashMap, VecDeque};

use spade::handles::FixedVertexHandle;
use spade::{
    ConstrainedDelaunayTriangulation, Intersection, LineIntersectionIterator, Point2,
    PositionInTriangulation, Triangulation,
};

use super::plan;
use super::refine::{Bound, Memo, Refined};
use super::{StepWitness, push_step_witness};
use crate::terrain::field::Field;
use crate::terrain::region::{RegionLineEdge, RegionMesh, RegionVertex};

/// What the mesher meshes for: the design and the ground it is graded over, at a plan point;
/// the base surface the region mesh's points lie on; and the grading's at-grade snap. The
/// outline lane grades over the base surface; the unified build (#149 T1) over the ground its
/// region entries composed first.
pub(in crate::terrain) struct Lane<'a> {
    /// The design height at a plan point and whether a face ran out at its limit there
    /// ([`super::Faces::height`]); `None` off the base surface.
    pub design: &'a (dyn Fn(f64, f64) -> Option<(f64, bool)> + 'a),
    /// The ground the design is graded over.
    pub ground: &'a (dyn Fn(f64, f64) -> Option<f64> + 'a),
    /// The base surface: the region mesh's own heights, and its base edges.
    pub base: &'a Field,
    /// Within this of the ground the design is the ground, metres.
    pub snap: f64,
    /// Where the faces can move the ground at all: a triangle elsewhere is not evaluated.
    pub near: &'a super::Near,
    /// The region mesh's lines `traced[0]..traced[1]` are the faces' own traced lines (#149 T2:
    /// the toe and creases). A vertex on one is at the toe, not beyond it. The lines from
    /// `traced[1]` on are the pavement's private supports (its TIN edges): one beside a kerb's
    /// face is not split -- a kerb is two of them a few centimetres apart in plan, and a split
    /// of one leaves slivers tilting across the kerb.
    pub traced: [u32; 2],
    /// Whether a plan point is on the pavement (its footprint, a kerb's face included): its own
    /// TIN, which the rebuild carries exactly (its vertices as spots, its edges as supports), so
    /// a triangle there is not refined.
    pub paved: &'a (dyn Fn(f64, f64) -> bool + 'a),
    /// Whether a located diagnostic may be retained. Applied before the witness cap, never to
    /// classification or refinement itself.
    pub allowed: &'a (dyn Fn(f64, f64) -> bool + 'a),
}

type Cdt = ConstrainedDelaunayTriangulation<Point2<f64>>;

/// How many of a triangle's lengths its new point may lie from it.
const REACH: f64 = 4.0;

/// Most edges a walk from a triangle to its circumcentre may cross.
const WALK: u32 = 4096;

/// Rounds before the mesher reports itself exhausted.
const MAX_ROUNDS: u32 = 64;

/// What a constrained side is.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
enum Kind {
    Ring,
    Line { line: u32, segment: u32 },
    Base,
}

/// Why a triangle is refined: its actionable error, a demonstrated boundary-step error, or only
/// its angle. Boundary steps remain error work but are not actionable residual.
#[derive(Clone, Copy, Debug, PartialEq)]
enum Why {
    Error(f64),
    Step(f64),
    Angle,
}

/// A triangle the mesher could not or must not refine, and one whose fix broke the copy.
enum Stop {
    Leave,
    Broken,
}

fn key(a: usize, b: usize) -> [usize; 2] {
    if a < b { [a, b] } else { [b, a] }
}

fn distance(p: Point2<f64>, q: Point2<f64>) -> f64 {
    (q.x - p.x).hypot(q.y - p.y)
}

/// Plan distance from `p` to the finite segment `a`-`b`, metres.
fn segment_distance(p: Point2<f64>, a: Point2<f64>, b: Point2<f64>) -> f64 {
    let (dx, dy) = (b.x - a.x, b.y - a.y);
    let squared = dx.mul_add(dx, dy * dy);
    if squared <= 0.0 {
        return distance(p, a);
    }
    let t = ((p.x - a.x).mul_add(dx, (p.y - a.y) * dy) / squared).clamp(0.0, 1.0);
    distance(p, Point2::new(dx.mul_add(t, a.x), dy.mul_add(t, a.y)))
}

/// The smallest angle, radians, of the triangle `p`, `q`, `r`.
fn smallest_angle(first: Point2<f64>, second: Point2<f64>, third: Point2<f64>) -> f64 {
    let angle = |apex: Point2<f64>, left: Point2<f64>, right: Point2<f64>| {
        let along = [left.x - apex.x, left.y - apex.y];
        let across = [right.x - apex.x, right.y - apex.y];
        along[0]
            .mul_add(across[1], -along[1] * across[0])
            .abs()
            .atan2(along[0].mul_add(across[0], along[1] * across[1]))
    };
    angle(first, second, third)
        .min(angle(second, third, first))
        .min(angle(third, first, second))
}

struct Mesher<'a, 'f> {
    lane: &'a Lane<'f>,
    memo: &'a mut Memo,
    bound: Bound,
    cdt: Cdt,
    /// Per `spade` vertex: its region mesh vertex, base height and whether it is on the ring.
    vertex: Vec<u32>,
    base: Vec<f64>,
    ring: Vec<bool>,
    /// Region mesh vertices before the mesher's, and `spade` vertices before the mesher's (the
    /// inside's): an added `spade` vertex `k` is region vertex `original + k - inside`.
    original: u32,
    inside: usize,
    kinds: BTreeMap<[usize; 2], Vec<Kind>>,
    ring_plan: Vec<[f64; 3]>,
    /// The ring's sides, which are never split. Ordered so a located witness never depends on hash
    /// iteration when more than one local segment could demonstrate the mismatch.
    ring_sides: BTreeSet<[usize; 2]>,
    /// Bounded located evidence for triangles classified as boundary-step work.
    steps: Vec<StepWitness>,
    /// Each triangle's assessment, by its sorted vertices: a triangle that survives a round
    /// is not assessed again (its corners, their heights and their lines are unchanged).
    assessed: HashMap<[usize; 3], (Option<Why>, bool)>,
}

impl Mesher<'_, '_> {
    fn position(&self, v: usize) -> Option<Point2<f64>> {
        self.cdt
            .get_vertex(FixedVertexHandle::from_index(v))
            .map(|vertex| vertex.position())
    }

    /// The design height at a vertex: the ground on the ring, the faces elsewhere.
    fn height(&mut self, v: usize) -> Option<f64> {
        let base = *self.base.get(v)?;
        if self.ring.get(v).copied().unwrap_or(false) {
            return Some(base);
        }
        let p = self.position(v)?;
        Some(
            self.memo
                .height(self.lane.design, p.x, p.y)
                .map_or(base, |(height, _)| height),
        )
    }

    /// At the triangle's inside samples, whether the design keeps the ground at some and moves
    /// it at others: `[kept, moved]`. The toe crosses a triangle between its samples where both
    /// are seen. A corner on a traced toe line is neither side, so corners are not asked; the
    /// traced line strays up to its 5 mm tolerance from the exact toe, so one sample keeping
    /// the ground beside it is not a crossing either.
    fn moves(&mut self, [pa, pb, pc]: [Point2<f64>; 3]) -> [bool; 2] {
        let mut seen = [false, false];
        for [wa, wb, wc] in super::refine::SAMPLES {
            let (x, y) = (
                wa.mul_add(pa.x, wb.mul_add(pb.x, wc * pc.x)),
                wa.mul_add(pa.y, wb.mul_add(pb.y, wc * pc.y)),
            );
            let (Some(ground), Some((height, _))) = (
                (self.lane.ground)(x, y),
                self.memo.height(self.lane.design, x, y),
            ) else {
                continue;
            };
            let moved = height.to_bits() != ground.to_bits();
            if let Some(flag) = seen.get_mut(usize::from(moved)) {
                *flag = true;
            }
        }
        seen
    }

    /// Whether the design moves the ground at vertex `v`.
    fn moved_at(&mut self, v: usize) -> bool {
        let (Some(p), Some(height)) = (self.position(v), self.height(v)) else {
            return false;
        };
        (self.lane.ground)(p.x, p.y).is_some_and(|ground| ground.to_bits() != height.to_bits())
    }

    /// Whether the triangle's smallest angle lies between two constrained sides.
    fn input_angle(&self, [a, b, c]: [usize; 3], [pa, pb, pc]: [Point2<f64>; 3]) -> bool {
        let angle = |apex: Point2<f64>, left: Point2<f64>, right: Point2<f64>| {
            let along = [left.x - apex.x, left.y - apex.y];
            let across = [right.x - apex.x, right.y - apex.y];
            along[0]
                .mul_add(across[1], -along[1] * across[0])
                .abs()
                .atan2(along[0].mul_add(across[0], along[1] * across[1]))
        };
        let corners = [
            (angle(pa, pb, pc), a, b, c),
            (angle(pb, pc, pa), b, c, a),
            (angle(pc, pa, pb), c, a, b),
        ];
        corners
            .iter()
            .min_by(|x, y| x.0.total_cmp(&y.0))
            .is_some_and(|&(_, apex, left, right)| {
                self.is_constraint(apex, left) && self.is_constraint(apex, right)
            })
    }

    /// Whether vertex `v` lies on one of the faces' traced lines.
    fn on_traced(&self, v: usize) -> bool {
        let [first, last] = self.lane.traced;
        self.cdt
            .get_vertex(FixedVertexHandle::from_index(v))
            .is_some_and(|vertex| {
                vertex.out_edges().any(|edge| {
                    let other = edge.to().fix().index();
                    self.kinds.get(&key(v, other)).is_some_and(|kinds| {
                        kinds.iter().any(|kind| {
                            matches!(kind, Kind::Line { line, .. } if (first..last).contains(line))
                        })
                    })
                })
            })
    }

    /// A ring vertex where the faces are off the ground: the faces end there in a step.
    fn clipped(&mut self, v: usize) -> bool {
        if !self.ring.get(v).copied().unwrap_or(false) {
            return false;
        }
        let (Some(p), Some(&base)) = (self.position(v), self.base.get(v)) else {
            return false;
        };
        self.memo
            .height(self.lane.design, p.x, p.y)
            .is_some_and(|(height, _)| (height - base).abs() > self.bound.refine.tolerance)
    }

    /// The located mismatch when this above-tolerance triangle is in a bounded strip beside the
    /// stitch. The ring is never split, so such a step cannot be interpolated away. Every corner
    /// must be within the toe band of the same finite ring segment; merely being near the ring,
    /// or having a large ordinary error there without a demonstrated mismatch, is not enough.
    fn boundary_step(&mut self, positions: [Point2<f64>; 3], error: f64) -> Option<StepWitness> {
        let tolerance = self.bound.refine.tolerance;
        if error <= tolerance {
            return None;
        }
        // Borrow the ring only while collecting the few geometrically qualifying segments; the
        // mismatch probes then mutate the height memo without copying the whole ring per triangle.
        let candidates: Vec<_> = self
            .ring_sides
            .iter()
            .filter_map(|&[a, b]| {
                let (Some(ra), Some(rb), Some(&za), Some(&zb)) = (
                    self.position(a),
                    self.position(b),
                    self.base.get(a),
                    self.base.get(b),
                ) else {
                    return None;
                };
                let (dx, dy) = (rb.x - ra.x, rb.y - ra.y);
                let squared = dx.mul_add(dx, dy * dy);
                if squared <= 0.0
                    || !positions
                        .iter()
                        .all(|&p| segment_distance(p, ra, rb) <= self.bound.refine.toe)
                {
                    return None;
                }
                Some((ra, rb, za, zb, squared))
            })
            .collect();
        for (ra, rb, za, zb, squared) in candidates {
            let (dx, dy) = (rb.x - ra.x, rb.y - ra.y);
            let projections = positions
                .map(|p| ((p.x - ra.x).mul_add(dx, (p.y - ra.y) * dy) / squared).clamp(0.0, 1.0));
            let lo = projections.iter().copied().fold(1.0_f64, f64::min);
            let hi = projections.iter().copied().fold(0.0_f64, f64::max);
            // Witnesses belong to this triangle's projected interval, not a remote part of a long
            // ring segment. Endpoints include every corner projection; the interior samples cover
            // a mismatch between them without making that claim exhaustive.
            for t in [
                lo,
                lo + (hi - lo) / 3.0,
                lo.midpoint(hi),
                lo + 2.0 * (hi - lo) / 3.0,
                hi,
            ] {
                let x = (1.0 - t).mul_add(ra.x, t * rb.x);
                let y = (1.0 - t).mul_add(ra.y, t * rb.y);
                let retained = (1.0 - t).mul_add(za, t * zb);
                if let Some((design, _)) = self.memo.height(self.lane.design, x, y)
                    && (design - retained).abs() > tolerance
                {
                    return Some(StepWitness::StepStrip {
                        position: [x, y, retained],
                        design,
                        residual: error,
                    });
                }
            }
        }
        None
    }

    /// Why the triangle `[a, b, c]` needs a vertex (`None` when it does not or must not get
    /// one; [`Why::Angle`] for any thin triangle, the caller keeps it near the design), and
    /// whether the design moves the ground at one of its samples.
    fn assess(&mut self, triangle: [usize; 3]) -> (Option<Why>, bool) {
        let mut sorted = triangle;
        sorted.sort_unstable();
        if let Some(&known) = self.assessed.get(&sorted) {
            return known;
        }
        let found = self.judge(triangle).unwrap_or((None, false));
        self.assessed.insert(sorted, found);
        found
    }

    fn judge(&mut self, [a, b, c]: [usize; 3]) -> Option<(Option<Why>, bool)> {
        let refine = self.bound.refine;
        let (pa, pb, pc) = (self.position(a)?, self.position(b)?, self.position(c)?);
        let longest = distance(pa, pb).max(distance(pb, pc)).max(distance(pc, pa));
        if longest < refine.shortest {
            return None;
        }
        if (self.lane.paved)((pa.x + pb.x + pc.x) / 3.0, (pa.y + pb.y + pc.y) / 3.0) {
            return None;
        }
        let lo = [pa.x.min(pb.x).min(pc.x), pa.y.min(pb.y).min(pc.y)];
        let hi = [pa.x.max(pb.x).max(pc.x), pa.y.max(pb.y).max(pc.y)];
        if !self.lane.near.touches(lo, hi) {
            return None;
        }
        // At a ring vertex where the faces are off the ground they end in a step (the ring keeps
        // the ground for the stitch). Such a triangle is refined whatever its step, down to the
        // shortest side: the step narrows to a strip along the ring instead of tilting a large
        // triangle across the faces (#151's slab edge, which the offset rings used to densify).
        let clipped = [a, b, c].iter().any(|&v| self.clipped(v));
        let [kept, moved] = self.moves([pa, pb, pc]);
        let corner_moved = [a, b, c].map(|v| (self.moved_at(v), v));
        // A thin triangle whose smallest angle lies between two constrained sides is an input
        // angle (two lines meeting): no vertex mends it, and Ruppert's cascade around it does not
        // end. It is left.
        // A thin triangle under twice the shortest side is left too: Ruppert's minimum size, so
        // the angle refinement ends instead of running to the round cap. A long sliver with one
        // short side is refined.
        let thin = smallest_angle(pa, pb, pc) < refine.angle.to_radians()
            && longest >= 2.0 * refine.shortest
            && !self.input_angle([a, b, c], [pa, pb, pc]);
        if !moved && !corner_moved.iter().any(|&(m, _)| m) {
            // The design keeps the ground here: the mesh is the ground's own (the base's, or the
            // surface the unified build's region entries compose at its vertices), not the
            // faces' to refine. Only its angle is asked, beside a triangle the design moves.
            return Some((thin.then_some(Why::Angle), false));
        }
        let corners = [
            [pa.x, pa.y, self.height(a)?],
            [pb.x, pb.y, self.height(b)?],
            [pc.x, pc.y, self.height(c)?],
        ];
        let error = self.memo.error(self.lane.design, corners);
        if error > refine.tolerance
            && let Some(witness) = self.boundary_step([pa, pb, pc], error)
        {
            let position = witness.position();
            if (self.lane.allowed)(position[0], position[1]) {
                push_step_witness(&mut self.steps, witness);
            }
            return Some((Some(Why::Step(error)), moved));
        }
        // Within a triangle a field no steeper than `steepest` strays from its corners' plane by
        // at most twice `steepest` times its size: more is a step, where a face ends.
        let step = 2.0f64.mul_add(self.bound.steepest * longest, 2.0 * self.lane.snap);
        if error > step && !clipped {
            return None;
        }
        // Across the toe: the samples disagree, or a moved corner shares the triangle with one
        // that keeps the ground and is not on a traced toe line (the ground beyond the toe, which
        // a long triangle would lift by a fraction of the tolerance: #65 wants it exact).
        let across = longest > refine.toe
            && ((kept && moved)
                || (corner_moved.iter().any(|&(m, _)| m)
                    && corner_moved.iter().any(|&(m, v)| {
                        // A ring vertex keeps the ground for the stitch, not because it lies
                        // beyond the toe; the ring is never split, so it cannot be refined away.
                        !m && !self.on_traced(v) && !self.ring.get(v).copied().unwrap_or(false)
                    })));
        if error > refine.tolerance || across {
            return Some((Some(Why::Error(error)), moved));
        }
        Some((thin.then_some(Why::Angle), moved))
    }

    /// The inside faces, by a walk from a face inside the ring that never crosses it.
    fn inside(&self) -> Vec<[usize; 3]> {
        let ring = &self.ring_plan;
        let seed = self.cdt.inner_faces().find(|face| {
            let [p, q, r] = face.positions();
            plan::inside(ring, (p.x + q.x + r.x) / 3.0, (p.y + q.y + r.y) / 3.0) == Some(true)
        });
        let Some(seed) = seed else {
            return Vec::new();
        };
        let mut visited = vec![false; self.cdt.num_all_faces()];
        if let Some(flag) = visited.get_mut(seed.fix().index()) {
            *flag = true;
        }
        let mut queue = VecDeque::from([seed.fix()]);
        let mut out = Vec::new();
        while let Some(fixed) = queue.pop_front() {
            let face = self.cdt.face(fixed);
            out.push(face.vertices().map(|v| v.fix().index()));
            for edge in face.adjacent_edges() {
                let [from, to] = edge.vertices().map(|v| v.fix().index());
                if self.ring_sides.contains(&key(from, to)) {
                    continue;
                }
                if let Some(next) = edge.rev().face().as_inner()
                    && let Some(flag) = visited.get_mut(next.fix().index())
                    && !*flag
                {
                    *flag = true;
                    queue.push_back(next.fix());
                }
            }
        }
        out
    }

    /// Whether `[a, b, c]` is still a face, counter-clockwise.
    fn exists(&self, [a, b, c]: [usize; 3]) -> bool {
        self.cdt
            .get_edge_from_neighbors(
                FixedVertexHandle::from_index(a),
                FixedVertexHandle::from_index(b),
            )
            .is_some_and(|edge| !edge.is_outer_edge() && edge.next().to().fix().index() == c)
    }

    fn is_constraint(&self, a: usize, b: usize) -> bool {
        self.kinds.contains_key(&key(a, b))
    }

    /// Record a vertex `spade` just added at `p`; `Stop::Leave` when it merged with one.
    fn added(&mut self, handle: FixedVertexHandle, p: Point2<f64>) -> Result<usize, Stop> {
        let index = handle.index();
        if index != self.base.len() {
            return Err(Stop::Leave);
        }
        let base = self
            .lane
            .base
            .height_at(p.x, p.y)
            .ok_or(Stop::Broken)?
            .height;
        let next = index
            .checked_sub(self.inside)
            .and_then(|offset| u32::try_from(offset).ok())
            .and_then(|offset| self.original.checked_add(offset))
            .ok_or(Stop::Broken)?;
        self.vertex.push(next);
        self.base.push(base);
        self.ring.push(false);
        Ok(index)
    }

    /// Insert a free point inside the region.
    fn insert(&mut self, p: Point2<f64>) -> Result<(), Stop> {
        if self.crowds(p) {
            return Err(Stop::Leave);
        }
        let handle = self.cdt.insert(p).map_err(|_| Stop::Leave)?;
        self.added(handle, p).map(|_| ())
    }

    /// Whether `p` is closer than half the shortest side to a vertex already there: one of the
    /// corners of the triangle (or side) it falls in, or one of their neighbours.
    fn crowds(&self, p: Point2<f64>) -> bool {
        let near = self.bound.refine.shortest / 2.0;
        let corners: Vec<_> = match self.cdt.locate(p) {
            PositionInTriangulation::OnVertex(_) => return true,
            PositionInTriangulation::OnFace(face) => self.cdt.face(face).vertices().to_vec(),
            PositionInTriangulation::OnEdge(edge) => {
                self.cdt.directed_edge(edge).vertices().to_vec()
            }
            _ => return false,
        };
        corners.iter().any(|vertex| {
            distance(vertex.position(), p) < near
                || vertex
                    .out_edges()
                    .any(|edge| distance(edge.to().position(), p) < near)
        })
    }

    /// Split the constrained side `[a, b]` at its midpoint, keeping both halves constrained.
    /// Angle-only work keeps a kerb's support whole and twice the shortest side is its limit;
    /// height-error work may split the kerb and continues to the shortest side, since leaving the
    /// adjacent triangle unrefined makes steeper slivers beside the kerb (#149/#170).
    fn split(&mut self, [a, b]: [usize; 2], error: bool) -> Result<(), Stop> {
        let (pa, pb) = (
            self.position(a).ok_or(Stop::Broken)?,
            self.position(b).ok_or(Stop::Broken)?,
        );
        let kinds = self.kinds.get(&key(a, b)).cloned().ok_or(Stop::Leave)?;
        let supports = self.lane.traced[1];
        let support = kinds
            .iter()
            .any(|kind| matches!(kind, Kind::Line { line, .. } if *line >= supports));
        let shortest = self.bound.refine.shortest * if error { 1.0 } else { 2.0 };
        if kinds.contains(&Kind::Ring)
            || distance(pa, pb) < shortest
            || (support && !error && self.kerb([a, b]))
        {
            return Err(Stop::Leave);
        }
        let (ha, hb) = (
            FixedVertexHandle::from_index(a),
            FixedVertexHandle::from_index(b),
        );
        let edge = self
            .cdt
            .get_edge_from_neighbors(ha, hb)
            .ok_or(Stop::Broken)?
            .as_undirected()
            .fix();
        if !self.cdt.remove_constraint_edge(edge) {
            return Err(Stop::Broken);
        }
        self.kinds.remove(&key(a, b));
        let mid = Point2::new(pa.x.midpoint(pb.x), pa.y.midpoint(pb.y));
        let handle = self.cdt.insert(mid).map_err(|_| Stop::Broken)?;
        if self.added(handle, mid).is_err() {
            return self.restore([ha, hb], &kinds, &[]);
        }
        let mut placed: Vec<[usize; 2]> = Vec::new();
        for (from, to) in [(ha, handle), (handle, hb)] {
            let pieces = self.cdt.try_add_constraint(from, to);
            if pieces.is_empty() {
                // A half that will not go in (beside a sliver rounded onto a base edge): undo the
                // split and leave the triangle.
                self.forget(handle);
                return self.restore([ha, hb], &kinds, &placed);
            }
            for piece in pieces {
                let piece = self.cdt.directed_edge(piece);
                let [p, q] = piece.vertices().map(|v| v.fix().index());
                placed.push(key(p, q));
                self.kinds.entry(key(p, q)).or_default().extend(&kinds);
            }
        }
        Ok(())
    }

    /// Whether a paved triangle beside the side `[a, b]` is narrower from it than twice the
    /// shortest side: a kerb's face, which a split of the side would cut into slivers tilting
    /// across the kerb. The pavement's other triangles take a split of its edge as it comes.
    fn kerb(&self, [a, b]: [usize; 2]) -> bool {
        let (Some(pa), Some(pb)) = (self.position(a), self.position(b)) else {
            return true;
        };
        let length = distance(pa, pb);
        let Some(edge) = self.cdt.get_edge_from_neighbors(
            FixedVertexHandle::from_index(a),
            FixedVertexHandle::from_index(b),
        ) else {
            return true;
        };
        [edge, edge.rev()].iter().any(|side| {
            let Some(face) = side.face().as_inner() else {
                return false;
            };
            let corners = face.positions();
            let centroid = [
                corners.iter().map(|corner| corner.x).sum::<f64>() / 3.0,
                corners.iter().map(|corner| corner.y).sum::<f64>() / 3.0,
            ];
            if !(self.lane.paved)(centroid[0], centroid[1]) {
                return false;
            }
            let apex = side.next().to().position();
            let height = (pb.x - pa.x)
                .mul_add(apex.y - pa.y, -(pb.y - pa.y) * (apex.x - pa.x))
                .abs()
                / length.max(f64::MIN_POSITIVE);
            height < 2.0 * self.bound.refine.shortest
        })
    }

    /// Remove the newest vertex, `handle`, and its records: `spade` removes a vertex by moving
    /// the last into its place, so the newest leaves every other index as it was.
    fn forget(&mut self, handle: FixedVertexHandle) {
        if handle.index() + 1 == self.cdt.num_vertices() && handle.index() + 1 == self.base.len() {
            self.cdt.remove(handle);
            self.vertex.pop();
            self.base.pop();
            self.ring.pop();
        }
    }

    /// Put the constraint `[a, b]` of `kinds` back after a split that did not go in, dropping
    /// the half pieces `placed`; the triangle is left.
    fn restore(
        &mut self,
        [a, b]: [FixedVertexHandle; 2],
        kinds: &[Kind],
        placed: &[[usize; 2]],
    ) -> Result<(), Stop> {
        for side in placed {
            self.kinds.remove(side);
        }
        if self.cdt.num_vertices() != self.base.len() {
            return Err(Stop::Broken);
        }
        let pieces = self.cdt.try_add_constraint(a, b);
        if pieces.is_empty() {
            return Err(Stop::Broken);
        }
        for piece in pieces {
            let [p, q] = self
                .cdt
                .directed_edge(piece)
                .vertices()
                .map(|v| v.fix().index());
            self.kinds.entry(key(p, q)).or_default().extend(kinds);
        }
        Err(Stop::Leave)
    }

    /// The constrained side a point at `target` would cross on its way from `from`, or fall in
    /// the diametral circle of; `Err` when the way meets a vertex.
    fn encroached(
        &self,
        from: Point2<f64>,
        target: Point2<f64>,
    ) -> Result<Option<[usize; 2]>, Stop> {
        // A walk that does not end within this many edges is left: on near-degenerate input the
        // iterator can cycle (the 2026-11-07 TwoPaths fixture hung in it).
        let mut walked = 0_u32;
        for intersection in LineIntersectionIterator::new(&self.cdt, from, target) {
            walked += 1;
            if walked > WALK {
                return Err(Stop::Leave);
            }
            match intersection {
                Intersection::EdgeIntersection(edge) | Intersection::EdgeOverlap(edge) => {
                    let [p, q] = edge.vertices().map(|v| v.fix().index());
                    if self.is_constraint(p, q) {
                        return Ok(Some([p, q]));
                    }
                }
                Intersection::VertexIntersection(_) => return Err(Stop::Leave),
            }
        }
        let sides: Vec<[usize; 2]> = match self.cdt.locate(target) {
            PositionInTriangulation::OnFace(face) => self
                .cdt
                .face(face)
                .adjacent_edges()
                .iter()
                .map(|edge| edge.vertices().map(|v| v.fix().index()))
                .collect(),
            PositionInTriangulation::OnEdge(edge) => {
                vec![
                    self.cdt
                        .directed_edge(edge)
                        .vertices()
                        .map(|v| v.fix().index()),
                ]
            }
            _ => return Err(Stop::Leave),
        };
        for [p, q] in sides {
            if !self.is_constraint(p, q) {
                continue;
            }
            let (Some(pp), Some(pq)) = (self.position(p), self.position(q)) else {
                continue;
            };
            let centre = Point2::new(pp.x.midpoint(pq.x), pp.y.midpoint(pq.y));
            if distance(centre, target) <= distance(pp, pq) / 2.0 {
                return Ok(Some([p, q]));
            }
        }
        Ok(None)
    }

    /// One vertex for the triangle `[a, b, c]`.
    fn fix(&mut self, [first, second, _]: [usize; 3], why: Why) -> Result<(), Stop> {
        let face = self
            .cdt
            .get_edge_from_neighbors(
                FixedVertexHandle::from_index(first),
                FixedVertexHandle::from_index(second),
            )
            .ok_or(Stop::Leave)?
            .face()
            .as_inner()
            .ok_or(Stop::Leave)?;
        let corners = face.positions();
        let centroid = Point2::new(
            corners.iter().map(|c| c.x).sum::<f64>() / 3.0,
            corners.iter().map(|c| c.y).sum::<f64>() / 3.0,
        );
        let [p, q, r] = corners;
        let longest = distance(p, q).max(distance(q, r)).max(distance(r, p));
        let doubled = (q.x - p.x).mul_add(r.y - p.y, -(q.y - p.y) * (r.x - p.x));
        // A triangle of collinear corners has its circumcentre at infinity, where the walk
        // towards it never ends (the 2026-11-07 TwoPaths fixture hung there): it is left.
        let centre = face.circumcenter();
        if doubled.abs() <= 1e-12 * longest * longest
            || !(centre.x.is_finite() && centre.y.is_finite())
        {
            return Err(Stop::Leave);
        }
        // A sliver's circumcentre is far off: the point goes no further than a few of its
        // lengths that way, which is where its refinement acts.
        let reach = REACH * longest;
        let off = distance(centroid, centre);
        let centre = if off > reach {
            Point2::new(
                (centre.x - centroid.x).mul_add(reach / off, centroid.x),
                (centre.y - centroid.y).mul_add(reach / off, centroid.y),
            )
        } else {
            centre
        };
        // A point closer than half the shortest side to the triangle's corners would only chase
        // one spot ever closer (a refinement that does not converge): the triangle is left.
        let crowded = |point: Point2<f64>| {
            corners
                .iter()
                .any(|&corner| distance(corner, point) < self.bound.refine.shortest / 2.0)
        };
        let vertices = face.vertices().map(|v| v.fix().index());
        let (centre_crowded, centroid_crowded) = (crowded(centre), crowded(centroid));
        match self.encroached(centroid, centre) {
            Ok(None) if centre_crowded => Err(Stop::Leave),
            Ok(None) => self.insert(centre),
            Ok(Some(side)) => match self.split(side, matches!(why, Why::Error(_) | Why::Step(_))) {
                Err(Stop::Leave) if matches!(why, Why::Error(_) | Why::Step(_)) => {
                    self.fall_back(vertices, centroid, centroid_crowded)
                }
                other => other,
            },
            Err(stop) => match why {
                Why::Error(_) | Why::Step(_) => {
                    self.fall_back(vertices, centroid, centroid_crowded)
                }
                Why::Angle => Err(stop),
            },
        }
    }

    /// A triangle refined for its error whose circumcentre will not do: its centroid, or, where
    /// that crowds a corner (a long thin triangle, whose centroids would chase one spot), its
    /// longest side bisected -- split if constrained, its midpoint inserted if free.
    fn fall_back(
        &mut self,
        [a, b, c]: [usize; 3],
        centroid: Point2<f64>,
        crowded: bool,
    ) -> Result<(), Stop> {
        if !crowded {
            return self.insert(centroid);
        }
        let (Some(pa), Some(pb), Some(pc)) = (self.position(a), self.position(b), self.position(c))
        else {
            return Err(Stop::Leave);
        };
        let [(length, from, to, start, end), ..] = {
            let mut sides = [
                (distance(pa, pb), a, b, pa, pb),
                (distance(pb, pc), b, c, pb, pc),
                (distance(pc, pa), c, a, pc, pa),
            ];
            sides.sort_by(|left, right| right.0.total_cmp(&left.0));
            sides
        };
        if length < 2.0 * self.bound.refine.shortest {
            return Err(Stop::Leave);
        }
        if self.is_constraint(from, to) {
            return self.split([from, to], true);
        }
        self.insert(Point2::new(
            start.x.midpoint(end.x),
            start.y.midpoint(end.y),
        ))
    }
}

/// How far either side of a side's midpoint the ground is asked which base triangle it is in,
/// metres: far beyond a crossing vertex's rounding, far inside any triangle the rebuild keeps.
const PROBE: f64 = 1e-7;

/// Whether the side `p`-`q` lies on a base edge where the ground kinks: just either side of
/// its midpoint the ground is in two base triangles of different planes. The rebuild frees a
/// base edge between coplanar triangles, so such a side is free here too. The ground is asked
/// rather than the side's two triangles: a crossing rounded onto a base edge leaves slivers
/// along it whose far corner is on the edge, and on both planes.
fn kinked(ground: &crate::terrain::field::Field, p: [f64; 3], q: [f64; 3]) -> bool {
    let (dx, dy) = (q[0] - p[0], q[1] - p[1]);
    let length = dx.hypot(dy);
    if length <= 0.0 {
        return false;
    }
    let (nx, ny) = (-dy / length * PROBE, dx / length * PROBE);
    let (mx, my) = (p[0].midpoint(q[0]), p[1].midpoint(q[1]));
    let (left, right) = (
        ground.height_at(mx + nx, my + ny),
        ground.height_at(mx - nx, my - ny),
    );
    let (Some(left), Some(right)) = (left, right) else {
        return true;
    };
    if left.face == right.face {
        return false;
    }
    let (Some(a), Some(b)) = (
        ground.normal_at(mx + nx, my + ny),
        ground.normal_at(mx - nx, my - ny),
    ) else {
        return true;
    };
    a.iter().zip(&b).any(|(u, v)| (u - v).abs() > 1e-12)
}

/// `mesh` refined to `bound` on one triangulation, and what that did; `None` when the
/// triangulation could not be built from it or a split broke it (the caller keeps `mesh`).
pub(in crate::terrain) fn refine(
    lane: &Lane<'_>,
    memo: &mut Memo,
    mesh: &RegionMesh,
    bound: Bound,
) -> Option<(RegionMesh, Refined, Vec<StepWitness>)> {
    let corners = plan::vertices(&mesh.points);
    let triangles = mesh.triangles.as_chunks::<3>().0;
    let mut mesher = Mesher {
        lane,
        memo,
        bound,
        cdt: Cdt::new(),
        vertex: Vec::new(),
        base: Vec::new(),
        ring: Vec::new(),
        original: u32::try_from(corners.len()).ok()?,
        inside: 0,
        kinds: BTreeMap::new(),
        ring_sides: BTreeSet::new(),
        steps: Vec::new(),
        assessed: HashMap::new(),
        ring_plan: mesh
            .ring
            .iter()
            .filter_map(|&v| corners.get(v as usize).copied())
            .collect(),
    };
    // The inside's vertices, ascending.
    let inside_vertices: BTreeSet<u32> = triangles
        .iter()
        .zip(&mesh.inside)
        .filter(|(_, inside)| **inside)
        .flat_map(|(t, _)| *t)
        .collect();
    let mut local = BTreeMap::new();
    let ring_vertices: BTreeSet<u32> = mesh.ring.iter().copied().collect();
    for &v in &inside_vertices {
        let &[x, y, z] = corners.get(v as usize)?;
        let handle = mesher.cdt.insert(Point2::new(x, y)).ok()?;
        if handle.index() != mesher.vertex.len() {
            return None;
        }
        mesher.vertex.push(v);
        mesher.base.push(z);
        mesher.ring.push(ring_vertices.contains(&v));
        local.insert(v, handle.index());
    }
    mesher.inside = mesher.vertex.len();
    let local_of = |v: u32| local.get(&v).copied();
    // Every constrained side and what it is.
    let mut constraints: Vec<([usize; 2], Kind)> = Vec::new();
    for (k, &v) in mesh.ring.iter().enumerate() {
        let next = *mesh.ring.get((k + 1) % mesh.ring.len())?;
        constraints.push(([local_of(v)?, local_of(next)?], Kind::Ring));
    }
    for edge in &mesh.line_edges {
        let [p, q] = edge.vertices;
        constraints.push((
            [local_of(p)?, local_of(q)?],
            Kind::Line {
                line: edge.line,
                segment: edge.segment,
            },
        ));
    }
    let mut sides: BTreeMap<[u32; 2], Vec<usize>> = BTreeMap::new();
    for (index, (&[a, b, c], _)) in triangles
        .iter()
        .zip(&mesh.inside)
        .enumerate()
        .filter(|(_, (_, inside))| **inside)
    {
        for (p, q) in [(a, b), (b, c), (c, a)] {
            sides.entry([p.min(q), p.max(q)]).or_default().push(index);
        }
    }
    for (&[p, q], users) in &sides {
        if let ([_, _], Some(&a), Some(&b)) = (
            users.as_slice(),
            corners.get(p as usize),
            corners.get(q as usize),
        ) && kinked(lane.base, a, b)
        {
            constraints.push(([local_of(p)?, local_of(q)?], Kind::Base));
        }
    }
    for ([p, q], kind) in constraints {
        let pieces = mesher.cdt.try_add_constraint(
            FixedVertexHandle::from_index(p),
            FixedVertexHandle::from_index(q),
        );
        if pieces.is_empty() {
            // A crossing rounded onto a base edge leaves a sliver along it whose sides overlap
            // in exact arithmetic; the edge is already constrained by the sliver's other sides.
            // A ring or line side refused is a copy unlike the mesh.
            if kind == Kind::Base {
                continue;
            }
            return None;
        }
        for piece in pieces {
            let [a, b] = mesher
                .cdt
                .directed_edge(piece)
                .vertices()
                .map(|v| v.fix().index());
            let kinds = mesher.kinds.entry(key(a, b)).or_default();
            if !kinds.contains(&kind) {
                kinds.push(kind);
            }
        }
    }

    mesher.ring_sides = mesher
        .kinds
        .iter()
        .filter(|(_, kinds)| kinds.contains(&Kind::Ring))
        .map(|(side, _)| *side)
        .collect();
    let mut report = Refined::default();
    let mut left: BTreeSet<[usize; 3]> = BTreeSet::new();
    let budget = usize::try_from(bound.refine.budget).unwrap_or(usize::MAX);
    let initial = mesher.base.len();
    loop {
        report.rounds += 1;
        let mut bad: Vec<([usize; 3], Why)> = Vec::new();
        let mut thin: Vec<[usize; 3]> = Vec::new();
        // The sides of the triangles where the design moves the ground: a thin triangle is
        // refined there and beside them (the toe line's other side), not across the rest of
        // the region, which is the ground's own mesh (the unified build's is the property).
        let mut graded: BTreeSet<[usize; 2]> = BTreeSet::new();
        let mut step_remaining = false;
        report.residual = 0.0;
        for triangle in mesher.inside() {
            let mut sorted = triangle;
            sorted.sort_unstable();
            let (why, moved) = mesher.assess(triangle);
            if moved {
                let [a, b, c] = triangle;
                graded.extend([key(a, b), key(b, c), key(c, a)]);
            }
            if let Some(Why::Error(error)) = why {
                report.residual = report.residual.max(error);
            }
            step_remaining |= matches!(why, Some(Why::Step(_)));
            match why {
                _ if left.contains(&sorted) => {}
                Some(Why::Angle) if !moved => thin.push(triangle),
                Some(why) => bad.push((triangle, why)),
                None => {}
            }
        }
        bad.extend(
            thin.into_iter()
                .filter(|&[a, b, c]| {
                    [key(a, b), key(b, c), key(c, a)]
                        .iter()
                        .any(|side| graded.contains(side))
                })
                .map(|triangle| (triangle, Why::Angle)),
        );
        if bad.is_empty() {
            // A refused triangle stays in `left` while unrelated work settles. Its residual is
            // still unresolved; do not report a successful exhaustion state just because no
            // actionable triangles remain.
            report.exhausted = step_remaining || report.residual > bound.refine.tolerance;
            break;
        }
        if report.rounds > MAX_ROUNDS || mesher.base.len() - initial >= budget {
            report.exhausted = true;
            break;
        }
        // Actionable errors first, then boundary steps, then angles.
        bad.sort_by(|(_, x), (_, y)| match (x, y) {
            (Why::Error(p), Why::Error(q)) | (Why::Step(p), Why::Step(q)) => q.total_cmp(p),
            (Why::Error(_), _) | (Why::Step(_), Why::Angle) => std::cmp::Ordering::Less,
            (Why::Step(_), Why::Error(_)) | (Why::Angle, Why::Error(_) | Why::Step(_)) => {
                std::cmp::Ordering::Greater
            }
            (Why::Angle, Why::Angle) => std::cmp::Ordering::Equal,
        });
        let before = mesher.base.len();
        for (triangle, why) in bad {
            if mesher.base.len() - initial >= budget {
                break;
            }
            if !mesher.exists(triangle) {
                continue;
            }
            match mesher.fix(triangle, why) {
                Ok(()) => {}
                Err(Stop::Leave) => {
                    let mut sorted = triangle;
                    sorted.sort_unstable();
                    left.insert(sorted);
                }
                Err(Stop::Broken) => return None,
            }
        }
        if mesher.base.len() == before {
            report.exhausted = step_remaining || report.residual > bound.refine.tolerance;
            break;
        }
    }
    report.added = u32::try_from(mesher.base.len() - initial).ok()?;
    let built = assemble(mesh, &mesher)?;
    Some((built, report, mesher.steps))
}

/// The region mesh: the outside as it was, the inside from the triangulation.
fn assemble(mesh: &RegionMesh, mesher: &Mesher<'_, '_>) -> Option<RegionMesh> {
    let mut points = mesh.points.clone();
    let mut new_vertices = mesh.new_vertices.clone();
    for v in mesher.inside..mesher.vertex.len() {
        let p = mesher.position(v)?;
        points.extend([p.x, p.y, *mesher.base.get(v)?]);
        new_vertices.push(RegionVertex::Refined);
    }
    let global = |v: usize| mesher.vertex.get(v).copied();
    let mut triangles = Vec::new();
    let mut parents = Vec::new();
    let mut kept = Vec::new();
    let mut inside = Vec::new();
    for (k, (&[a, b, c], &is_inside)) in mesh
        .triangles
        .as_chunks::<3>()
        .0
        .iter()
        .zip(&mesh.inside)
        .enumerate()
    {
        if is_inside {
            continue;
        }
        triangles.extend([a, b, c]);
        parents.push(*mesh.parents.get(k)?);
        kept.push(*mesh.kept.get(k)?);
        inside.push(false);
    }
    for [a, b, c] in mesher.inside() {
        let (pa, pb, pc) = (
            mesher.position(a)?,
            mesher.position(b)?,
            mesher.position(c)?,
        );
        let parent = mesher
            .lane
            .base
            .height_at((pa.x + pb.x + pc.x) / 3.0, (pa.y + pb.y + pc.y) / 3.0)?
            .face;
        triangles.extend([global(a)?, global(b)?, global(c)?]);
        parents.push(parent);
        kept.push(false);
        inside.push(true);
    }
    // Each line's edges along its segments, in order.
    let all = plan::vertices(&points);
    let corner = |v: u32| all.get(v as usize).copied();
    let mut line_edges: Vec<(u32, u32, f64, RegionLineEdge)> = Vec::new();
    for (&[p, q], kinds) in &mesher.kinds {
        for kind in kinds {
            let Kind::Line { line, segment } = *kind else {
                continue;
            };
            let start = mesh
                .breakline_vertices
                .get(line as usize)?
                .get(segment as usize)
                .copied()?;
            let end = mesh
                .breakline_vertices
                .get(line as usize)?
                .get(segment as usize + 1)
                .copied()?;
            let (s, e) = (corner(start)?, corner(end)?);
            let along = |v: u32| {
                corner(v).map(|r| (r[0] - s[0]).mul_add(e[0] - s[0], (r[1] - s[1]) * (e[1] - s[1])))
            };
            let (gp, gq) = (global(p)?, global(q)?);
            let (tp, tq) = (along(gp)?, along(gq)?);
            let (from, to, t) = if tp <= tq { (gp, gq, tp) } else { (gq, gp, tq) };
            line_edges.push((
                line,
                segment,
                t,
                RegionLineEdge {
                    line,
                    segment,
                    vertices: [from, to],
                },
            ));
        }
    }
    line_edges.sort_by(|x, y| (x.0, x.1).cmp(&(y.0, y.1)).then(x.2.total_cmp(&y.2)));
    Some(RegionMesh {
        points,
        triangles,
        parents,
        kept,
        inside,
        input_vertices: mesh.input_vertices,
        split_vertices: mesh.split_vertices.clone(),
        new_vertices,
        breakline_vertices: mesh.breakline_vertices.clone(),
        point_vertices: mesh.point_vertices.clone(),
        line_edges: line_edges.into_iter().map(|(_, _, _, edge)| edge).collect(),
        ring: mesh.ring.clone(),
    })
}
