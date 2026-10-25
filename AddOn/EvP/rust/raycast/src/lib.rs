//! The ray-tracing scene (raycast plan E0).
//!
//! A [`Scene`] is built once from borrowed flat arrays, owns everything it
//! needs afterwards, and is immutable and shareable across threads. It
//! implements `tapioca_geometry::ray::TriangleIndex`, so the query semantics
//! (what counts as a hit) stay in `geometry`; this crate only decides which
//! triangles a ray is tested against.
//!
//! No host, platform or FFI code belongs here.

#![forbid(unsafe_code)]

pub mod scene;

pub use scene::{Scene, SceneError, SceneInput};
