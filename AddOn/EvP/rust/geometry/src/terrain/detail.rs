//! The built terrain at a chosen level of detail: fewer triangles, the features exact.
//!
//! A built mesh is refined to a centimetre so its cut/fill and its certification are exact: tens
//! of thousands of triangles, which an Archicad Mesh cannot be worked with and a site does not
//! need. This makes a coarser surface from it, a constrained Delaunay triangulation of
//!
//! - what must stay exact: the vertices the caller keeps (the existing ground's own vertices,
//!   control points), its lines (authored edges -- paths, regions -- and the existing ground's
//!   edges where the design left it) and the mesh's boundary. A line or boundary vertex is
//!   dropped only where the line stays within [`EXACT`] of it, in plan and height, so a straight
//!   side with a straight grade is one edge;
//! - and, round by round, the built vertex furthest from that surface in height inside each
//!   triangle, until every built vertex is within `tolerance` of it (greedy insertion).
//!
//! The graded faces, the transitions and the pavement's inside keep only the points their
//! shape needs at that tolerance. Because the result is itself a constrained Delaunay
//! triangulation of its points and lines, a consumer that triangulates the same points with
//! the same lines as constraints (an Archicad Mesh's level points and level lines) builds the
//! same surface.

use spade::handles::FixedVertexHandle;
use spade::{ConstrainedDelaunayTriangulation, Point2, PositionInTriangulation, Triangulation};
use std::collections::{BTreeMap, HashMap};

use super::field::Field;

type Cdt = ConstrainedDelaunayTriangulation<Point2<f64>>;

/// How far, metres, a kept line or the boundary may move where a vertex on it is dropped.
pub const EXACT: f64 = 1e-3;

/// Most insertion rounds; each inserts at most one vertex per triangle.
const MAX_ROUNDS: u32 = 200;

/// What to simplify.
pub struct DetailRequest<'a> {
    /// The built mesh, flat xyz.
    pub points: &'a [f64],
    /// Its triangles, index triples.
    pub triangles: &'a [u32],
    /// Per point, whether it stays whatever the tolerance; empty: none.
    pub keep: &'a [bool],
    /// Edges between points that stay edges (chained into polylines where they meet two at a
    /// point, then reduced as one line).
    pub lines: &'a [[u32; 2]],
    /// Largest height difference, metres, between the built vertices and the result; positive.
    pub tolerance: f64,
}

/// The simplified surface.
#[derive(Clone, Debug, PartialEq)]
pub struct Detailed {
    /// Flat xyz, a subset of the request's points.
    pub points: Vec<f64>,
    /// Counter-clockwise index triples into `points`.
    pub triangles: Vec<u32>,
    /// Per output point, its request point.
    pub source: Vec<u32>,
    /// The constrained edges (kept lines and boundary), as output index pairs.
    pub lines: Vec<[u32; 2]>,
    /// Largest height difference left at a built vertex, metres.
    pub error: f64,
}

/// Why there is no simplified surface.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DetailError {
    /// A non-finite or non-positive tolerance, a bad index or a mismatched `keep`.
    Parameters,
    /// The mesh is not a field (its triangles overlap in plan or have no area).
    Field,
    /// The triangulation refused a point or a line.
    Triangulation,
}

impl DetailError {
    /// Static diagnostic beginning with the stable refusal code.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::Parameters => {
                "DetailParameters: the tolerance must be positive and every index valid"
            }
            Self::Field => "DetailField: the built mesh is not a field",
            Self::Triangulation => "DetailTriangulation: a point or line could not be triangulated",
        }
    }
}

