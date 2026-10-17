//! Platform-independent geometry kernel scaffold.
//!
//! No algorithms have been migrated. Add cohesive modules only when an operation,
//! its numerical contract, and its parity fixtures have been approved.
//!
//! This crate must not depend on the host, rendering, platform, or FFI layers.

#![forbid(unsafe_code)]
