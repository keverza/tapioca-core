//! Complete detached legacy values: retained bounds and capture catalogs.
//!
//! This is an additive value gate layered on the geometry-only legacy ingress.
//! It does not replace or tighten [`crate::mesh::validate_mesh`] or
//! [`crate::mesh::validate_snapshot`]; callers that only need geometry semantics
//! keep using those functions. A successful result here certifies the declared
//! bounds, catalog records and catalog references only. It is not authored
//! identity/D14 certification, not a current-host-state read, and not a claim
//! that the C++ publication gates can be skipped.
//!
//! The C++ `geomsrv::Mesh`/`geomsrv::Snapshot` types remain authoritative. Views
//! borrow their flat arrays and text for the call; no record is copied, repaired,
//! normalized, sorted or retained. Catalog order is preserved and irrelevant.
//!
//! Bounds are world-space metres in the existing right-handed Z-up frame. They
//! are checked inclusively against every position vertex, with no epsilon. An
//! empty vertex array may carry either the canonical empty envelope
//! (`mn = +inf`, `mx = -inf`) or any finite ordered envelope; mixed or NaN
//! sentinels are malformed. Nonempty vertices require a finite ordered envelope
//! that contains all vertices, including unused vertices and meshes without
//! triangles.
//!
//! Material pool indices are capture-local `ModelerAPI` pool indices, not
//! project surface indices. Triangle material indices must be zero or positive;
//! zero is the real no-material/unknown value and is exempt from strict catalog
//! lookup. Negative triangle material indices are malformed. A material
//! identity's `external_reference` is an opaque signed pool identity: every
//! `i32`, including zero and negative values, is legitimate and is never required
//! to resolve in the surface catalog or to equal `tri_surface_attribute`.
//! `tri_surface_attribute == 0` stays unknown and is never inferred through the
//! material pool.
//!
//! Catalog keys are unique within each catalog and positive. Transparency is a
//! finite fraction in `0..=1`, but is intentionally sparse because per-material
//! extraction can fail; a missing transparency row remains unknown. When
//! catalogs are required, only referenced positive material-pool keys need an
//! identity row and referenced positive surface attributes need a surface row.
//! Transparency rows are never required. Names are already UTF-8 `&str`, may be
//! empty, need not be unique, and are never altered. A surface GUID must be the
//! canonical uppercase dashed non-nil 36-byte native form, matching the identity
//! gate's GUID rules. Supplying no catalog is allowed; supplying a malformed row,
//! including an invalid transparency row, is not, even when that row is
//! unreferenced.

use std::collections::BTreeSet;
use std::fmt;

use crate::mesh::{self, MeshView};

const WIRE_EDGE_MASK: u8 = 0b111;

/// Axis-aligned world-space bounds in metres, `Aabb.hpp` layout.
///
/// `mn` and `mx` are inclusive. The canonical empty envelope is all `+inf` in
/// `mn` and all `-inf` in `mx`. Values are retained exactly; this type does not
/// repair or canonicalize them.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Aabb {
    /// Inclusive minimum on x, y and z, in metres.
    pub mn: [f64; 3],
    /// Inclusive maximum on x, y and z, in metres.
    pub mx: [f64; 3],
}

/// Captured material-pool transparency, a fraction rather than a percentage.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct MaterialTransparency {
    /// Capture-local `ModelerAPI` material-pool key; positive.
    pub index: i32,
    /// Transparency fraction, finite and within `0..=1`.
    pub transparency: f64,
}

/// Captured material-pool identity with its opaque signed pool reference.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct MaterialIdentity<'a> {
    /// Positive capture-local `ModelerAPI` material-pool key.
    pub index: i32,
    /// Opaque signed pool identity. Every `i32`, including zero and negative
    /// values, is legitimate and is not a project surface lookup key.
    pub external_reference: i32,
    /// Captured material name bytes. Empty is permitted; not rewritten.
    pub name: &'a str,
}

/// Captured project surface attribute, not an authored editor identity.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SurfaceAttribute<'a> {
    /// Positive project-local surface attribute index.
    pub index: i32,
    /// Captured surface name bytes. Empty is permitted; not rewritten.
    pub name: &'a str,
    /// Canonical uppercase dashed non-nil native GUID text.
    pub guid: &'a str,
}

/// Borrowed complete mesh value, including its retained world-metre bounds.
#[derive(Clone, Copy, Debug)]
pub struct MeshValueView<'a> {
    /// Existing geometry-only mesh view, unchanged.
    pub mesh: MeshView<'a>,
    /// Retained bounds. They are checked, never recomputed or repaired.
    pub bounds: Aabb,
}