/// The surface at `request.tolerance` (see the module).
///
/// # Errors
/// [`DetailError`] for bad parameters, a mesh that is not a field, or a refused triangulation.
pub fn simplify(request: &DetailRequest<'_>) -> Result<Detailed, DetailError> {
    let tolerance = request.tolerance;
    let vertices = request.points.as_chunks::<3>().0;
    let count = vertices.len();
    if !tolerance.is_finite()
        || tolerance <= 0.0
        || !request.points.len().is_multiple_of(3)
        || !request.triangles.len().is_multiple_of(3)
        || (!request.keep.is_empty() && request.keep.len() != count)
        || request.triangles.iter().any(|&v| v as usize >= count)
        || request.lines.iter().flatten().any(|&v| v as usize >= count)
        || request.lines.iter().any(|&[a, b]| a == b)
    {
        return Err(DetailError::Parameters);
    }
    let last = u32::try_from(count).map_err(|_| DetailError::Parameters)?;
    let field = Field::new(request.points, request.triangles).map_err(|_| DetailError::Field)?;
    // Every index was checked above; the default is never reached.
    let at = |v: u32| -> [f64; 3] { vertices.get(v as usize).copied().unwrap_or_default() };

    // The chains that stay edges: the caller's lines and the boundary loops, each reduced to
    // EXACT, the caller's kept points always staying.
    let kept = |v: u32| request.keep.get(v as usize).copied().unwrap_or(false);
    let mut chains: Vec<Vec<u32>> = chain(request.lines)
        .iter()
        .map(|(line, closed)| reduce(line, *closed, &at, &kept))
        .collect();
    chains.extend(
        boundary_loops(request.triangles)
            .iter()
            .map(|ring| reduce(ring, true, &at, &kept)),
    );

    let mut built = Building::default();
    for v in (0..last).filter(|&v| kept(v)) {
        built.insert(at(v), v)?;
    }
    for chain in &chains {
        for &v in chain {
            built.insert(at(v), v)?;
        }
    }
    let mut constraints: Vec<[FixedVertexHandle; 2]> = Vec::new();
    for chain in &chains {
        for pair in chain.windows(2) {
            let &[from, to] = pair else { continue };
            let (Some(&a), Some(&b)) = (built.inserted.get(&from), built.inserted.get(&to)) else {
                continue;
            };
            if a != b && built.cdt.can_add_constraint(a, b) {
                built.cdt.add_constraint(a, b);
                constraints.push([a, b]);
            }
        }
    }
    if built.cdt.num_inner_faces() == 0 {
        return Err(DetailError::Triangulation);
    }

    // Greedy insertion: per triangle, its worst built vertex while over the tolerance.
    let mut left: Vec<u32> = (0..last)
        .filter(|v| !built.inserted.contains_key(v))
        .collect();
    let mut rounds = 0;
    loop {
        rounds += 1;
        let mut worst: BTreeMap<usize, (f64, u32)> = BTreeMap::new();
        for &v in &left {
            let [x, y, z] = at(v);
            let Some((face, plane)) = plane_at(&built.cdt, &built.heights, x, y) else {
                continue; // on a kept vertex, or outside every triangle
            };
            let error = (z - plane).abs();
            if error <= tolerance {
                continue;
            }
            let entry = worst.entry(face).or_insert((error, v));
            if error > entry.0 {
                *entry = (error, v);
            }
        }
        if worst.is_empty() || rounds > MAX_ROUNDS {
            break;
        }
        for &(_, v) in worst.values() {
            built.insert(at(v), v)?;
        }
        left.retain(|v| !built.inserted.contains_key(v));
    }
    let Building {
        cdt,
        heights,
        source,
        ..
    } = built;

    // The faces over the built mesh: a hull face over a notch or a hole is not the terrain's.
    let mut triangles = Vec::new();
    for face in cdt.inner_faces() {
        let corners = face.vertices();
        let [pa, pb, pc] = corners.map(|v| v.position());
        let (cx, cy) = ((pa.x + pb.x + pc.x) / 3.0, (pa.y + pb.y + pc.y) / 3.0);
        if field.height_at(cx, cy).is_none() {
            continue;
        }
        triangles.extend(corners.map(|v| u32::try_from(v.fix().index()).unwrap_or(u32::MAX)));
    }
    let mut points = Vec::with_capacity(source.len() * 3);
    for (vertex, &z) in cdt.vertices().zip(&heights) {
        let p = vertex.position();
        points.extend([p.x, p.y, z]);
    }
    let lines = constraints
        .iter()
        .map(|[a, b]| [a.index(), b.index()].map(|i| u32::try_from(i).unwrap_or(u32::MAX)))
        .collect();
    let error = left
        .iter()
        .filter_map(|&v| {
            let [x, y, z] = at(v);
            plane_at(&cdt, &heights, x, y).map(|(_, plane)| (z - plane).abs())
        })
        .fold(0.0, f64::max);
    Ok(Detailed {
        points,
        triangles,
        source,
        lines,
        error,
    })
}

/// The triangulation being built: per CDT vertex its height and request point.
#[derive(Default)]
struct Building {
    cdt: Cdt,
    heights: Vec<f64>,
    source: Vec<u32>,
    inserted: HashMap<u32, FixedVertexHandle>,
}

