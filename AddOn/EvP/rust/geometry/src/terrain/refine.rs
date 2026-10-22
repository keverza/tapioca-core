//! Bounded spade refinement, with interpolation against the unrefined surface.
//!
//! All original edges are fixed before refinement: sampling original heights
//! alone is insufficient because flipping an original diagonal changes a
//! nonplanar surface between the samples. These extra constraints are private
//! surface supports, not authored breaklines or domain boundaries.

use std::collections::BTreeMap;

use spade::{AngleLimit, Point2, PositionInTriangulation, RefinementParameters, Triangulation};

use super::constrain::{self, Built, Cdt};
use super::input::Prepared;
use super::{ConstraintEdge, Error, SegmentRef, canonical, extract_edges};

mod paths;
#[cfg(test)]
mod tests;

/// Opt-in quality/area targets with a hard additional-vertex budget.
#[derive(Clone, Copy, Debug)]
pub struct Refinement {
    /// Minimum plan triangle angle, in degrees, in 0..=30 (0 disables it).
    /// Input constraint angles cannot necessarily be improved by refinement.
    pub min_angle_degrees: f64,
    /// Optional maximum plan triangle area in square metres, strictly positive.
    pub max_area_square_metres: Option<f64>,
    /// Maximum new vertices over the whole convex hull, including excluded
    /// faces. Exhaustion refuses with `RefinementLimit`, never partial output.
    pub max_added_vertices: u32,
}

/// Original surface support of a refinement vertex (indices precede refinement).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SteinerOrigin {
    /// All authored segments containing this vertex, sorted and deduplicated.
    Segments(Vec<SegmentRef>),
    /// Original non-authored mesh edge, ascending endpoint indices.
    Edge([u32; 2]),
    /// Original CCW face, lowest vertex first; its corners carry input identity.
    Face([u32; 3]),
}

/// One added vertex and its complete original support.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Steiner {
    /// Output vertex index.
    pub vertex: u32,
    /// Original support used to interpolate its height.
    pub origin: SteinerOrigin,
}

pub(super) fn refine(
    prepared: &mut Prepared,
    mut built: Built,
    parameters: Refinement,
) -> Result<(Built, Vec<Steiner>), Error> {
    validate(parameters, prepared.positions.len())?;
    // One full snapshot is needed for spatial point location and interpolation;
    // a mutable/refined surface must never become the elevation oracle.
    let original = built.cdt.clone();
    let handles: Vec<_> = built.cdt.fixed_vertices().collect();
    let owners: Vec<_> = (0..u32::try_from(handles.len()).map_err(|_| Error::Shape)?).collect();
    for edge in extract_edges(&built.faces.iter().flatten().copied().collect::<Vec<_>>())? {
        constrain::add_piece(&mut built.cdt, &handles, &owners, edge)
            .ok_or(Error::TriangulationDegenerate)?;
    }
    let mut options = RefinementParameters::new()
        .with_angle_limit(AngleLimit::from_deg(parameters.min_angle_degrees))
        .with_max_additional_vertices(parameters.max_added_vertices as usize);
    if let Some(area) = parameters.max_area_square_metres {
        options = options.with_max_allowed_area(area);
    }
    // Never use exclude_outer_faces: its constraint parity treats breaklines
    // (and our private surface edges) as rings. Winding remains the authority.
    if !built.cdt.refine(options).refinement_complete {
        return Err(Error::RefinementLimit);
    }
    let paths = paths::Paths::new(&original, &built.cdt)?;
    let mut steiner = Vec::new();
    for vertex in built.cdt.vertices().skip(original.num_vertices()) {
        let index = u32::try_from(vertex.index()).map_err(|_| Error::Shape)?;
        let position = vertex.position();
        let (z, origin) = if let Some(&edge) = paths.hosts.get(&index) {
            (
                edge_height(prepared, edge, position)?,
                SteinerOrigin::Edge(edge),
            )
        } else {
            interpolate(&original, prepared, position)?
        };
        if !z.is_finite() {
            return Err(Error::NonFinite);
        }
        prepared.positions.push([position.x, position.y, z]);
        steiner.push(Steiner {
            vertex: index,
            origin,
        });
    }
    built.rows = paths.rebuild_rows(&built.rows)?;
    let mut segments: BTreeMap<u32, Vec<SegmentRef>> = BTreeMap::new();
    for row in &built.rows {
        for vertex in row.vertices {
            segments.entry(vertex).or_default().push(row.segment);
        }
    }
    for point in &mut steiner {
        if let Some(mut support) = segments.remove(&point.vertex) {
            support.sort_unstable();
            support.dedup();
            point.origin = SteinerOrigin::Segments(support);
        }
    }
    built.faces = constrain::faces(&built.cdt)?;
    Ok((built, steiner))
}

fn validate(parameters: Refinement, vertices: usize) -> Result<(), Error> {
    let valid = parameters.min_angle_degrees.is_finite()
        && (0.0..=30.0).contains(&parameters.min_angle_degrees)
        && parameters
            .max_area_square_metres
            .is_none_or(|area| area.is_finite() && area > 0.0)
        && u32::try_from(vertices)
            .ok()
            .and_then(|count| count.checked_add(parameters.max_added_vertices))
            .is_some();
    if valid {
        Ok(())
    } else {
        Err(Error::RefinementParameters)
    }
}

fn edge_height(
    prepared: &Prepared,
    vertices: [u32; 2],
    position: Point2<f64>,
) -> Result<f64, Error> {
    let [a, b] = vertices.map(|vertex| prepared.position(vertex));
    let (a, b) = (a?, b?);
    let dx = b[0] - a[0];
    let dy = b[1] - a[1];
    let t = if dx.abs() >= dy.abs() {
        (position.x - a[0]) / dx
    } else {
        (position.y - a[1]) / dy
    };
    Ok(a[2] + t * (b[2] - a[2]))
}

fn interpolate(
    original: &Cdt,
    prepared: &Prepared,
    position: Point2<f64>,
) -> Result<(f64, SteinerOrigin), Error> {
    match original.locate(position) {
        PositionInTriangulation::OnEdge(handle) => {
            let edge = original.directed_edge(handle);
            let [a, b] = edge
                .vertices()
                .map(|vertex| u32::try_from(vertex.index()).map_err(|_| Error::Shape));
            let mut vertices = [a?, b?];
            vertices.sort_unstable();
            Ok((
                edge_height(prepared, vertices, position)?,
                SteinerOrigin::Edge(vertices),
            ))
        }
        PositionInTriangulation::OnFace(handle) => {
            let face = original.face(handle);
            let weights = face.barycentric_interpolation(position);
            let [a, b, c] = face
                .vertices()
                .map(|vertex| u32::try_from(vertex.index()).map_err(|_| Error::Shape));
            let vertices = [a?, b?, c?];
            let mut z = 0.0;
            for (vertex, weight) in vertices.into_iter().zip(weights) {
                z += prepared.position(vertex)?[2] * weight;
            }
            Ok((z, SteinerOrigin::Face(canonical(vertices))))
        }
        _ => Err(Error::TriangulationDegenerate),
    }
}