/// Borrowed complete detached snapshot value with its capture catalogs.
#[derive(Clone, Copy, Debug)]
pub struct SnapshotValueView<'a> {
    /// Retained snapshot identifier, separate from the capture epoch. Zero is
    /// valid for detached values; publication/consumer gates validate ids.
    pub id: u64,
    /// Model-watch capture epoch, not an editor document revision. Zero is valid
    /// for detached or synthetic data and is not implied by `complete_model`.
    pub capture_stamp: u64,
    /// Actual capture mode: `all` or `selection`.
    pub scope: &'a str,
    /// Whole-model completeness marker. True is only valid with scope `all`.
    pub complete_model: bool,
    /// Meshes in capture order.
    pub meshes: &'a [MeshValueView<'a>],
    /// Captured material-pool transparency rows, in any order.
    pub material_transparency: &'a [MaterialTransparency],
    /// Captured material-pool identity rows, in any order.
    pub material_identity: &'a [MaterialIdentity<'a>],
    /// Captured project surface rows, in any order.
    pub surface_attributes: &'a [SurfaceAttribute<'a>],
}

/// Explicit policy for validating one detached snapshot value.
#[derive(Clone, Copy, Debug)]
pub struct SnapshotValueRequest<'a> {
    /// Complete detached value to validate.
    pub snapshot: SnapshotValueView<'a>,
    /// Zero accepts detached or synthetic framing without a freshness check.
    /// Nonzero requires the snapshot capture epoch to equal this value; the
    /// snapshot id is not constrained here.
    pub expected_capture_stamp: u64,
    /// When true, every referenced material and surface must resolve in the
    /// supplied catalogs. Missing required entries are refused; unreferenced
    /// catalog rows are still shape-checked but need not be complete.
    pub require_catalogs: bool,
}

/// Stable detached-value validation failure categories.
///
/// Use [`Self::message`] for a static diagnostic prefix and fixed description.
/// Capture/completeness conflicts, duplicate keys/GUIDs and missing required
/// catalogs are state errors; geometry, bounds and malformed values are arguments.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// Legacy mesh geometry, sidecar length or source provenance is invalid.
    MeshGeometryInvalid,
    /// Bounds are not the canonical empty envelope, or not finite and ordered.
    BoundsInvalid,
    /// Finite ordered bounds do not contain every position vertex inclusively.
    BoundsNotContaining,
    /// A triangle wire-edge mask uses bits above the low three.
    WireEdgeInvalid,
    /// A triangle material-pool index is negative.
    MaterialIndexInvalid,
    /// A triangle surface attribute is negative.
    SurfaceAttributeInvalid,
    /// A transparency is not a finite fraction within `0..=1`.
    TransparencyInvalid,
    /// A material or surface catalog key is not positive.
    CatalogIndexInvalid,
    /// Keys are not unique within one catalog array.
    DuplicateCatalogs,
    /// A surface GUID is not canonical uppercase dashed non-nil text.
    SurfaceGuidInvalid,
    /// The capture scope is not an actual mode (`all` or `selection`).
    ScopeInvalid,
    /// `complete_model` is true with a non-whole-model scope.
    CompleteModelScope,
    /// The expected capture epoch does not match the snapshot.
    CaptureStale,
    /// A required referenced material or surface catalog entry is absent.
    MissingCatalog,
    /// Two meshes with the same nonempty GUID occur in one snapshot.
    SnapshotDuplicateGuid,
}

