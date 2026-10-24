//! S3a.1 terrain TIN (G11): constrained Delaunay triangulation of spot heights,
//! boundary and hole rings and breaklines, with a complete input mapping.
//!
//! Plan positions are world metres in `f64`, heights are z in metres; the
//! triangulation is 2.5D (by xy, carrying z). `spade` (pinned, vendored) does
//! the constrained Delaunay work behind this module; no `spade` type is public.
//!
//! **Pre-split.** `spade` accepts only constraints that meet at vertices, so
//! every proper crossing between two constraint segments gets a vertex first,
//! with the heights of both segments required to agree within the request's
//! `z_tolerance`. Rings may not cross rings; breaklines may cross anything.
//!
//! **Result contract.** Deterministic for a given request: vertices are inserted
//! in a fixed order and every output list has a documented order. Results are
//! equal as triangulations across releases, not byte-identical across `spade`
//! patch versions (the pin keeps them stable here). Triangles are
//! counter-clockwise, rotated to start at their lowest vertex and sorted.
//!
//! **Identity (D14).** Every input point maps to its output vertex; crossing
//! vertices name the two segments they split; every constraint edge names its
//! `(constraint, segment)`; every kept face names its bounding boundary ring.
//! Callers translate these input indices to their source/operation/object IDs.
//! A result whose constraint paths do not verify is refused, never returned.
//!
//! [`split_mesh`] makes a closed property line a path of edges of an existing
//! terrain mesh, rebuilding only the triangles it touches.
//!
//! Optional refinement subdivides the original piecewise-linear surface; every
//! Steiner point names its original constraint segments, edge or face.

mod classify;
mod constrain;
pub mod daylight;
pub mod design;
pub mod edit;
pub mod field;
pub mod hybrid;
mod input;
pub mod outline;
mod plan;
mod presplit;
mod refine;
pub mod region;
mod split;
pub mod surface;
mod verify;
pub mod volume;

#[cfg(test)]
mod profile;
#[cfg(test)]
mod tests;

pub use refine::{Refinement, Steiner, SteinerOrigin};
pub use split::{Host, LineEdge, NewVertex, Origin, SplitMesh, SplitRequest, split_mesh};
pub use verify::{extract_edges, verify_constraints};

/// What a constraint polyline means for the surface and its domain.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Role {
    /// Closed ring; faces inside it belong to the terrain.
    Boundary,
    /// Closed ring; faces inside it are excluded (courtyard, void).
    Hole,
    /// Open polyline the surface must follow as triangle edges.
    Breakline,
}

/// One constraint polyline over input point indices.
#[derive(Clone, Copy, Debug)]
pub struct Constraint<'a> {
    /// Boundary/hole rings are implicitly closed (at least three indices, no
    /// repeated closing index); breaklines are open (at least two).
    pub role: Role,
    /// Indices into [`Request::points`] (vertices, not scalar offsets).
    pub vertices: &'a [u32],
}

/// Borrowed triangulation request.
#[derive(Clone, Copy, Debug)]
pub struct Request<'a> {
    /// Flat xyz world metres. Every point becomes a vertex, including points
    /// outside the kept domain. Points at the same xy are one vertex if their
    /// heights agree within `z_tolerance` (the first height is kept).
    pub points: &'a [f64],
    /// Constraints in caller order; their order is the result's identity order.
    pub constraints: &'a [Constraint<'a>],
    /// Absolute height tolerance in metres for merged points and crossings.
    pub z_tolerance: f64,
    /// Optional bounded surface-preserving refinement; `None` retains the
    /// original triangulation and its pinned ordering exactly.
    pub refinement: Option<Refinement>,
}

/// One input constraint segment: `segment` joins the constraint's vertex
/// `segment` to the next (the last ring segment closes back to the first).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct SegmentRef {
    /// Index into [`Request::constraints`].
    pub constraint: u32,
    /// Segment index within that constraint.
    pub segment: u32,
}

/// A vertex created where two constraint segments cross.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Crossing {
    /// Output vertex (an existing vertex when the crossing lands on one).
    pub vertex: u32,
    /// The two segments, ascending.
    pub segments: [SegmentRef; 2],
}

/// One triangle edge that carries an input constraint segment.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ConstraintEdge {
    /// The input segment this edge is part of.
    pub segment: SegmentRef,
    /// Output vertices in the segment's direction.
    pub vertices: [u32; 2],
    /// Whether the edge borders a kept triangle. Boundary/hole edges always
    /// do; breakline edges outside the domain do not.
    pub kept: bool,
}

/// A triangulated terrain and its mapping back to the request.
#[derive(Clone, Debug, PartialEq)]
pub struct Tin {
    /// Flat xyz output vertices: the distinct input points in first-occurrence
    /// order, then crossing vertices in [`Tin::crossings`] order, then Steiner
    /// vertices in [`Tin::steiner`] insertion order.
    pub points: Vec<f64>,
    /// Kept triangles, three output vertex indices each (see the module docs).
    pub triangles: Vec<u32>,
    /// Per kept triangle, the boundary constraint bounding its region; `None`
    /// when the request has no boundary and the domain is the convex hull.
    pub face_boundary: Vec<Option<u32>>,
    /// Per input point, its output vertex.
    pub input_vertices: Vec<u32>,
    /// Crossing vertices, ordered by their segment pair.
    pub crossings: Vec<Crossing>,
    /// Constraint edges ordered by segment, then along the segment. An edge
    /// shared by several segments has one row per segment.
    pub constraint_edges: Vec<ConstraintEdge>,
    /// Refinement vertices in insertion order, after inputs and crossings.
    /// Empty when refinement is disabled.
    pub steiner: Vec<Steiner>,
}

