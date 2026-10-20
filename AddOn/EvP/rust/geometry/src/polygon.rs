//! S2b attributed single-contour value conversion, not polygon modelling.
//!
//! Mirrors `GeometryEngine.hpp::Polygon` as flat world-metre xyz. Caller order,
//! winding, duplicate vertices and an explicit closing vertex are retained.
//! Each stored vertex has an implicit edge to its successor (last to first),
//! including zero-length edges; the one contour has one face. No holes are inferred.

use crate::identity::{self, EntityCounts, ValidationRequest};

/// Shape/numeric ingress and structural attribution refusals.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// Whole xyz triples; either empty or at least three stored vertices.
    Shape,
    /// All coordinate values must be finite, including unused/repeated vertices.
    NonFinite,
    /// The declared complete vertex/edge/face attribution does not match the value.
    Provenance(identity::Error),
}
impl Error {
    /// Static diagnostic reused by the C boundary.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::Shape => "PolygonShape: expected empty or at least three whole xyz vertices",
            Self::NonFinite => "PolygonNonFinite: all coordinates must be finite",
            Self::Provenance(error) => error.message(),
        }
    }
}

/// Borrowed legacy contour plus its immutable, independently framed attribution.
pub struct PolygonView<'a> {
    /// Flat xyz f64 world metres, right-handed Z-up, in original stored order.
    pub points: &'a [f64],
    /// Exactly domains mask 7: vertices + implicit edges + face, no patches.
    pub provenance: &'a ValidationRequest<'a>,
}

/// Copy a fully attributed contour without evaluating or repairing geometry.
///
/// The caller retains its entire provenance table unchanged; no new operation,
/// table ID, entity ordering or identity is created. Empty contours have zero
/// counts in all domains; nonempty contours have N vertices, N edges and one
/// face. Explicit closure remains an additional vertex/edge, not silently welded.
/// Degenerate, collinear, nonplanar and self-intersecting values can round-trip;
/// successful conversion does NOT certify Boolean/offset/triangulation validity,
/// geometric contributor truth, hole roles, manifoldness or terrain suitability.
/// O(points + provenance validation) work, one coordinate allocation after all
/// validation; worker-classified until measured.
///
/// # Errors
/// Malformed/non-finite values or mismatched, missing, conflicting or stale
/// attribution refuse without a partial owned coordinate result.
pub fn copy_attributed_polygon(input: &PolygonView<'_>) -> Result<Vec<f64>, Error> {
    let (vertices, remainder) = input.points.as_chunks::<3>();
    if !remainder.is_empty() || matches!(vertices.len(), 1 | 2) {
        return Err(Error::Shape);
    }
    if !input.points.iter().all(|value| value.is_finite()) {
        return Err(Error::NonFinite);
    }
    let count = u64::try_from(vertices.len()).map_err(|_| Error::Shape)?;
    let counts = EntityCounts {
        vertices: count,
        edges: count,
        faces: u64::from(count != 0),
        patches: 0,
    };
    if input.provenance.domains != 7 || input.provenance.counts != counts {
        return Err(Error::Provenance(identity::Error::ProvenanceUnattributable));
    }
    identity::validate_provenance(input.provenance).map_err(Error::Provenance)?;
    Ok(input.points.to_vec())
}

#[cfg(test)]
mod tests;