impl Error {
    /// Return the stable code prefix and fixed description for this error.
    ///
    /// No formatting or allocation is performed; callers may place this text in
    /// an FFI error buffer without exposing Rust formatting machinery.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::MeshGeometryInvalid => {
                "MeshGeometryInvalid: legacy mesh geometry, sidecar length or source provenance is invalid"
            }
            Self::BoundsInvalid => {
                "BoundsInvalid: bounds must be finite and ordered on all axes or the canonical empty envelope"
            }
            Self::BoundsNotContaining => {
                "BoundsNotContaining: bounds must contain every mesh vertex inclusively"
            }
            Self::WireEdgeInvalid => {
                "WireEdgeInvalid: triangle wire-edge masks must use only the low three bits"
            }
            Self::MaterialIndexInvalid => {
                "MaterialIndexInvalid: triangle material-pool indices must be zero or positive"
            }
            Self::SurfaceAttributeInvalid => {
                "SurfaceAttributeInvalid: triangle surface attributes must be zero or positive"
            }
            Self::TransparencyInvalid => {
                "TransparencyInvalid: material transparency must be a finite fraction within 0..1"
            }
            Self::CatalogIndexInvalid => {
                "CatalogIndexInvalid: material and surface catalog keys must be positive"
            }
            Self::DuplicateCatalogs => {
                "DuplicateCatalogs: catalog keys must be unique within each catalog"
            }
            Self::SurfaceGuidInvalid => {
                "SurfaceGuidInvalid: GUID must be canonical uppercase dashed non-nil 36-character text"
            }
            Self::ScopeInvalid => "ScopeInvalid: snapshot scope must be all or selection",
            Self::CompleteModelScope => "CompleteModelScope: complete_model requires scope all",
            Self::CaptureStale => {
                "CaptureStale: expected capture stamp does not match the snapshot"
            }
            Self::MissingCatalog => {
                "MissingCatalog: a referenced material or surface catalog entry is absent"
            }
            Self::SnapshotDuplicateGuid => {
                "SnapshotDuplicateGuid: nonempty mesh GUIDs must be unique within a snapshot"
            }
        }
    }

    /// Return whether this failure is a state conflict rather than malformed input.
    #[must_use]
    pub const fn is_state_error(self) -> bool {
        matches!(
            self,
            Self::CaptureStale
                | Self::DuplicateCatalogs
                | Self::SnapshotDuplicateGuid
                | Self::CompleteModelScope
                | Self::MissingCatalog
        )
    }
}

impl fmt::Display for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.message())
    }
}

/// Validate one complete borrowed mesh value, including retained bounds.
///
/// Runs the unchanged legacy geometry gate first, then checks all six bounds
/// components, inclusive containment of every position vertex, low-three-bit wire
/// masks, nonnegative material-pool indices and nonnegative surface attributes.
/// Empty geometry with the canonical empty envelope is valid. No value is
/// modified.
///
/// # Errors
///
/// Returns [`Error::MeshGeometryInvalid`] for legacy geometry failures,
/// [`Error::BoundsInvalid`] for malformed bounds, [`Error::BoundsNotContaining`]
/// when bounds exclude a vertex, [`Error::WireEdgeInvalid`] for mask bits above
/// the low three, [`Error::MaterialIndexInvalid`] for a negative triangle
/// material index, or [`Error::SurfaceAttributeInvalid`] for a negative surface
/// attribute.
pub fn validate_mesh_value(value: &MeshValueView<'_>) -> Result<(), Error> {
    mesh::validate_mesh(&value.mesh).map_err(|_| Error::MeshGeometryInvalid)?;
    validate_bounds(&value.mesh, value.bounds)?;
    if value
        .mesh
        .tri_wire_edges
        .iter()
        .any(|&mask| mask & !WIRE_EDGE_MASK != 0)
    {
        return Err(Error::WireEdgeInvalid);
    }
    if value.mesh.tri_material.iter().any(|&index| index < 0) {
        return Err(Error::MaterialIndexInvalid);
    }
    if value
        .mesh
        .tri_surface_attribute
        .iter()
        .any(|&index| index < 0)
    {
        return Err(Error::SurfaceAttributeInvalid);
    }
    Ok(())
}

/// Validate one complete detached snapshot value and its optional catalogs.
///
/// Meshes are validated in capture order; nonempty GUIDs must be unique while
/// blank synthetic GUIDs may repeat. The scope must be an actual mode, and
/// `complete_model` implies `all`. A zero `expected_capture_stamp` disables the
/// freshness check; a nonzero value requires only an equal capture epoch. The
/// snapshot id is retained framing and may remain zero for a detached value
/// because the owning consumer/publication gate validates ids separately. All
/// catalog rows are shape-checked. When `require_catalogs` is true, every
/// referenced positive material pool must have an identity row and every
/// referenced positive surface attribute must exist. Transparency is sparse and
/// is never required; opaque material external references are never resolved
/// against the surface catalog. Unreferenced rows need not complete the catalogs.
///
/// The input is borrowed and never modified. This does not validate editor
/// revisions, prove current host state, or certify D14 authored identity.
///
/// # Errors
///
/// Returns the first mesh/bounds/catalog/scope failure in the order documented
/// above. Capture/completeness conflicts, duplicate keys/GUIDs and missing required
/// catalogs are state errors; the remaining variants are malformed input.
pub fn validate_snapshot_value(request: &SnapshotValueRequest<'_>) -> Result<(), Error> {
    let snapshot = &request.snapshot;
    for mesh in snapshot.meshes {
        validate_mesh_value(mesh)?;
    }
    validate_snapshot_guids(snapshot.meshes)?;
    validate_scope(snapshot)?;
    validate_freshness(snapshot, request.expected_capture_stamp)?;
    let catalogs = validate_catalogs(snapshot)?;
    if request.require_catalogs {
        validate_references(snapshot.meshes, &catalogs)?;
    }
    Ok(())
}

