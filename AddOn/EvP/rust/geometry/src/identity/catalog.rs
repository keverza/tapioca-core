//! Frozen schema-v1 provenance records and borrowed table views.
//!
//! These types mirror the C ABI records exactly. Every plain-old-data record is
//! `repr(C)`, copyable and free of Rust enums, `bool` fields, references and
//! containers. Unknown numeric discriminants remain representable so validation
//! can reject them without first creating an invalid Rust value.
//!
//! A [`TableView`] borrows immutable caller-owned arrays for one validation
//! call. It is a Rust view, not a foreign-memory type, and performs no repair or
//! ownership transfer.

/// Output domain discriminator for vertex attribution.
pub const DOMAIN_VERTEX: u32 = 1;
/// Output domain discriminator for edge attribution.
pub const DOMAIN_EDGE: u32 = 2;
/// Output domain discriminator for face attribution.
pub const DOMAIN_FACE: u32 = 3;
/// Output domain discriminator for patch attribution.
pub const DOMAIN_PATCH: u32 = 4;

/// Mapping kind for an unchanged entity with one exact predecessor.
pub const KIND_EXACT: u32 = 1;
/// Mapping kind for an entity that inherits one or more predecessors.
pub const KIND_INHERITED: u32 = 2;
/// Mapping kind for a fragment split from one or more predecessors.
pub const KIND_SPLIT: u32 = 3;
/// Mapping kind for an entity merged from two or more distinct predecessors.
pub const KIND_MERGED: u32 = 4;
/// Mapping kind for generated geometry that may have no predecessor.
pub const KIND_GENERATED: u32 = 5;

/// Logical binding discriminator for source IDs.
pub const LOGICAL_SOURCE: u32 = 1;
/// Logical binding discriminator for operation IDs.
pub const LOGICAL_OPERATION: u32 = 2;
/// Logical binding discriminator for object IDs.
pub const LOGICAL_OBJECT: u32 = 3;

/// Authored source identifier.
///
/// All bit patterns, including zero, are representable so that foreign records
/// can be validated. Zero is reserved for absent and is rejected where a source
/// identity is required.
#[repr(transparent)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct SourceId(
    /// Raw unsigned 64-bit identifier; zero means absent until validated.
    pub u64,
);

/// Authored construction-operation identifier.
///
/// All bit patterns, including zero, are representable so that foreign records
/// can be validated. Zero is reserved for absent and is rejected where an
/// operation identity is required.
#[repr(transparent)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct OperationId(
    /// Raw unsigned 64-bit identifier; zero means absent until validated.
    pub u64,
);

/// Authored hardscape-object identifier.
///
/// All bit patterns, including zero, are representable so that foreign records
/// can be validated. Zero is reserved for absent and is rejected where an object
/// identity is required.
#[repr(transparent)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct ObjectId(
    /// Raw unsigned 64-bit identifier; zero means absent until validated.
    pub u64,
);

/// `offset`/`count` range into a caller-owned flat array.
///
/// Validation checks unsigned addition and the destination length before any
/// access. A zero count is valid only when `offset` is still within the
/// destination (including its end).
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Range {
    /// Zero-based element offset.
    pub offset: u64,
    /// Number of elements in the range.
    pub count: u64,
}

/// Session and revision framing for one immutable provenance table.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Context {
    /// Schema version. Schema version 1 is the only accepted version.
    pub schema_version: u32,
    /// Persisted nonzero session UUID shared by every current record.
    pub session_uuid: [u8; 16],
    /// Input document revision; zero is valid for an untouched document.
    pub revision: u64,
    /// Call-local nonzero table identifier.
    pub table_id: u64,
}

/// Declared output-entity counts for the four attribution domains.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct EntityCounts {
    /// Number of output vertices.
    pub vertices: u64,
    /// Number of output edges.
    pub edges: u64,
    /// Number of output faces.
    pub faces: u64,
    /// Number of output patches.
    pub patches: u64,
}

/// One authored source and its native-binding references.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SourceRecord {
    /// Nonzero authored source identifier.
    pub id: SourceId,
    /// Range of indices into [`TableView::binding_refs`].
    pub bindings: Range,
}

/// One construction operation and its ordered predecessor references.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct OperationRecord {
    /// Nonzero authored operation identifier.
    pub id: OperationId,
    /// Range of indices into [`TableView::operation_ids`]. The referenced list
    /// may be in any order but must contain each predecessor once.
    pub parents: Range,
}

/// One hardscape object and its owning operation.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ObjectRecord {
    /// Nonzero authored object identifier.
    pub id: ObjectId,
    /// Nonzero identifier of the operation that owns this object.
    pub operation: OperationId,
}