impl Building {
    /// Request point `v` at `[x, y, z]`, once.
    fn insert(&mut self, [x, y, z]: [f64; 3], v: u32) -> Result<FixedVertexHandle, DetailError> {
        if let Some(&handle) = self.inserted.get(&v) {
            return Ok(handle);
        }
        let handle = self
            .cdt
            .insert(Point2::new(x, y))
            .map_err(|_| DetailError::Triangulation)?;
        if handle.index() == self.heights.len() {
            self.heights.push(z);
            self.source.push(v);
        }
        self.inserted.insert(v, handle);
        Ok(handle)
    }
}

/// The face at `(x, y)` and the surface's height there; `None` on a vertex or off the hull.
fn plane_at(cdt: &Cdt, heights: &[f64], x: f64, y: f64) -> Option<(usize, f64)> {
    let face = match cdt.locate(Point2::new(x, y)) {
        PositionInTriangulation::OnFace(face) => face,
        PositionInTriangulation::OnEdge(edge) => cdt.directed_edge(edge).face().as_inner()?.fix(),
        _ => return None,
    };
    let vertices = cdt.face(face).vertices();
    let [pa, pb, pc] = vertices.map(|v| v.position());
    let [za, zb, zc] = vertices.map(|v| heights.get(v.fix().index()).copied().unwrap_or(0.0));
    let det = (pb.y - pc.y).mul_add(pa.x - pc.x, (pc.x - pb.x) * (pa.y - pc.y));
    if det == 0.0 {
        return None;
    }
    let wa = (pb.y - pc.y).mul_add(x - pc.x, (pc.x - pb.x) * (y - pc.y)) / det;
    let wb = (pc.y - pa.y).mul_add(x - pc.x, (pa.x - pc.x) * (y - pc.y)) / det;
    let wc = 1.0 - wa - wb;
    Some((face.index(), wa.mul_add(za, wb.mul_add(zb, wc * zc))))
}

/// Edges joined into polylines through every point exactly two of them meet at; a loop of such
/// points is closed (first point not repeated). Duplicate edges count once.
fn chain(edges: &[[u32; 2]]) -> Vec<(Vec<u32>, bool)> {
    let mut unique: Vec<[u32; 2]> = edges.iter().map(|&[a, b]| [a.min(b), a.max(b)]).collect();
    unique.sort_unstable();
    unique.dedup();
    let mut around: BTreeMap<u32, Vec<usize>> = BTreeMap::new();
    for (i, &[a, b]) in unique.iter().enumerate() {
        around.entry(a).or_default().push(i);
        around.entry(b).or_default().push(i);
    }
    let mut used = vec![false; unique.len()];
    let unused = |used: &[bool], e: usize| used.get(e) == Some(&false);
    let walk = |start: u32, first: usize, used: &mut Vec<bool>| {
        let mut line = vec![start];
        let (mut at, mut edge) = (start, first);
        loop {
            if let Some(flag) = used.get_mut(edge) {
                *flag = true;
            }
            let Some(&[a, b]) = unique.get(edge) else {
                break;
            };
            let to = if a == at { b } else { a };
            line.push(to);
            at = to;
            let next = match around.get(&at) {
                Some(list) if list.len() == 2 => list.iter().copied().find(|&e| unused(used, e)),
                _ => None,
            };
            match next {
                Some(e) => edge = e,
                None => break,
            }
        }
        line
    };
    let mut out = Vec::new();
    // Open chains from their ends (points not met by exactly two edges) first.
    for (&v, list) in &around {
        if list.len() == 2 {
            continue;
        }
        for &e in list {
            if unused(&used, e) {
                out.push((walk(v, e, &mut used), false));
            }
        }
    }
    // What is left is loops.
    for (e, &[start, _]) in unique.iter().enumerate() {
        if unused(&used, e) {
            let mut line = walk(start, e, &mut used);
            let closed = line.first() == line.last();
            if closed {
                line.pop();
            }
            out.push((line, closed));
        }
    }
    out
}

