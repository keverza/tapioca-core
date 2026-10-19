//! Borrowed views and ingress validation for legacy shared geometry.
//!
//! The C++ `geomsrv::Mesh` and `geomsrv::Snapshot` types remain authoritative.
//! These views mirror the geometry arrays needed by Rust algorithms and retain
//! snapshot identity metadata only for framing. They intentionally omit material
//! and surface catalogs: this is a geometry-only legacy gate, not attributed S1
//! validation.
//!
//! Validation checks array shape, finite coordinates and normals, triangle index
//! bounds, optional sidecar lengths, and source-body/provenance shape. It does
//! not require manifoldness, orientation, nonzero triangle area, unique triangle
//! edges, or non-overlapping triangles. Empty meshes are valid, and no shape is
//! repaired or canonicalized.

use std::collections::BTreeSet;
use std::fmt;

/// Borrowed legacy mesh geometry, with world-space positions in metres.
///
/// The view is read-only and call-scoped. `guid` is an opaque identity string:
/// synthetic or otherwise noncanonical GUIDs are valid at this ingress, and a
/// blank GUID denotes an anonymous synthetic mesh. Snapshot validation rejects
/// duplicate nonempty GUIDs but does not canonicalize or interpret them.
///
/// Sidecars are optional. Empty means unavailable. A nonempty normals array must
/// contain exactly one xyz normal per position vertex, and each nonempty
/// per-triangle sidecar must contain exactly one entry per triangle. Source body
/// and source polygon must both be absent or both present with positive 1-based
/// values. No sidecar is generated or inferred.
#[derive(Debug, Clone, Copy)]
pub struct MeshView<'a> {
    /// Element GUID or synthetic mesh identifier. Not canonicalized.
    pub guid: &'a str,
    /// Opaque `ModelerAPI` element type. Not interpreted by geometry code.
    pub elem_type: i32,
    /// Interleaved xyz position triples in world-space metres (`f64`).
    pub vertices: &'a [f64],
    /// Optional per-vertex xyz normals. Empty, or exactly `vertices.len()`
    /// scalar values.
    pub normals: &'a [f32],
    /// Triangle corner indices into `vertices`, measured in vertices rather than
    /// scalar offsets. Length is a multiple of three.
    pub triangles: &'a [u32],
    /// Optional material pool index per triangle. Empty, or one per triangle.
    pub tri_material: &'a [i32],
    /// Optional wire-edge bitmask per triangle. Empty, or one per triangle. The
    /// legacy extractor uses the low three bits for source-face boundary edges;
    /// this gate checks only presence and length.
    pub tri_wire_edges: &'a [u8],
    /// Optional 1-based `ModelerAPI` tessellated-body index per triangle. Empty, or
    /// present together with `tri_source_pgon` for every triangle.
    pub tri_source_body: &'a [i32],
    /// Optional 1-based `MeshBody::GetPolygon` index per triangle. Empty, or
    /// present together with `tri_source_body` for every triangle.
    pub tri_source_pgon: &'a [i32],
    /// Optional project surface attribute per triangle. Empty, or one per
    /// triangle. Zero retains its legacy "unknown/generated" meaning; this gate
    /// checks length only and performs no catalog lookup.
    pub tri_surface_attribute: &'a [i32],
}

/// Borrowed legacy snapshot metadata and mesh list.
///
/// `scope`, `complete_model`, `id`, and `capture_stamp` mirror the legacy
/// snapshot framing. They are retained so callers can keep the validated meshes
/// associated with their capture, but this geometry-only gate does not validate
/// scope text, snapshot identity, capture freshness, or completeness semantics.
/// Material and surface catalogs are deliberately not part of this view.
///
/// Every mesh must pass [`validate_mesh`]. Nonempty GUIDs must be unique within
/// the snapshot; blank GUIDs are allowed and may repeat for anonymous synthetic
/// meshes. An empty mesh list is valid.
#[derive(Debug, Clone, Copy)]
pub struct SnapshotView<'a> {
    /// Legacy capture scope, such as `all` or `selection`. Not interpreted here.
    pub scope: &'a str,
    /// Legacy completeness marker. It does not relax or strengthen validation.
    pub complete_model: bool,
    /// Borrowed meshes in capture order.
    pub meshes: &'a [MeshView<'a>],
    /// Legacy snapshot identifier, retained for framing and not validated.
    pub id: u64,
    /// Legacy nonzero model-watch capture stamp, retained and not validated.
    pub capture_stamp: u64,
}

/// Geometry-ingress validation failure.
///
/// The variants are stable machine-readable categories. Use [`Self::message`]
/// for a stable code prefix and human-readable description. They do not imply
/// that material/surface catalogs or attributed bindings were validated.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ValidationError {
    /// Position or triangle array length is not a multiple of three.
    MeshArrayLength,
    /// A position or normal contains NaN or infinity.
    MeshNonFinite,
    /// A triangle corner index does not refer to an existing vertex.
    MeshIndexOutOfBounds,
    /// An optional sidecar is nonempty and has the wrong entry count. This also
    /// covers a normals array whose scalar length differs from `vertices`.
    MeshSidecarLength,
    /// Source body and source polygon are only partially present, or a present
    /// source index is not positive and 1-based.
    MeshSourceSidecarInvalid,
    /// Two meshes with the same nonempty GUID occur in one snapshot.
    SnapshotDuplicateGuid,
}

