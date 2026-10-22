//! S4.1 region re-triangulation: rebuild the inside of a closed region of an
//! existing terrain mesh with extra breaklines, keeping everything outside it
//! exactly as it was and sharing every boundary vertex.
//!
//! **Steps.** [`split_mesh`] first makes the region ring a path of mesh edges
//! (the surface does not change). The triangles inside it are then replaced by
//! one plan-only constrained triangulation ([`triangulate`]) whose constraints
//! are, in order: the ring (boundary), **every interior edge of the inside
//! triangles** (so the base surface stays affine inside every output triangle,
//! which is what makes a triangle-sum volume exact), and the caller's
//! breaklines. Heights are not used by the triangulation; every vertex takes its
//! height from the base surface (unchanged for mesh vertices, interpolated for
//! breakline and crossing vertices), so the result is still the base surface,
//! now carrying the breaklines. Callers assign their own heights afterwards.
//!
//! **Stitch.** The ring must come out of the triangulation unchanged: a
//! breakline that touches or crosses it would put a vertex on a boundary edge
//! shared with an untouched triangle (a T-junction), so it refuses instead.
//! Base holes or non-manifold edges inside the region also refuse.
//!
//! **Order.** Output vertices are the split mesh's (input vertices unchanged,
//! then the split's new ones), then new vertices in the triangulation's output
//! order. Outside triangles come first in split order, then the rebuilt inside.

use std::collections::{BTreeMap, BTreeSet};

use super::field::Field;
use super::split::{NewVertex, SplitRequest, split_mesh};
use super::{Constraint, Error, Request, Role, plan, triangulate};

/// Borrowed region request.
#[derive(Clone, Copy, Debug)]
pub struct RegionRequest<'a> {
    /// Flat xyz base mesh vertices, world metres.
    pub points: &'a [f64],
    /// Counter-clockwise (in plan) vertex triples of a plan-view terrain.
    pub triangles: &'a [u32],
    /// Flat xyz closed straight ring (z ignored), as for [`split_mesh`].
    pub region: &'a [f64],
    /// Open flat xyz polylines (z ignored), every vertex strictly inside the
    /// region; their order is the identity of [`RegionLineEdge::line`].
    pub breaklines: &'a [&'a [f64]],
}

/// A constraint a crossing vertex lies on.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Support {
    /// An interior base edge, as its two output vertices (ascending).
    BaseEdge([u32; 2]),
    /// A caller breakline segment.
    Breakline {
        /// Index into [`RegionRequest::breaklines`].
        line: u32,
        /// Segment within it.
        segment: u32,
    },
}

/// Why an appended vertex exists.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RegionVertex {
    /// A caller breakline vertex that is not an existing mesh vertex.
    Breakline {
        /// Index into [`RegionRequest::breaklines`].
        line: u32,
        /// Vertex within it.
        vertex: u32,
    },
    /// Where two constraints cross.
    Crossing([Support; 2]),
}

/// One output edge carrying a caller breakline segment.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RegionLineEdge {
    /// Index into [`RegionRequest::breaklines`].
    pub line: u32,
    /// Segment within it.
    pub segment: u32,
    /// Output vertices in the segment's direction.
    pub vertices: [u32; 2],
}

/// The rebuilt mesh and its mapping to the request.
#[derive(Clone, Debug, PartialEq)]
pub struct RegionMesh {
    /// Flat xyz on the base surface: split mesh vertices, then appended ones.
    pub points: Vec<f64>,
    /// Counter-clockwise triples (see the module docs for order).
    pub triangles: Vec<u32>,
    /// Per output triangle, the input triangle it lies in.
    pub parents: Vec<u32>,
    /// Per output triangle, whether it is its parent verbatim.
    pub kept: Vec<bool>,
    /// Per output triangle, whether it lies inside the region.
    pub inside: Vec<bool>,
    /// Number of input vertices (a prefix of the output, unchanged).
    pub input_vertices: u32,
    /// The split's new vertices, after the input vertices.
    pub split_vertices: Vec<NewVertex>,
    /// Vertices appended after the split mesh's.
    pub new_vertices: Vec<RegionVertex>,
    /// Per breakline, per vertex, its output vertex.
    pub breakline_vertices: Vec<Vec<u32>>,
    /// Breakline edges by line, then segment, then along the segment.
    pub line_edges: Vec<RegionLineEdge>,
    /// The region ring as a closed chain of output vertices.
    pub ring: Vec<u32>,
}

