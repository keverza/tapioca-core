//! S3a.1 property-line mesh split: make a closed straight line a path of mesh
//! edges while leaving every triangle it does not touch exactly as it was.
//!
//! The line is plan-only: its z values are ignored and every new vertex takes
//! its height from the mesh surface (linear along a mesh edge, barycentric
//! inside a triangle), so the surface itself never changes.
//!
//! **Vertices.** Input mesh vertices keep their indices. New vertices follow in
//! line order: each segment's start (unless it is a mesh vertex) and then the
//! segment's crossings with mesh edges by distance along the segment.
//!
//! **Triangles.** Input triangles are visited in order. A triangle the line does
//! not enter, and whose edges gain no vertex, is emitted verbatim (same corner
//! order). A straddled triangle is replaced by the constrained triangulation of
//! its corners, the new vertices on its edges and the line pieces inside it
//! ([`super::triangulate`], so the pieces are verified as edges). Each output
//! triangle names its input parent and whether it lies inside the line.
//!
//! Predicates are plain `f64`, evaluated with one argument order per mesh edge
//! (low vertex to high) so neighbouring triangles always agree. Preconditions:
//! the mesh is a plan-view triangulation (counter-clockwise, non-overlapping,
//! manifold); overlap is not detected, the rest refuses.

use std::collections::{BTreeMap, BTreeSet};

use super::{Constraint, Error, Request, Role, triangulate};

/// Borrowed split request.
#[derive(Clone, Copy, Debug)]
pub struct SplitRequest<'a> {
    /// Flat xyz mesh vertices, world metres.
    pub points: &'a [f64],
    /// Counter-clockwise (in plan) vertex triples.
    pub triangles: &'a [u32],
    /// Flat xyz property-line vertices, implicitly closed (3 to 20,000, no
    /// repeated closing vertex). Only x and y are used.
    pub line: &'a [f64],
    /// Empty, or one arc angle (radians) per line segment; any nonzero angle
    /// refuses with `PropertyLineArcUnsupported`.
    pub arc_angles: &'a [f64],
}

/// The mesh element a new vertex lies on.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Host {
    /// On the input edge `[low, high]`, strictly between its ends.
    Edge([u32; 2]),
    /// Strictly inside the input triangle with this index.
    Face(u32),
}

/// Why a new vertex exists.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Origin {
    /// This property-line vertex.
    LineVertex(u32),
    /// This property-line segment crossing its host edge.
    Crossing(u32),
}

/// One vertex appended after the input vertices.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NewVertex {
    /// What created it.
    pub origin: Origin,
    /// Where on the input mesh it lies (its height comes from there).
    pub host: Host,
}

/// One output edge that carries a property-line segment.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LineEdge {
    /// Property-line segment index (vertex `segment` to the next).
    pub segment: u32,
    /// Output vertices in the segment's direction.
    pub vertices: [u32; 2],
}

/// The split mesh and its mapping to the request.
#[derive(Clone, Debug, PartialEq)]
pub struct SplitMesh {
    /// Input vertices unchanged, then the new vertices.
    pub points: Vec<f64>,
    /// Output triangles (see the module docs for order).
    pub triangles: Vec<u32>,
    /// Per output triangle, its input triangle.
    pub parents: Vec<u32>,
    /// Per output triangle, whether it lies inside the property line.
    pub inside: Vec<bool>,
    /// Per new vertex (output index minus the input vertex count).
    pub new_vertices: Vec<NewVertex>,
    /// Per property-line vertex, its output vertex.
    pub line_vertices: Vec<u32>,
    /// Line edges by segment, then along the segment.
    pub line_edges: Vec<LineEdge>,
}

const MAX_LINE_VERTICES: usize = 20_000;

/// The validated line and where each of its vertices landed.
struct Line {
    plan: Vec<[f64; 2]>,
    vertices: Vec<u32>,
    locations: Vec<Location>,
}