/// One native or metadata identity binding.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct BindingRecord {
    /// Logical identity kind: source, operation or object.
    pub logical_kind: u32,
    /// Nonzero authored identifier of the selected logical kind.
    pub logical_id: u64,
    /// Canonical uppercase dashed `8-4-4-4-12` native GUID bytes. The field is
    /// exactly 36 bytes and is not NUL terminated.
    pub native_guid: [u8; 36],
    /// Optional metadata entity UUID bytes.
    pub metadata_uuid: [u8; 16],
    /// Metadata presence flag: `0` absent, `1` present.
    pub metadata_present: u32,
    /// Range into [`TableView::role_bytes`] containing a nonempty UTF-8 role
    /// without NUL bytes.
    pub role: Range,
    /// Input revision in which this binding was authored.
    pub revision: u64,
}

/// One output-entity attribution row.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MappingRecord {
    /// Output domain discriminator, one of the four `DOMAIN_*` constants.
    pub domain: u32,
    /// Mapping kind discriminator, one of the five `KIND_*` constants.
    pub kind: u32,
    /// Zero-based output entity index within the declared domain count.
    pub output_index: u64,
    /// Range into [`TableView::source_ids`] with strictly ascending unique IDs.
    pub sources: Range,
    /// Range into [`TableView::operation_ids`] with strictly ascending unique
    /// IDs.
    pub operations: Range,
    /// Optional nonzero owning object; zero means no object attribution.
    pub object: ObjectId,
    /// Range into [`TableView::predecessors`] with unique support locators.
    pub predecessors: Range,
    /// Nonzero token required for patch rows; optional member token with an owner.
    pub patch_key: u64,
}

/// One transient entity locator in an immutable support table.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PredecessorRecord {
    /// Nonzero call-local support-table identifier.
    pub table_id: u64,
    /// Support output domain, one of the four `DOMAIN_*` constants.
    pub domain: u32,
    /// Zero-based entity index within that support table's declared count.
    pub entity_index: u64,
}

/// One immutable imported support table.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SupportTable {
    /// Support-table context. The schema and session must match the current
    /// table, and the table ID must be nonzero and distinct from the output.
    pub context: Context,
    /// Entity counts used to bound predecessor locators.
    pub counts: EntityCounts,
    /// Historical flag: `0` current revision, `1` older revision.
    pub historical: u32,
}

/// Borrowed immutable arrays that make up one provenance table.
///
/// Every array is call-scoped caller memory. Validation never changes, clones or
/// repairs it. Flat arrays support zero-length views with empty slices.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TableView<'a> {
    /// Current session/revision/table context.
    pub context: Context,
    /// Authored source catalog in caller order.
    pub sources: &'a [SourceRecord],
    /// Authored operation catalog in caller order.
    pub operations: &'a [OperationRecord],
    /// Authored object catalog in caller order.
    pub objects: &'a [ObjectRecord],
    /// Complete binding catalog in caller order.
    pub bindings: &'a [BindingRecord],
    /// Flat pool of binding indices referenced by source records.
    pub binding_refs: &'a [u64],
    /// Flat pool of source IDs referenced by mapping rows.
    pub source_ids: &'a [SourceId],
    /// Flat pool of operation IDs referenced by mapping rows and operation
    /// parent ranges.
    pub operation_ids: &'a [OperationId],
    /// Complete transient support locator catalog in caller order.
    pub predecessors: &'a [PredecessorRecord],
    /// Output-entity mapping rows in caller order.
    pub mappings: &'a [MappingRecord],
    /// Flat role UTF-8 byte storage referenced by bindings.
    pub role_bytes: &'a [u8],
    /// Immutable imported support tables.
    pub support_tables: &'a [SupportTable],
}

/// Inputs for one structural provenance validation call.
///
/// The expected session and revision identify the caller's current immutable
/// view. The domain mask declares which output domains the primitive returns;
/// counts for every undeclared domain must be zero.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ValidationRequest<'a> {
    /// Caller's nonzero expected session UUID. It must match the table context.
    pub expected_session: [u8; 16],
    /// Caller's current input revision; zero is valid.
    pub expected_revision: u64,
    /// Declared output-entity counts for all four domains.
    pub counts: EntityCounts,
    /// Bitmask of declared output domains; bit `n` corresponds to domain `n+1`.
    /// The mask must be nonzero and use only bits zero through three.
    pub domains: u32,
    /// Whether every mapping row must name an owning object.
    pub require_object: bool,
    /// Immutable borrowed table.
    pub table: TableView<'a>,
}