/// The mesh's boundary loops: its edges used by one triangle, chained (closed, first point not
/// repeated).
fn boundary_loops(triangles: &[u32]) -> Vec<Vec<u32>> {
    let sides = |&[a, b, c]: &[u32; 3]| [(a, b), (b, c), (c, a)];
    let mut count: HashMap<(u32, u32), u32> = HashMap::new();
    for triangle in triangles.as_chunks::<3>().0 {
        for (a, b) in sides(triangle) {
            *count.entry((a.min(b), a.max(b))).or_default() += 1;
        }
    }
    // Directed boundary edges, as the triangles run them.
    let mut next: BTreeMap<u32, u32> = BTreeMap::new();
    for triangle in triangles.as_chunks::<3>().0 {
        for (a, b) in sides(triangle) {
            if count.get(&(a.min(b), a.max(b))) == Some(&1) {
                next.insert(a, b);
            }
        }
    }
    let mut loops = Vec::new();
    while let Some((&start, _)) = next.iter().next() {
        let mut ring = vec![start];
        let mut at = start;
        while let Some(to) = next.remove(&at) {
            if to == start {
                break;
            }
            ring.push(to);
            at = to;
        }
        if ring.len() >= 3 {
            loops.push(ring);
        }
    }
    loops
}

/// A polyline (or closed ring) reduced to [`EXACT`] in plan and height: Douglas-Peucker on the
/// 3D points, the ends and every kept point staying. A closed ring is cut at its kept points,
/// or at its first point and the one furthest from it; it comes back closed (first repeated).
fn reduce(
    line: &[u32],
    closed: bool,
    at: &dyn Fn(u32) -> [f64; 3],
    kept: &dyn Fn(u32) -> bool,
) -> Vec<u32> {
    let points: Vec<[f64; 3]> = line.iter().map(|&v| at(v)).collect();
    let mut stays: Vec<bool> = line.iter().map(|&v| kept(v)).collect();
    if let Some(first) = stays.first_mut() {
        *first = true;
    }
    if !closed && let Some(last) = stays.last_mut() {
        *last = true;
    }
    if closed && let Some(&origin) = points.first() {
        let far = points
            .iter()
            .enumerate()
            .skip(1)
            .max_by(|(_, p), (_, q)| distance(**p, origin).total_cmp(&distance(**q, origin)))
            .map(|(i, _)| i);
        if let Some(flag) = far.and_then(|i| stays.get_mut(i)) {
            *flag = true;
        }
    }
    // The pieces between staying points, each reduced on its own; a ring's last piece runs back
    // to its first point (`points.len()`, taken modulo its length).
    let mut marks: Vec<usize> = (0..line.len())
        .filter(|&i| stays.get(i) == Some(&true))
        .collect();
    if closed {
        marks.push(line.len());
    }
    for pair in marks.windows(2) {
        if let &[from, to] = pair {
            douglas_peucker(&points, from, to, &mut stays);
        }
    }
    let mut out: Vec<u32> = line
        .iter()
        .zip(&stays)
        .filter(|&(_, &s)| s)
        .map(|(&v, _)| v)
        .collect();
    if closed && let Some(&first) = out.first() {
        out.push(first);
    }
    out
}

fn douglas_peucker(points: &[[f64; 3]], from: usize, to: usize, stays: &mut [bool]) {
    if to <= from + 1 || points.is_empty() {
        return;
    }
    let point = |i: usize| points.get(i % points.len()).copied().unwrap_or_default();
    let (a, b) = (point(from), point(to));
    let mut far = (0.0, from);
    for i in from + 1..to {
        let d = segment_distance(point(i), a, b);
        if d > far.0 {
            far = (d, i);
        }
    }
    if far.0 > EXACT {
        if let Some(flag) = stays.get_mut(far.1 % points.len()) {
            *flag = true;
        }
        douglas_peucker(points, from, far.1, stays);
        douglas_peucker(points, far.1, to, stays);
    }
}

fn distance([ax, ay, az]: [f64; 3], [bx, by, bz]: [f64; 3]) -> f64 {
    (ax - bx).hypot(ay - by).hypot(az - bz)
}

/// The 3D distance from `p` to the segment `a`-`b`.
fn segment_distance(p: [f64; 3], a: [f64; 3], b: [f64; 3]) -> f64 {
    let [ab, ap] = [[b, a], [p, a]].map(|[u, v]| [u[0] - v[0], u[1] - v[1], u[2] - v[2]]);
    let length = ab[0].mul_add(ab[0], ab[1].mul_add(ab[1], ab[2] * ab[2]));
    let t = if length > 0.0 {
        (ap[0].mul_add(ab[0], ap[1].mul_add(ab[1], ap[2] * ab[2])) / length).clamp(0.0, 1.0)
    } else {
        0.0
    };
    distance(
        p,
        [
            t.mul_add(ab[0], a[0]),
            t.mul_add(ab[1], a[1]),
            t.mul_add(ab[2], a[2]),
        ],
    )
}

#[cfg(test)]
mod tests;