impl Line {
    /// Plan position, output vertex and location of line vertex `index`
    /// (wrapping, so the last segment closes the ring).
    fn at(&self, index: u32) -> Result<([f64; 2], u32, Location), Error> {
        let index = usize::try_from(index).map_err(|_| Error::Index)? % self.plan.len().max(1);
        match (
            self.plan.get(index),
            self.vertices.get(index),
            self.locations.get(index),
        ) {
            (Some(position), Some(vertex), Some(location)) => Ok((*position, *vertex, *location)),
            _ => Err(Error::Index),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Location {
    Vertex(u32),
    Edge([u32; 2]),
    Face(u32),
}

struct Mesh<'a> {
    corners: &'a [[u32; 3]],
    positions: Vec<[f64; 3]>,
    input_count: u32,
    adjacency: BTreeMap<[u32; 2], Vec<u32>>,
    incident: Vec<Vec<u32>>,
    lookup: BTreeMap<[u64; 2], u32>,
    new_vertices: Vec<NewVertex>,
    /// New vertices on each input edge with their parameter from low to high.
    on_edges: BTreeMap<[u32; 2], Vec<(f64, u32)>>,
}

/// Split a terrain mesh along a closed straight property line.
///
/// O(line segments x (mesh vertices + mesh edges)) predicate tests plus one
/// small constrained triangulation per straddled triangle. Worker-classified.
///
/// # Errors
/// `PropertyLineArcUnsupported` for an arc, `PropertyLineOutside` for a line
/// that is malformed, not simple or not covered by the mesh, mesh shape errors
/// as in [`Error`], and the triangulation codes when a straddled triangle
/// cannot be rebuilt or the line is not enforced.
pub fn split_mesh(request: &SplitRequest<'_>) -> Result<SplitMesh, Error> {
    let line = line(request)?;
    let mut mesh = Mesh::new(request)?;
    let mut placed = Line {
        plan: line,
        vertices: Vec::new(),
        locations: Vec::new(),
    };
    for (index, point) in (0_u32..).zip(&placed.plan) {
        let (vertex, location) = mesh.add_line_vertex(index, *point)?;
        placed.vertices.push(vertex);
        placed.locations.push(location);
    }
    let segments = u32::try_from(placed.plan.len()).map_err(|_| Error::Shape)?;
    let mut pieces: BTreeMap<u32, Vec<(u32, [u32; 2])>> = BTreeMap::new();
    let mut line_edges = Vec::new();
    for segment in 0..segments {
        let chain = mesh.chain(segment, &placed)?;
        for pair in chain.windows(2) {
            let &[(u, from), (v, to)] = pair else {
                continue;
            };
            if u == v {
                return Err(Error::TriangulationDegenerate);
            }
            line_edges.push(LineEdge {
                segment,
                vertices: [u, v],
            });
            if let Some(face) = mesh.interior_piece(from, to)? {
                pieces.entry(face).or_default().push((segment, [u, v]));
            }
        }
    }
    let straddled = mesh.straddled(&pieces);
    let mut triangles = Vec::with_capacity(request.triangles.len());
    let mut parents = Vec::with_capacity(mesh.corners.len());
    for (face, corners) in (0_u32..).zip(mesh.corners) {
        if straddled.contains(&face) {
            let local = mesh.rebuild(face, pieces.get(&face).map_or(&[][..], Vec::as_slice))?;
            parents.extend(std::iter::repeat_n(face, local.len() / 3));
            triangles.extend(local);
        } else {
            triangles.extend_from_slice(corners);
            parents.push(face);
        }
    }
    let inside = inside_flags(&triangles, &line_edges, &placed.plan)?;
    let result = SplitMesh {
        points: mesh.positions.iter().flatten().copied().collect(),
        triangles,
        parents,
        inside,
        new_vertices: mesh.new_vertices,
        line_vertices: placed.vertices,
        line_edges,
    };
    verify_split(&result)?;
    Ok(result)
}

/// Every line edge is a triangle edge with exactly one inside neighbour.
fn verify_split(result: &SplitMesh) -> Result<(), Error> {
    let mut sides: BTreeMap<[u32; 2], (usize, usize)> = BTreeMap::new();
    for (face, &inside) in result
        .triangles
        .as_chunks::<3>()
        .0
        .iter()
        .zip(&result.inside)
    {
        for edge in super::classify::face_edges(*face) {
            let entry = sides.entry(super::classify::key(edge)).or_default();
            entry.0 += 1;
            entry.1 += usize::from(inside);
        }
    }
    for edge in &result.line_edges {
        match sides.get(&super::classify::key(edge.vertices)) {
            Some(&(1 | 2, 1)) => {}
            // The line's interior is not covered by the mesh.
            Some(&(1, 0)) => return Err(Error::PropertyLineOutside),
            _ => {
                return Err(Error::BreaklineUnenforced {
                    constraint: edge.segment,
                });
            }
        }
    }
    Ok(())
}

/// Inside flags by winding depth across the line edges (see
/// [`super::classify::winding`]). The depth outside the mesh is 0, so a mesh
/// boundary inside the line (an uncovered hole, or the line's interior leaving
/// the mesh) makes the depths inconsistent or negative and refuses.
fn inside_flags(
    triangles: &[u32],
    line_edges: &[LineEdge],
    line: &[[f64; 2]],
) -> Result<Vec<bool>, Error> {
    let orientation = if ring_area(line) > 0.0 { 1 } else { -1 };
    let mut weights: BTreeMap<[u32; 2], i64> = BTreeMap::new();
    for edge in line_edges {
        *weights.entry(edge.vertices).or_default() += orientation;
    }
    let faces = triangles.as_chunks::<3>().0;
    super::classify::winding(faces, &weights, 0)
        .ok_or(Error::PropertyLineOutside)?
        .into_iter()
        .map(|depth| match depth {
            0 => Ok(false),
            1 => Ok(true),
            _ => Err(Error::PropertyLineOutside),
        })
        .collect()
}

fn ring_area(ring: &[[f64; 2]]) -> f64 {
    ring.iter()
        .zip(ring.iter().cycle().skip(1))
        .map(|(a, b)| cross(*a, *b))
        .sum()
}

/// Validate the line: arcs first (as the editor does), then shape and
/// simplicity. Returns plan positions.
fn line(request: &SplitRequest<'_>) -> Result<Vec<[f64; 2]>, Error> {
    let (points, remainder) = request.line.as_chunks::<3>();
    let outside = Error::PropertyLineOutside;
    if !request.arc_angles.is_empty() && request.arc_angles.len() != points.len() {
        return Err(outside);
    }
    if request.arc_angles.iter().any(|&angle| angle != 0.0) {
        return Err(Error::PropertyLineArcUnsupported);
    }
    if !remainder.is_empty()
        || !(3..=MAX_LINE_VERTICES).contains(&points.len())
        || !request.line.iter().all(|value| value.is_finite())
    {
        return Err(outside);
    }
    let ring: Vec<[f64; 2]> = points.iter().map(|&[x, y, _]| [x, y]).collect();
    if simple(&ring) {
        Ok(ring)
    } else {
        Err(outside)
    }
}

/// No zero-length segment, no touching or crossing between segments other
/// than the shared end of neighbours, and nonzero area.
fn simple(ring: &[[f64; 2]]) -> bool {
    let count = ring.len();
    let segment = |index: usize| -> Option<[[f64; 2]; 2]> {
        Some([*ring.get(index % count)?, *ring.get((index + 1) % count)?])
    };
    let mut area = 0.0;
    for first in 0..count {
        let Some([a, b]) = segment(first) else {
            return false;
        };
        let step = sub(b, a);
        if step[0] == 0.0 && step[1] == 0.0 {
            return false;
        }
        area += cross(a, b);
        for second in first + 1..count {
            let Some([c, d]) = segment(second) else {
                return false;
            };
            let adjacent = second == first + 1 || (first == 0 && second == count - 1);
            if adjacent {
                // Neighbours share one end; they must not fold back over each other.
                let (shared, other_first, other_second) = if second == first + 1 {
                    (b, a, d)
                } else {
                    (a, b, c)
                };
                if orient2(shared, other_first, other_second) == 0.0
                    && dot(sub(other_first, shared), sub(other_second, shared)) > 0.0
                {
                    return false;
                }
            } else if segments_touch([a, b], [c, d]) {
                return false;
            }
        }
    }
    area != 0.0
}

fn segments_touch([a, b]: [[f64; 2]; 2], [c, d]: [[f64; 2]; 2]) -> bool {
    let (o1, o2) = (orient2(a, b, c), orient2(a, b, d));
    let (o3, o4) = (orient2(c, d, a), orient2(c, d, b));
    if o1 * o2 < 0.0 && o3 * o4 < 0.0 {
        return true;
    }
    let within = |p: [f64; 2], q: [f64; 2], r: [f64; 2]| {
        r[0] >= p[0].min(q[0])
            && r[0] <= p[0].max(q[0])
            && r[1] >= p[1].min(q[1])
            && r[1] <= p[1].max(q[1])
    };
    (o1 == 0.0 && within(a, b, c))
        || (o2 == 0.0 && within(a, b, d))
        || (o3 == 0.0 && within(c, d, a))
        || (o4 == 0.0 && within(c, d, b))
}

fn sub(a: [f64; 2], b: [f64; 2]) -> [f64; 2] {
    [a[0] - b[0], a[1] - b[1]]
}

fn cross(a: [f64; 2], b: [f64; 2]) -> f64 {
    a[0] * b[1] - a[1] * b[0]
}

fn dot(a: [f64; 2], b: [f64; 2]) -> f64 {
    a[0] * b[0] + a[1] * b[1]
}

/// Plain `f64` orientation of `r` against the directed line `p -> q`.
fn orient2(p: [f64; 2], q: [f64; 2], r: [f64; 2]) -> f64 {
    cross(sub(q, p), sub(r, p))
}

fn plan(point: [f64; 3]) -> [f64; 2] {
    [point[0], point[1]]
}

fn key([a, b]: [u32; 2]) -> [u32; 2] {
    if a < b { [a, b] } else { [b, a] }
}

fn sign(value: f64) -> i8 {
    if value > 0.0 {
        1
    } else if value < 0.0 {
        -1
    } else {
        0
    }
}

impl<'a> Mesh<'a> {
    fn new(request: &SplitRequest<'a>) -> Result<Self, Error> {
        let (positions, remainder) = request.points.as_chunks::<3>();
        let (corners, rest) = request.triangles.as_chunks::<3>();
        let input_count = u32::try_from(positions.len()).map_err(|_| Error::Shape)?;
        if !remainder.is_empty() || !rest.is_empty() || u32::try_from(corners.len()).is_err() {
            return Err(Error::Shape);
        }
        if !request.points.iter().all(|value| value.is_finite()) {
            return Err(Error::NonFinite);
        }
        let mut mesh = Self {
            corners,
            positions: positions.to_vec(),
            input_count,
            adjacency: BTreeMap::new(),
            incident: vec![Vec::new(); positions.len()],
            lookup: BTreeMap::new(),
            new_vertices: Vec::new(),
            on_edges: BTreeMap::new(),
        };
        for (vertex, position) in (0_u32..).zip(positions) {
            let bits = [(position[0] + 0.0).to_bits(), (position[1] + 0.0).to_bits()];
            if mesh.lookup.insert(bits, vertex).is_some() {
                return Err(Error::TriangulationDegenerate);
            }
        }
        let mut directed = BTreeSet::new();
        for (face, triangle) in (0_u32..).zip(corners) {
            let [a, b, c] = triangle.map(|vertex| mesh.position(vertex).map_err(|_| Error::Index));
            if orient2(plan(a?), plan(b?), plan(c?)) <= 0.0 {
                return Err(Error::TriangulationDegenerate);
            }
            for edge in super::classify::face_edges(*triangle) {
                if !directed.insert(edge) {
                    return Err(Error::TriangulationDegenerate);
                }
                mesh.adjacency.entry(key(edge)).or_default().push(face);
            }
            for &vertex in triangle {
                if let Some(list) = usize::try_from(vertex)
                    .ok()
                    .and_then(|i| mesh.incident.get_mut(i))
                {
                    list.push(face);
                }
            }
        }
        Ok(mesh)
    }

    fn position(&self, vertex: u32) -> Result<[f64; 3], Error> {
        usize::try_from(vertex)
            .ok()
            .and_then(|index| self.positions.get(index))
            .copied()
            .ok_or(Error::Index)
    }

    fn corners_of(&self, face: u32) -> Result<[u32; 3], Error> {
        usize::try_from(face)
            .ok()
            .and_then(|index| self.corners.get(index))
            .copied()
            .ok_or(Error::Index)
    }

    /// Orientation of `point` against an edge, always evaluated low to high and
    /// signed for the requested direction.
    fn side(&self, [from, to]: [u32; 2], point: [f64; 2]) -> Result<i8, Error> {
        let [low, high] = key([from, to]);
        let value = sign(orient2(
            plan(self.position(low)?),
            plan(self.position(high)?),
            point,
        ));
        Ok(if from < to { value } else { -value })
    }

    fn push_vertex(&mut self, position: [f64; 3], vertex: NewVertex) -> Result<u32, Error> {
        let index = u32::try_from(self.positions.len()).map_err(|_| Error::Shape)?;
        self.positions.push(position);
        self.new_vertices.push(vertex);
        Ok(index)
    }

    /// Point on an edge at parameter `t` from its low vertex.
    fn edge_point(&self, edge: [u32; 2], t: f64) -> Result<[f64; 3], Error> {
        let [low, high] = edge.map(|vertex| self.position(vertex));
        let ([lx, ly, lz], [hx, hy, hz]) = (low?, high?);
        Ok([lx + t * (hx - lx), ly + t * (hy - ly), lz + t * (hz - lz)])
    }

    fn edge_parameter(&self, edge: [u32; 2], point: [f64; 2]) -> Result<f64, Error> {
        let low = plan(self.position(edge[0])?);
        let along = sub(plan(self.position(edge[1])?), low);
        Ok(dot(sub(point, low), along) / dot(along, along))
    }

    fn add_line_vertex(&mut self, index: u32, point: [f64; 2]) -> Result<(u32, Location), Error> {
        let bits = [(point[0] + 0.0).to_bits(), (point[1] + 0.0).to_bits()];
        if let Some(&vertex) = self.lookup.get(&bits) {
            return Ok((vertex, Location::Vertex(vertex)));
        }
        for (face, corners) in (0_u32..).zip(self.corners) {
            let mut zero = None;
            let mut outside = false;
            for edge in super::classify::face_edges(*corners) {
                match self.side(edge, point)? {
                    1 => {}
                    0 if zero.is_none() => zero = Some(key(edge)),
                    _ => outside = true,
                }
            }
            if outside {
                continue;
            }
            let origin = Origin::LineVertex(index);
            if let Some(edge) = zero {
                let t = self.edge_parameter(edge, point)?;
                let z = self.edge_point(edge, t)?[2];
                let vertex = self.push_vertex(
                    [point[0], point[1], z],
                    NewVertex {
                        origin,
                        host: Host::Edge(edge),
                    },
                )?;
                self.on_edges.entry(edge).or_default().push((t, vertex));
                return Ok((vertex, Location::Edge(edge)));
            }
            let z = self.face_height(*corners, point)?;
            let vertex = self.push_vertex(
                [point[0], point[1], z],
                NewVertex {
                    origin,
                    host: Host::Face(face),
                },
            )?;
            return Ok((vertex, Location::Face(face)));
        }
        Err(Error::PropertyLineOutside)
    }

    fn face_height(&self, corners: [u32; 3], point: [f64; 2]) -> Result<f64, Error> {
        let [a, b, c] = corners.map(|vertex| self.position(vertex));
        let (a, b, c) = (a?, b?, c?);
        let area = orient2(plan(a), plan(b), plan(c));
        let wa = orient2(plan(b), plan(c), point) / area;
        let wb = orient2(plan(c), plan(a), point) / area;
        let wc = orient2(plan(a), plan(b), point) / area;
        Ok(wa * a[2] + wb * b[2] + wc * c[2])
    }

    /// The segment's vertices in order with their locations: its start, mesh
    /// vertices on it, its crossings with mesh edges, its end.
    fn chain(&mut self, segment: u32, line: &Line) -> Result<Vec<(u32, Location)>, Error> {
        let (from_xy, start, start_at) = line.at(segment)?;
        let (to_xy, end, end_at) = line.at(segment.wrapping_add(1))?;
        let direction = sub(to_xy, from_xy);
        let length = dot(direction, direction);
        let parameter = |point: [f64; 2]| dot(sub(point, from_xy), direction) / length;
        let in_box = |vertex_xy: [f64; 2]| {
            vertex_xy[0] >= from_xy[0].min(to_xy[0])
                && vertex_xy[0] <= from_xy[0].max(to_xy[0])
                && vertex_xy[1] >= from_xy[1].min(to_xy[1])
                && vertex_xy[1] <= from_xy[1].max(to_xy[1])
        };
        let mut inner: Vec<(f64, u32, Location)> = Vec::new();
        for vertex in 0..self.input_count {
            let vertex_xy = plan(self.position(vertex)?);
            if vertex != start
                && vertex != end
                && in_box(vertex_xy)
                && orient2(from_xy, to_xy, vertex_xy) == 0.0
            {
                let t = parameter(vertex_xy);
                if t > 0.0 && t < 1.0 {
                    inner.push((t, vertex, Location::Vertex(vertex)));
                }
            }
        }
        let edges: Vec<([u32; 2], usize)> = self
            .adjacency
            .iter()
            .map(|(edge, faces)| (*edge, faces.len()))
            .collect();
        for (edge, faces) in edges {
            let [a, b] = edge.map(|vertex| self.position(vertex).map(plan));
            let (a, b) = (a?, b?);
            if a[0].max(b[0]) < from_xy[0].min(to_xy[0])
                || a[0].min(b[0]) > from_xy[0].max(to_xy[0])
                || a[1].max(b[1]) < from_xy[1].min(to_xy[1])
                || a[1].min(b[1]) > from_xy[1].max(to_xy[1])
            {
                continue;
            }
            let proper = self.side(edge, from_xy)? * self.side(edge, to_xy)? < 0
                && sign(orient2(from_xy, to_xy, a)) * sign(orient2(from_xy, to_xy, b)) < 0;
            if !proper {
                continue;
            }
            if faces < 2 {
                return Err(Error::PropertyLineOutside);
            }
            let denominator = cross(sub(b, a), direction);
            let t = cross(sub(from_xy, a), direction) / denominator;
            if !t.is_finite() {
                return Err(Error::TriangulationDegenerate);
            }
            let position = self.edge_point(edge, t.clamp(0.0, 1.0))?;
            let vertex = self.push_vertex(
                position,
                NewVertex {
                    origin: Origin::Crossing(segment),
                    host: Host::Edge(edge),
                },
            )?;
            self.on_edges.entry(edge).or_default().push((t, vertex));
            inner.push((parameter(plan(position)), vertex, Location::Edge(edge)));
        }
        inner.sort_by(|left, right| left.0.total_cmp(&right.0).then(left.1.cmp(&right.1)));
        let mut chain = vec![(start, start_at)];
        chain.extend(
            inner
                .into_iter()
                .map(|(_, vertex, location)| (vertex, location)),
        );
        chain.push((end, end_at));
        Ok(chain)
    }

    fn candidates(&self, location: Location) -> Vec<u32> {
        match location {
            Location::Vertex(vertex) => usize::try_from(vertex)
                .ok()
                .and_then(|index| self.incident.get(index))
                .cloned()
                .unwrap_or_default(),
            Location::Edge(edge) => self.adjacency.get(&edge).cloned().unwrap_or_default(),
            Location::Face(face) => vec![face],
        }
    }

    /// The triangle a line piece runs through, or `None` when it runs along an
    /// existing (possibly split) mesh edge.
    fn interior_piece(&self, from: Location, to: Location) -> Result<Option<u32>, Error> {
        let on = |location: Location, edge: [u32; 2]| match location {
            Location::Vertex(vertex) => edge.contains(&vertex),
            Location::Edge(other) => other == edge,
            Location::Face(_) => false,
        };
        let along = match (from, to) {
            (Location::Edge(edge), other) | (other, Location::Edge(edge)) => on(other, edge),
            (Location::Vertex(a), Location::Vertex(b)) => self.adjacency.contains_key(&key([a, b])),
            _ => false,
        };
        if along {
            return Ok(None);
        }
        let first = self.candidates(from);
        let shared: Vec<u32> = self
            .candidates(to)
            .into_iter()
            .filter(|face| first.contains(face))
            .collect();
        match shared.as_slice() {
            [face] => Ok(Some(*face)),
            [] => Err(Error::PropertyLineOutside),
            _ => Err(Error::TriangulationDegenerate),
        }
    }

    fn straddled(&self, pieces: &BTreeMap<u32, Vec<(u32, [u32; 2])>>) -> BTreeSet<u32> {
        let mut faces: BTreeSet<u32> = pieces.keys().copied().collect();
        for edge in self.on_edges.keys() {
            faces.extend(self.adjacency.get(edge).into_iter().flatten());
        }
        faces
    }

    /// Constrained triangulation of one straddled triangle in output indices.
    fn rebuild(&self, face: u32, pieces: &[(u32, [u32; 2])]) -> Result<Vec<u32>, Error> {
        let corners = self.corners_of(face)?;
        let mut ring = Vec::new();
        for [from, to] in super::classify::face_edges(corners) {
            ring.push(from);
            let mut on_edge = self
                .on_edges
                .get(&key([from, to]))
                .cloned()
                .unwrap_or_default();
            on_edge.sort_by(|a, b| a.0.total_cmp(&b.0).then(a.1.cmp(&b.1)));
            if from > to {
                on_edge.reverse();
            }
            ring.extend(on_edge.into_iter().map(|(_, vertex)| vertex));
        }
        let mut globals = ring.clone();
        for &(_, [u, v]) in pieces {
            for vertex in [u, v] {
                if !globals.contains(&vertex) {
                    globals.push(vertex);
                }
            }
        }
        let local = |vertex: u32| -> Result<u32, Error> {
            globals
                .iter()
                .position(|&g| g == vertex)
                .and_then(|index| u32::try_from(index).ok())
                .ok_or(Error::Index)
        };
        let points = globals
            .iter()
            .map(|&vertex| self.position(vertex))
            .collect::<Result<Vec<_>, _>>()?
            .concat();
        let ring_local =
            (0..u32::try_from(ring.len()).map_err(|_| Error::Shape)?).collect::<Vec<_>>();
        let lines = pieces
            .iter()
            .map(|&(_, [u, v])| Ok([local(u)?, local(v)?]))
            .collect::<Result<Vec<_>, Error>>()?;
        let mut constraints = vec![Constraint {
            role: Role::Boundary,
            vertices: &ring_local,
        }];
        constraints.extend(lines.iter().map(|line| Constraint {
            role: Role::Breakline,
            vertices: line,
        }));
        let request = Request {
            points: &points,
            constraints: &constraints,
            z_tolerance: 0.0,
        };
        let tin = triangulate(&request).map_err(|error| match error {
            Error::TriangulationConstraint { constraint }
            | Error::BreaklineUnenforced { constraint } => {
                let segment = usize::try_from(constraint)
                    .ok()
                    .and_then(|index| index.checked_sub(1))
                    .and_then(|index| pieces.get(index));
                segment.map_or(Error::TriangulationDegenerate, |&(segment, _)| {
                    Error::TriangulationConstraint {
                        constraint: segment,
                    }
                })
            }
            other => other,
        })?;
        let identity = tin
            .input_vertices
            .iter()
            .zip(0_u32..)
            .all(|(&vertex, index)| vertex == index);
        if !tin.crossings.is_empty() || !identity || tin.points.len() != points.len() {
            return Err(Error::TriangulationDegenerate);
        }
        tin.triangles
            .iter()
            .map(|&vertex| {
                usize::try_from(vertex)
                    .ok()
                    .and_then(|index| globals.get(index))
                    .copied()
                    .ok_or(Error::Index)
            })
            .collect()
    }
}

#[cfg(test)]
mod tests;
