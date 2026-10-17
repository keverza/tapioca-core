//! C ABI static-library scaffold, not linked into the add-on.
//!
//! There are no exports or runtime behavior yet. Future exports must validate
//! inputs, contain unwinding panics, and specify allocation and borrow lifetimes.
//! See the workspace AGENTS.md before introducing an unsafe operation or export.

#![deny(unsafe_code)]
