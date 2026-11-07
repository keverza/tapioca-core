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
//! vertex where the faces end in a step is left too.
//!
//! Nothing is rebuilt between rounds: the triangulation is kept and only the
//! new vertices' heights are evaluated. The result is assembled straight into a
//! region mesh: the outside triangles as they were, the inside from the
//! triangulation, each added vertex a [`RegionVertex::Refined`], and every line
//! edge still a path along its segment.

use std::collections::{BTreeMap, BTreeSet, VecDeque};

use spade::handles::FixedVertexHandle;
use spade::{
    ConstrainedDelaunayTriangulation, Intersection, LineIntersectionIterator, Point2,
    PositionInTriangulation, Triangulation,
};

use super::refine::{Bound, Memo, Refined};
use super::{Faces, plan};
use crate::terrain::region::{RegionLineEdge, RegionMesh, RegionVertex};

type Cdt = ConstrainedDelaunayTriangulation<Point2<f64>>;

/// Rounds before the mesher reports itself exhausted.
const MAX_ROUNDS: u32 = 64;

/// What a constrained side is.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
enum Kind {
    Ring,
    Line { line: u32, segment: u32 },
    Base,
}

/// Why a triangle is refined: its error (this much), or only its angle.
#[derive(Clone, Copy, Debug, PartialEq)]
enum Why {
    Error(f64),
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
    faces: &'a Faces<'f>,
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
                .height(self.faces, p.x, p.y)
                .map_or(base, |(height, _)| height),
        )
    }

    /// Whether the toe crosses the triangle between its samples: they disagree on whether the
    /// design moves the ground. A corner on a traced toe line is neither side, so corners are
    /// not asked; the traced line strays up to its 5 mm tolerance from the exact toe, so one
    /// sample keeping the ground beside it is not a crossing either.
    fn straddles(&mut self, [pa, pb, pc]: [Point2<f64>; 3]) -> bool {
        let mut seen = [false, false];
        for [wa, wb, wc] in super::refine::SAMPLES {
            let (x, y) = (
                wa.mul_add(pa.x, wb.mul_add(pb.x, wc * pc.x)),
                wa.mul_add(pa.y, wb.mul_add(pb.y, wc * pc.y)),
            );
            let (Some(ground), Some((height, _))) = (
                self.faces.base.height_at(x, y),
                self.memo.height(self.faces, x, y),
            ) else {
                continue;
            };
            let moved = height.to_bits() != ground.height.to_bits();
            if let Some(flag) = seen.get_mut(usize::from(moved)) {
                *flag = true;
            }
        }
        seen == [true, true]
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
            .height(self.faces, p.x, p.y)
            .is_some_and(|(height, _)| (height - base).abs() > self.bound.refine.tolerance)
    }

    /// Why the triangle `[a, b, c]` needs a vertex; `None` when it does not or must not get one.
    fn assess(&mut self, [a, b, c]: [usize; 3]) -> Option<Why> {
        let refine = self.bound.refine;
        let (pa, pb, pc) = (self.position(a)?, self.position(b)?, self.position(c)?);
        let longest = distance(pa, pb).max(distance(pb, pc)).max(distance(pc, pa));
        if longest < refine.shortest {
            return None;
        }
        if [a, b, c].iter().any(|&v| self.clipped(v)) {
            return None;
        }
        let corners = [
            [pa.x, pa.y, self.height(a)?],
            [pb.x, pb.y, self.height(b)?],
            [pc.x, pc.y, self.height(c)?],
        ];
        let error = self.memo.error(self.faces, corners);
        // Within a triangle a field no steeper than `steepest` strays from its corners' plane by
        // at most twice `steepest` times its size: more is a step, where a face ends.
        let step = 2.0f64.mul_add(
            self.bound.steepest * longest,
            2.0 * self.faces.grading.tolerance,
        );
        if error > step {
            return None;
        }
        if error > refine.tolerance {
            return Some(Why::Error(error));
        }
        if longest > refine.toe && self.straddles([pa, pb, pc]) {
            return Some(Why::Error(error));
        }
        (smallest_angle(pa, pb, pc) < refine.angle.to_radians()).then_some(Why::Angle)
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
        let mut visited = BTreeSet::from([seed.fix()]);
        let mut queue = VecDeque::from([seed.fix()]);
        let mut out = Vec::new();
        while let Some(fixed) = queue.pop_front() {
            let face = self.cdt.face(fixed);
            out.push(face.vertices().map(|v| v.fix().index()));
            for edge in face.adjacent_edges() {
                let [from, to] = edge.vertices().map(|v| v.fix().index());
                if self
                    .kinds
                    .get(&key(from, to))
                    .is_some_and(|kinds| kinds.contains(&Kind::Ring))
                {
                    continue;
                }
                if let Some(next) = edge.rev().face().as_inner()
                    && visited.insert(next.fix())
                {
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
            .faces
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
        let handle = self.cdt.insert(p).map_err(|_| Stop::Leave)?;
        self.added(handle, p).map(|_| ())
    }

    /// Split the constrained side `[a, b]` at its midpoint, keeping both halves constrained.
    fn split(&mut self, [a, b]: [usize; 2]) -> Result<(), Stop> {
        let (pa, pb) = (
            self.position(a).ok_or(Stop::Broken)?,
            self.position(b).ok_or(Stop::Broken)?,
        );
        let kinds = self.kinds.get(&key(a, b)).cloned().ok_or(Stop::Leave)?;
        if kinds.contains(&Kind::Ring) || distance(pa, pb) < 2.0 * self.bound.refine.shortest {
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
        for intersection in LineIntersectionIterator::new(&self.cdt, from, target) {
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
        let centre = face.circumcenter();
        let corners = face.positions();
        let centroid = Point2::new(
            corners.iter().map(|c| c.x).sum::<f64>() / 3.0,
            corners.iter().map(|c| c.y).sum::<f64>() / 3.0,
        );
        match self.encroached(centroid, centre) {
            Ok(None) => self.insert(centre),
            Ok(Some(side)) => match self.split(side) {
                Err(Stop::Leave) if matches!(why, Why::Error(_)) => self.insert(centroid),
                other => other,
            },
            Err(stop) => match why {
                Why::Error(_) => self.insert(centroid),
                Why::Angle => Err(stop),
            },
        }
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
pub(super) fn refine(
    faces: &Faces<'_>,
    memo: &mut Memo,
    mesh: &RegionMesh,
    bound: Bound,
) -> Option<(RegionMesh, Refined)> {
    let corners = plan::vertices(&mesh.points);
    let triangles = mesh.triangles.as_chunks::<3>().0;
    let mut mesher = Mesher {
        faces,
        memo,
        bound,
        cdt: Cdt::new(),
        vertex: Vec::new(),
        base: Vec::new(),
        ring: Vec::new(),
        original: u32::try_from(corners.len()).ok()?,
        inside: 0,
        kinds: BTreeMap::new(),
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
        ) && kinked(faces.base, a, b)
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

    let mut report = Refined::default();
    let mut left: BTreeSet<[usize; 3]> = BTreeSet::new();
    let budget = usize::try_from(bound.refine.budget).unwrap_or(usize::MAX);
    let initial = mesher.base.len();
    loop {
        report.rounds += 1;
        let mut bad: Vec<([usize; 3], Why)> = Vec::new();
        report.residual = 0.0;
        for triangle in mesher.inside() {
            let mut sorted = triangle;
            sorted.sort_unstable();
            if left.contains(&sorted) {
                if let Some(Why::Error(error)) = mesher.assess(triangle) {
                    report.residual = report.residual.max(error);
                }
                continue;
            }
            if let Some(why) = mesher.assess(triangle) {
                if let Why::Error(error) = why {
                    report.residual = report.residual.max(error);
                }
                bad.push((triangle, why));
            }
        }
        if bad.is_empty() {
            break;
        }
        if report.rounds > MAX_ROUNDS || mesher.base.len() - initial >= budget {
            report.exhausted = true;
            break;
        }
        // The worst errors first, then the angles.
        bad.sort_by(|(_, x), (_, y)| match (x, y) {
            (Why::Error(p), Why::Error(q)) => q.total_cmp(p),
            (Why::Error(_), Why::Angle) => std::cmp::Ordering::Less,
            (Why::Angle, Why::Error(_)) => std::cmp::Ordering::Greater,
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
            report.exhausted = report.residual > bound.refine.tolerance;
            break;
        }
    }
    report.added = u32::try_from(mesher.base.len() - initial).ok()?;
    assemble(mesh, &mesher).map(|built| (built, report))
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
            .faces
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
