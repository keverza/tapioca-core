//! Schema-v1 authored geometry identity and structural provenance validation.
//!
//! The session owner in C++ remains authoritative for authored IDs, the
//! construction DAG, native GUIDs, persistence and patch allocation. Rust
//! borrows a flat, immutable table and validates the declared structural
//! relationships before an identity-required geometry consumer may use it.
//!
//! A successful result certifies only declared structural attribution. It does
//! not prove geometric contributor truth, semantic patch matching, monotonic ID
//! allocation history, or that a support-table locator still describes the same
//! historical entity. Validation never repairs or rewrites its inputs.
//!
//! All plain-old-data records are `repr(C)` with explicit field order and fixed
//! integer/byte arrays. Unknown discriminants are rejected by validation rather
//! than represented as invalid Rust enum or `bool` values.

mod catalog;
mod mapping;

#[cfg(test)]
mod tests;

pub use catalog::{
    BindingRecord, Context, DOMAIN_EDGE, DOMAIN_FACE, DOMAIN_PATCH, DOMAIN_VERTEX, EntityCounts,
    KIND_EXACT, KIND_GENERATED, KIND_INHERITED, KIND_MERGED, KIND_SPLIT, LOGICAL_OBJECT,
    LOGICAL_OPERATION, LOGICAL_SOURCE, MappingRecord, ObjectId, ObjectRecord, OperationId,
    OperationRecord, PredecessorRecord, Range, SourceId, SourceRecord, SupportTable, TableView,
    ValidationRequest,
};
pub use mapping::{Error, validate_provenance};
