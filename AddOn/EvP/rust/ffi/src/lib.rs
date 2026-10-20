//! Aggregate Tapioca C ABI for version, geometry ingress and structural attribution.
//!
//! Mesh/snapshot validators borrow the existing geometry arrays without mutation.
//! Separate provenance views validate declared identity relationships; geometry-only
//! validation is not D14 certification. S2b affine/polyline parity primitives
//! expose no host API and do not switch a production geometry consumer.

#![deny(unsafe_code)]

// All foreign-memory access is confined to this small C boundary. Geometry stays safe.
#[allow(
    unsafe_code,
    reason = "C ABI pointer validation, call-scoped writes and export attribute"
)]
mod boundary;

pub use boundary::tapioca_version;

#[allow(
    unsafe_code,
    reason = "Call-scoped validation views borrow checked foreign arrays only at the C boundary"
)]
mod validation;

pub use validation::{
    MeshView, SliceView, SnapshotView, tapioca_validate_mesh, tapioca_validate_snapshot,
};

#[allow(
    unsafe_code,
    reason = "Central checked call-scoped foreign borrow boundary shared by validation exports"
)]
mod borrow;

#[allow(
    unsafe_code,
    reason = "Provenance POD ingress and checked nonoverlapping diagnostic output at the C boundary"
)]
mod identity;

pub use identity::{
    ProvenanceRequest, ProvenanceView, tapioca_validate_attributed_mesh,
    tapioca_validate_provenance,
};

#[allow(
    unsafe_code,
    reason = "Complete detached-value POD ingress uses shared checked call-scoped foreign borrows"
)]
mod value;
pub use value::{
    MaterialIdentity, MeshValueView, SnapshotValueRequest, SnapshotValueView, SurfaceAttribute,
    tapioca_validate_mesh_value, tapioca_validate_snapshot_value,
};

#[allow(
    unsafe_code,
    reason = "Affine POD ingress and checked disjoint caller-owned coordinate/orientation publication"
)]
mod affine;
pub use affine::{
    AffineOutput, AffineRequest, tapioca_compose_transforms, tapioca_transform_vectors,
};

#[allow(
    unsafe_code,
    reason = "Polyline POD ingress and checked disjoint caller-owned measurement/sample publication"
)]
mod polyline;
pub use polyline::{PolylineOutput, PolylineRequest, tapioca_sample_polyline};