/// Region refusals.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RegionError {
    /// A split or triangulation refusal.
    Terrain(Error),
    /// A breakline is malformed or has a vertex not strictly inside the region.
    Breakline,
    /// A breakline touches or crosses the region boundary.
    BoundaryCrossed,
    /// The inside triangles do not form one disc bounded by the ring (a base
    /// hole or a non-manifold edge inside the region).
    Topology,
    /// A new vertex is off the base surface.
    Coverage,
}

impl RegionError {
    /// Static diagnostic beginning with the stable refusal code.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::Terrain(error) => error.message(),
            Self::Breakline => {
                "RegionBreakline: breaklines need two or more finite vertices strictly inside the region"
            }
            Self::BoundaryCrossed => {
                "RegionBoundaryCrossed: a breakline touches or crosses the region boundary"
            }
            Self::Topology => {
                "RegionTopology: the region must be one hole-free manifold disc of the base mesh"
            }
            Self::Coverage => "RegionCoverage: a new vertex is off the base surface",
        }
    }
}

fn index(value: usize) -> Result<u32, RegionError> {
    u32::try_from(value).map_err(|_| RegionError::Terrain(Error::Shape))
}

fn sorted(a: u32, b: u32) -> [u32; 2] {
    if a < b { [a, b] } else { [b, a] }
}

fn validate_breaklines(request: &RegionRequest<'_>) -> Result<(), RegionError> {
    let ring = plan::vertices(request.region);
    for line in request.breaklines {
        if !line.len().is_multiple_of(3) || line.len() < 6 {
            return Err(RegionError::Breakline);
        }
        for &[x, y, z] in plan::vertices(line) {
            if !(x.is_finite() && y.is_finite() && z.is_finite())
                || plan::inside(ring, x, y) != Some(true)
            {
                return Err(RegionError::Breakline);
            }
        }
    }
    Ok(())
}

