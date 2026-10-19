//! Aggregate Tapioca C ABI for version and legacy geometry ingress.
//!
//! Mesh/snapshot validators borrow the existing geometry arrays without mutation.
//! Attributed identity views remain a subsequent S1 gate; geometry-only validation
//! is not D14 certification. No geometry algorithm or host API is exposed here.

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
