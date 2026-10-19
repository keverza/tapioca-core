//! Aggregate Tapioca C ABI foundation, not yet linked into the add-on.
//!
//! The version export establishes the status/error and panic-containment envelope.
//! Geometry validators and attributed views are a subsequent S1 increment; no
//! geometry algorithm or host API is exposed here.

#![deny(unsafe_code)]

// All foreign-memory access is confined to this small C boundary. Geometry stays safe.
#[allow(
    unsafe_code,
    reason = "C ABI pointer validation, call-scoped writes and export attribute"
)]
mod boundary;

pub use boundary::tapioca_version;
