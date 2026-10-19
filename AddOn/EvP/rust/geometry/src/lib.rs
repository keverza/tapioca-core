//! Platform-independent geometry kernel.
//!
//! This increment provides the geometry-only gate for legacy shared-mesh
//! ingress. It borrows the existing `geomsrv::Mesh`/`Snapshot` buffers without
//! copying or repairing them, and validates the geometry arrays that algorithms
//! consume.
//!
//! Material and surface catalogs and whole-snapshot round-tripping remain outside
//! this increment. Authored provenance has its own structural validation gate;
//! legacy geometry-only validation does not imply attribution. Catalog fields
//! are deliberately not modeled here, so a successful snapshot validation is not
//! a claim that a complete `geomsrv::Snapshot` was validated.
//!
//! This crate must not depend on the host, rendering, platform, or FFI layers.

#![forbid(unsafe_code)]

pub mod identity;
pub mod mesh;