/// Rebuild the inside of `request.region` with `request.breaklines`.
///
/// One property-line split plus one constrained triangulation of the whole
/// region interior; worker-classified. Nothing partial is returned.
///
/// # Errors
/// See [`RegionError`].
pub fn retriangulate_region(request: &RegionRequest<'_>) -> Result<RegionMesh, RegionError> {
    let field = Field::new(request.points, request.triangles)
        .map_err(|_| RegionError::Terrain(Error::Shape))?;
    validate_breaklines(request)?;
    let split = split_mesh(&SplitRequest {
        points: request.points,
        triangles: request.triangles,
        line: request.region,
        arc_angles: &[],
    })
    .map_err(RegionError::Terrain)?;
    let split_count = index(split.points.len() / 3)?;
    let corners = split.triangles.as_chunks::<3>().0;

    // Inside topology: edge use counts and the ring.
    let mut uses: BTreeMap<[u32; 2], u32> = BTreeMap::new();
    let mut inside_vertices = BTreeSet::new();
    for (&[a, b, c], _) in corners
        .iter()
        .zip(&split.inside)
        .filter(|(_, inside)| **inside)
    {
        for (p, q) in [(a, b), (b, c), (c, a)] {
            *uses.entry(sorted(p, q)).or_default() += 1;
        }
        inside_vertices.extend([a, b, c]);
    }
    let ring: Vec<u32> = split
        .line_edges
        .iter()
        .map(|edge| edge.vertices[0])
        .collect();
    let ring_edges: BTreeSet<[u32; 2]> = split
        .line_edges
        .iter()
        .map(|edge| sorted(edge.vertices[0], edge.vertices[1]))
        .collect();
    let once: BTreeSet<[u32; 2]> = uses
        .iter()
        .filter(|&(_, &count)| count == 1)
        .map(|(edge, _)| *edge)
        .collect();
    if once != ring_edges || uses.values().any(|&count| count > 2) {
        return Err(RegionError::Topology);
    }
    let interior: Vec<[u32; 2]> = uses
        .iter()
        .filter(|&(_, &count)| count == 2)
        .map(|(edge, _)| *edge)
        .collect();

    // Plan-only local request: mesh vertices, then breakline vertices.
    let mesh_vertices: Vec<u32> = inside_vertices.into_iter().collect();
    let local_of: BTreeMap<u32, u32> = mesh_vertices
        .iter()
        .enumerate()
        .map(|(local, &global)| Ok((global, index(local)?)))
        .collect::<Result<_, RegionError>>()?;
    let local = |global: u32| local_of.get(&global).copied().ok_or(RegionError::Topology);
    let split_points = plan::vertices(&split.points);
    let mut points = Vec::new();
    for &global in &mesh_vertices {
        let &[x, y, _] = split_points
            .get(global as usize)
            .ok_or(RegionError::Topology)?;
        points.extend([x, y, 0.0]);
    }
    let mut owners = Vec::new();
    let mut lines: Vec<Vec<u32>> = Vec::new();
    for (line, polyline) in (0_u32..).zip(request.breaklines) {
        let mut chain = Vec::new();
        for (vertex, &[x, y, _]) in (0_u32..).zip(plan::vertices(polyline)) {
            chain.push(index(points.len() / 3)?);
            owners.push((line, vertex));
            points.extend([x, y, 0.0]);
        }
        lines.push(chain);
    }
    let boundary = ring
        .iter()
        .map(|&v| local(v))
        .collect::<Result<Vec<_>, _>>()?;
    let edges = interior
        .iter()
        .map(|&[a, b]| Ok([local(a)?, local(b)?]))
        .collect::<Result<Vec<[u32; 2]>, RegionError>>()?;
    let mut constraints = vec![Constraint {
        role: Role::Boundary,
        vertices: &boundary,
    }];
    constraints.extend(edges.iter().map(|edge| Constraint {
        role: Role::Breakline,
        vertices: edge,
    }));
    constraints.extend(lines.iter().map(|chain| Constraint {
        role: Role::Breakline,
        vertices: chain,
    }));
    let tin = triangulate(&Request {
        points: &points,
        constraints: &constraints,
        z_tolerance: 0.0,
        refinement: None,
    })
    .map_err(RegionError::Terrain)?;

    // Local output vertex -> global vertex, appending new ones.
    let first_line = index(interior.len() + 1)?;
    let support = |constraint: u32, segment: u32| -> Result<Support, RegionError> {
        if constraint == 0 {
            Err(RegionError::BoundaryCrossed)
        } else if constraint < first_line {
            interior
                .get(constraint as usize - 1)
                .map(|&edge| Support::BaseEdge(edge))
                .ok_or(RegionError::Topology)
        } else {
            Ok(Support::Breakline {
                line: constraint - first_line,
                segment,
            })
        }
    };
    let tin_points = plan::vertices(&tin.points);
    let distinct = tin_points.len() - tin.crossings.len();
    let mut origin: Vec<Option<Result<u32, RegionVertex>>> = vec![None; tin_points.len()];
    let mesh_count = mesh_vertices.len();
    for (input, &out) in tin.input_vertices.iter().enumerate() {
        let slot = origin.get_mut(out as usize).ok_or(RegionError::Topology)?;
        if slot.is_none() {
            *slot = Some(if input < mesh_count {
                Ok(*mesh_vertices.get(input).ok_or(RegionError::Topology)?)
            } else {
                let &(line, vertex) = owners
                    .get(input - mesh_count)
                    .ok_or(RegionError::Topology)?;
                Err(RegionVertex::Breakline { line, vertex })
            });
        }
    }
    for (offset, crossing) in tin.crossings.iter().enumerate() {
        let [a, b] = crossing.segments;
        let kind = RegionVertex::Crossing([
            support(a.constraint, a.segment)?,
            support(b.constraint, b.segment)?,
        ]);
        if let Some(slot) = origin.get_mut(distinct + offset) {
            *slot = Some(Err(kind));
        }
    }
    let mut output_points = split.points.clone();
    let mut new_vertices = Vec::new();
    let mut global = Vec::with_capacity(origin.len());
    for (slot, &[x, y, _]) in origin.iter().zip(tin_points) {
        match slot {
            Some(Ok(mesh)) => global.push(*mesh),
            Some(Err(kind)) => {
                let height = field.height_at(x, y).ok_or(RegionError::Coverage)?.height;
                global.push(split_count + index(new_vertices.len())?);
                new_vertices.push(*kind);
                output_points.extend([x, y, height]);
            }
            None => return Err(RegionError::Topology),
        }
    }
    let map = |local: u32| {
        global
            .get(local as usize)
            .copied()
            .ok_or(RegionError::Topology)
    };
    let is_mesh = |local: u32| matches!(origin.get(local as usize), Some(Some(Ok(_))));

    let mut line_edges = Vec::new();
    for row in &tin.constraint_edges {
        let [a, b] = row.vertices;
        if row.segment.constraint == 0 && !(is_mesh(a) && is_mesh(b)) {
            return Err(RegionError::BoundaryCrossed);
        }
        if row.segment.constraint >= first_line {
            // A breakline leaving through existing ring vertices touches rather
            // than crosses the ring; its outside piece is an unkept edge.
            if !row.kept {
                return Err(RegionError::BoundaryCrossed);
            }
            line_edges.push(RegionLineEdge {
                line: row.segment.constraint - first_line,
                segment: row.segment.segment,
                vertices: [map(a)?, map(b)?],
            });
        }
    }
    let breakline_vertices = lines
        .iter()
        .map(|chain| {
            chain
                .iter()
                .map(|&input| {
                    tin.input_vertices
                        .get(input as usize)
                        .ok_or(RegionError::Topology)
                        .and_then(|&out| map(out))
                })
                .collect::<Result<Vec<_>, _>>()
        })
        .collect::<Result<Vec<_>, _>>()?;

    let input_corners = request.triangles.as_chunks::<3>().0;
    let mut triangles = Vec::new();
    let (mut parents, mut kept, mut inside) = (Vec::new(), Vec::new(), Vec::new());
    for ((triple, &parent), _) in corners
        .iter()
        .zip(&split.parents)
        .zip(&split.inside)
        .filter(|(_, inside)| !**inside)
    {
        triangles.extend_from_slice(triple);
        parents.push(parent);
        kept.push(input_corners.get(parent as usize) == Some(triple));
        inside.push(false);
    }
    for &[a, b, c] in tin.triangles.as_chunks::<3>().0 {
        let triple = [map(a)?, map(b)?, map(c)?];
        let centroid = triple.iter().try_fold([0.0, 0.0], |sum, &v| {
            let &[x, y, _] = plan::vertices(&output_points)
                .get(v as usize)
                .ok_or(RegionError::Topology)?;
            Ok::<_, RegionError>([sum[0] + x / 3.0, sum[1] + y / 3.0])
        })?;
        let parent = field
            .height_at(centroid[0], centroid[1])
            .ok_or(RegionError::Coverage)?
            .face;
        triangles.extend(triple);
        parents.push(parent);
        kept.push(false);
        inside.push(true);
    }
    Ok(RegionMesh {
        points: output_points,
        triangles,
        parents,
        kept,
        inside,
        input_vertices: index(request.points.len() / 3)?,
        split_vertices: split.new_vertices,
        new_vertices,
        breakline_vertices,
        line_edges,
        ring,
    })
}

#[cfg(test)]
mod tests;
