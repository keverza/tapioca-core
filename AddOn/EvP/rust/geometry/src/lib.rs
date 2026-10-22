//! Platform-independent geometry kernel.
//!
//! This increment provides the geometry-only gate for legacy shared-mesh
//! ingress. It borrows the existing `geomsrv::Mesh`/`Snapshot` buffers without
//! copying or repairing them, and validates the geometry arrays that algorithms
//! consume.
//!
//! Complete detached values/catalogs and authored provenance have separate opt-in
//! gates. Legacy geometry-only validation implies neither attribution nor complete
//! snapshot validation. No gate reads host state or transfers snapshot ownership.
//!
//! This crate must not depend on the host, rendering, platform, or FFI layers.

#![forbid(unsafe_code)]

// Shared external-fixture reader is also used by the opt-in private-stage test.
#[cfg(test)]
extern crate self as tapioca_geometry;

pub mod affine;
pub mod arc;
pub mod identity;
pub mod mesh;
pub mod mesh_prep;
mod native_guid;
pub mod plane;
pub mod polygon;
pub mod polyline;
pub mod terrain;
pub mod value;