fn validate_bounds(mesh: &MeshView<'_>, bounds: Aabb) -> Result<(), Error> {
    if mesh.vertices.is_empty() {
        if is_empty_envelope(bounds) || is_finite_ordered(bounds) {
            return Ok(());
        }
        return Err(Error::BoundsInvalid);
    }
    if !is_finite_ordered(bounds) {
        return Err(Error::BoundsInvalid);
    }
    if !bounds_contain(mesh.vertices, bounds) {
        return Err(Error::BoundsNotContaining);
    }
    Ok(())
}

fn is_empty_envelope(bounds: Aabb) -> bool {
    bounds
        .mn
        .iter()
        .all(|value| value.is_infinite() && value.is_sign_positive())
        && bounds
            .mx
            .iter()
            .all(|value| value.is_infinite() && value.is_sign_negative())
}

fn is_finite_ordered(bounds: Aabb) -> bool {
    bounds
        .mn
        .iter()
        .zip(bounds.mx.iter())
        .all(|(&mn, &mx)| mn.is_finite() && mx.is_finite() && mn <= mx)
}

fn bounds_contain(vertices: &[f64], bounds: Aabb) -> bool {
    vertices.as_chunks::<3>().0.iter().all(|vertex| {
        bounds
            .mn
            .iter()
            .zip(bounds.mx.iter())
            .zip(vertex.iter())
            .all(|((&mn, &mx), &value)| mn <= value && value <= mx)
    })
}

fn validate_snapshot_guids(meshes: &[MeshValueView<'_>]) -> Result<(), Error> {
    let mut guids = BTreeSet::new();
    for mesh in meshes {
        if !mesh.mesh.guid.is_empty() && !guids.insert(mesh.mesh.guid) {
            return Err(Error::SnapshotDuplicateGuid);
        }
    }
    Ok(())
}

fn validate_scope(snapshot: &SnapshotValueView<'_>) -> Result<(), Error> {
    if !matches!(snapshot.scope, "all" | "selection") {
        return Err(Error::ScopeInvalid);
    }
    if snapshot.complete_model && snapshot.scope != "all" {
        return Err(Error::CompleteModelScope);
    }
    Ok(())
}

fn validate_freshness(
    snapshot: &SnapshotValueView<'_>,
    expected_capture_stamp: u64,
) -> Result<(), Error> {
    if expected_capture_stamp == 0 {
        return Ok(());
    }
    if snapshot.capture_stamp != expected_capture_stamp {
        return Err(Error::CaptureStale);
    }
    Ok(())
}

struct Catalogs {
    identities: BTreeSet<i32>,
    surfaces: BTreeSet<i32>,
}

fn validate_catalogs(snapshot: &SnapshotValueView<'_>) -> Result<Catalogs, Error> {
    let mut transparency = BTreeSet::new();
    for entry in snapshot.material_transparency {
        if entry.index <= 0 {
            return Err(Error::CatalogIndexInvalid);
        }
        if !transparency.insert(entry.index) {
            return Err(Error::DuplicateCatalogs);
        }
        if !entry.transparency.is_finite() || entry.transparency < 0.0 || entry.transparency > 1.0 {
            return Err(Error::TransparencyInvalid);
        }
    }

    let mut identities = BTreeSet::new();
    for entry in snapshot.material_identity {
        if entry.index <= 0 {
            return Err(Error::CatalogIndexInvalid);
        }
        if !identities.insert(entry.index) {
            return Err(Error::DuplicateCatalogs);
        }
    }

    let mut surfaces = BTreeSet::new();
    for entry in snapshot.surface_attributes {
        if entry.index <= 0 {
            return Err(Error::CatalogIndexInvalid);
        }
        if !surfaces.insert(entry.index) {
            return Err(Error::DuplicateCatalogs);
        }
        if !crate::native_guid::is_canonical(entry.guid.as_bytes()) {
            return Err(Error::SurfaceGuidInvalid);
        }
    }

    Ok(Catalogs {
        identities,
        surfaces,
    })
}

fn validate_references(meshes: &[MeshValueView<'_>], catalogs: &Catalogs) -> Result<(), Error> {
    for mesh in meshes {
        for &pool in mesh.mesh.tri_material {
            if pool > 0 && !catalogs.identities.contains(&pool) {
                return Err(Error::MissingCatalog);
            }
        }
        for &surface in mesh.mesh.tri_surface_attribute {
            if surface > 0 && !catalogs.surfaces.contains(&surface) {
                return Err(Error::MissingCatalog);
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests;