/// Request refusals and the terrain triangulation failure codes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// Points not whole xyz triples, a constraint too short, or a count that
    /// does not fit 32-bit indices.
    Shape,
    /// A coordinate is NaN or infinite.
    NonFinite,
    /// A constraint names a point that does not exist.
    Index,
    /// `z_tolerance` is not finite and non-negative.
    Tolerance,
    /// A coordinate is outside the range `spade` accepts.
    Coordinate,
    /// A constraint could not be inserted (an unresolved crossing, or crossing
    /// constraints whose heights disagree).
    TriangulationConstraint {
        /// The constraint that could not be inserted.
        constraint: u32,
    },
    /// Fewer than three distinct or only collinear points, conflicting heights
    /// at one position, a zero-length segment, or no kept triangle.
    TriangulationDegenerate,
    /// A constraint segment is not a path of triangle edges in the result.
    BreaklineUnenforced {
        /// The constraint whose path is broken.
        constraint: u32,
    },
    /// Rings cross, nest inconsistently or leave a ring edge without exactly
    /// one kept side.
    HoleClassificationFailed,
    /// The property line is malformed, not simple, or not covered by the mesh.
    PropertyLineOutside,
    /// The property line has an arc segment; only straight segments split.
    PropertyLineArcUnsupported,
    /// Invalid angle/area or an unrepresentable refinement vertex budget.
    RefinementParameters,
    /// Refinement exhausted its vertex budget; no partial TIN is returned.
    RefinementLimit,
}

impl Error {
    /// Static diagnostic beginning with the stable refusal code.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::Shape => "TerrainShape: whole xyz points and long-enough constraints required",
            Self::NonFinite => "TerrainNonFinite: all coordinates must be finite",
            Self::Index => "TerrainIndex: a constraint names a missing point",
            Self::Tolerance => "TerrainTolerance: z tolerance must be finite and non-negative",
            Self::Coordinate => "TerrainCoordinate: a coordinate is outside the supported range",
            Self::TriangulationConstraint { .. } => {
                "TriangulationConstraint: a constraint crosses another and cannot be split"
            }
            Self::TriangulationDegenerate => {
                "TriangulationDegenerate: degenerate points, heights or segments"
            }
            Self::BreaklineUnenforced { .. } => {
                "BreaklineUnenforced: a constraint is not a path of triangle edges"
            }
            Self::HoleClassificationFailed => {
                "HoleClassificationFailed: boundary and hole rings cannot be resolved"
            }
            Self::PropertyLineOutside => {
                "PropertyLineOutside: property line must be a simple ring covered by the mesh"
            }
            Self::PropertyLineArcUnsupported => {
                "PropertyLineArcUnsupported: property line requires straight segments"
            }
            Self::RefinementParameters => {
                "RefinementParameters: require angle 0..30 degrees, positive area and bounded vertices"
            }
            Self::RefinementLimit => "RefinementLimit: additional vertex budget exhausted",
        }
    }
}

/// Triangulate a terrain request.
///
/// O((n + c) log n) expected before optional refinement, plus a bounding-box
/// sweep over constraint segment pairs. Refinement fixes the original edges
/// and bounds added vertices, not latency. Allocates; worker-classified.
///
/// # Errors
/// See [`Error`]; nothing partial is returned.
pub fn triangulate(request: &Request<'_>) -> Result<Tin, Error> {
    let mut prepared = input::prepare(request)?;
    let split = presplit::presplit(&mut prepared)?;
    let built = constrain::constrain(&prepared, &split)?;
    let (built, steiner) = if let Some(parameters) = request.refinement {
        refine::refine(&mut prepared, built, parameters)?
    } else {
        (built, Vec::new())
    };
    // The topology snapshot is no longer needed during classification/output.
    drop(built.cdt);
    let mut faces: Vec<[u32; 3]> = built.faces.into_iter().map(canonical).collect();
    faces.sort_unstable();
    let classified = classify::classify(&prepared, &faces, &built.rows)?;
    let mut triangles = Vec::new();
    let mut face_boundary = Vec::new();
    for ((face, keep), boundary) in faces.iter().zip(&classified.kept).zip(&classified.boundary) {
        if *keep {
            triangles.extend_from_slice(face);
            face_boundary.push(*boundary);
        }
    }
    if triangles.is_empty() {
        return Err(Error::TriangulationDegenerate);
    }
    let constraint_edges = built
        .rows
        .into_iter()
        .map(|row| ConstraintEdge {
            kept: classified.edge_kept(row.vertices),
            ..row
        })
        .collect();
    // Authoritative kept flags/owners are now copied into output rows. Do not
    // overlap the private incidence index with the independent verifier's sets.
    drop(classified);
    drop(faces);
    let tin = Tin {
        points: prepared.positions.iter().flatten().copied().collect(),
        triangles,
        face_boundary,
        input_vertices: prepared.input_vertices,
        crossings: split.crossings,
        constraint_edges,
        steiner,
    };
    verify_constraints(request, &tin)?;
    Ok(tin)
}

/// Rotate a counter-clockwise triple to start at its lowest vertex.
fn canonical([a, b, c]: [u32; 3]) -> [u32; 3] {
    if a <= b && a <= c {
        [a, b, c]
    } else if b <= c {
        [b, c, a]
    } else {
        [c, a, b]
    }
}