impl ValidationError {
    /// Return the stable code prefix and fixed description for this error.
    ///
    /// The result is a static string; no formatting or allocation is performed.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::MeshArrayLength => {
                "MeshArrayLength: vertices and triangles must each contain whole elements"
            }
            Self::MeshNonFinite => {
                "MeshNonFinite: vertices and normals must contain only finite values"
            }
            Self::MeshIndexOutOfBounds => {
                "MeshIndexOutOfBounds: a triangle index does not refer to an existing vertex"
            }
            Self::MeshSidecarLength => {
                "MeshSidecarLength: an optional sidecar must be empty or have one entry per vertex or triangle"
            }
            Self::MeshSourceSidecarInvalid => {
                "MeshSourceSidecarInvalid: source body and polygon must both be empty or both be full and positive"
            }
            Self::SnapshotDuplicateGuid => {
                "SnapshotDuplicateGuid: nonempty mesh GUIDs must be unique within a snapshot"
            }
        }
    }
}

impl fmt::Display for ValidationError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.message())
    }
}

/// Validate one borrowed legacy mesh.
///
/// Checks run in a stable order: array lengths, finiteness, triangle indices,
/// optional sidecar lengths, then source provenance shape and values. Every
/// index must be less than the position vertex count. Degenerate triangles and
/// unused vertices are accepted because this gate does not perform manifold or
/// area validation. Empty geometry is valid. The input is never modified.
///
/// # Errors
///
/// Returns a [`ValidationError`] when an array is not whole triples, a coordinate
/// or normal is not finite, a triangle index is out of bounds, an optional
/// sidecar has the wrong length, or source body/polygon data is inconsistent.
pub fn validate_mesh(mesh: &MeshView<'_>) -> Result<(), ValidationError> {
    if !mesh.vertices.len().is_multiple_of(3) || !mesh.triangles.len().is_multiple_of(3) {
        return Err(ValidationError::MeshArrayLength);
    }
    if mesh.vertices.iter().any(|value| !value.is_finite())
        || mesh.normals.iter().any(|value| !value.is_finite())
    {
        return Err(ValidationError::MeshNonFinite);
    }
    validate_indices(mesh)?;

    let triangle_count = mesh.triangles.len() / 3;
    validate_optional_length(mesh.normals.len(), mesh.vertices.len())?;
    validate_optional_length(mesh.tri_material.len(), triangle_count)?;
    validate_optional_length(mesh.tri_wire_edges.len(), triangle_count)?;
    validate_optional_length(mesh.tri_surface_attribute.len(), triangle_count)?;
    validate_source_sidecars(mesh, triangle_count)
}

/// Validate the meshes and nonempty identity uniqueness of one snapshot view.
///
/// Meshes are validated in capture order. Duplicate detection considers only
/// nonempty GUIDs and is deterministic; blank GUIDs may repeat. Catalog,
/// completeness, scope, and snapshot-identity semantics are outside this gate.
///
/// # Errors
///
/// Returns the first mesh [`ValidationError`] in capture order, or
/// [`ValidationError::SnapshotDuplicateGuid`] when two nonempty GUIDs match.
pub fn validate_snapshot(snapshot: &SnapshotView<'_>) -> Result<(), ValidationError> {
    for mesh in snapshot.meshes {
        validate_mesh(mesh)?;
    }
    let mut guids = BTreeSet::new();
    for mesh in snapshot.meshes {
        // Borrow GUID strings; deterministic ordered membership avoids a
        // quadratic scan on large captured scenes and does not clone identities.
        if !mesh.guid.is_empty() && !guids.insert(mesh.guid) {
            return Err(ValidationError::SnapshotDuplicateGuid);
        }
    }
    Ok(())
}

fn validate_indices(mesh: &MeshView<'_>) -> Result<(), ValidationError> {
    // `u64` avoids narrowing a 64-bit vertex count while covering every `u32`
    // index on all supported targets.
    let vertex_count = u64::try_from(mesh.vertices.len() / 3).unwrap_or(u64::MAX);
    if mesh
        .triangles
        .iter()
        .any(|&index| u64::from(index) >= vertex_count)
    {
        return Err(ValidationError::MeshIndexOutOfBounds);
    }
    Ok(())
}

fn validate_optional_length(length: usize, expected: usize) -> Result<(), ValidationError> {
    if length == 0 || length == expected {
        Ok(())
    } else {
        Err(ValidationError::MeshSidecarLength)
    }
}

fn validate_source_sidecars(
    mesh: &MeshView<'_>,
    triangle_count: usize,
) -> Result<(), ValidationError> {
    let body_present = !mesh.tri_source_body.is_empty();
    let pgon_present = !mesh.tri_source_pgon.is_empty();
    if body_present != pgon_present {
        return Err(ValidationError::MeshSourceSidecarInvalid);
    }
    validate_optional_length(mesh.tri_source_body.len(), triangle_count)?;
    validate_optional_length(mesh.tri_source_pgon.len(), triangle_count)?;
    if body_present
        && (mesh.tri_source_body.iter().any(|&value| value <= 0)
            || mesh.tri_source_pgon.iter().any(|&value| value <= 0))
    {
        return Err(ValidationError::MeshSourceSidecarInvalid);
    }
    Ok(())
}

#[cfg(test)]
mod tests;
