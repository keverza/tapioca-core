//! Structural provenance ABI. No IDs/geometry are generated or repaired here.

use crate::borrow::BorrowContext;
use crate::boundary::{Failure, call, memory_range, overlaps};
use crate::validation::{MeshView, SliceView, validation_failure};
use tapioca_geometry::{identity as id, mesh};

/// Immutable catalog/range view matching `TapiocaProvenanceView` in the C header.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ProvenanceView {
    /// Session, schema, input revision and call-local table ID.
    pub context: id::Context,
    /// Authored source catalog; values are not Modeler sidecars.
    pub sources: SliceView<id::SourceRecord>,
    /// Complete referenced operation DAG catalog.
    pub operations: SliceView<id::OperationRecord>,
    /// Object owners and their owning operations.
    pub objects: SliceView<id::ObjectRecord>,
    /// Canonical native GUID/metadata UUID/role bindings.
    pub bindings: SliceView<id::BindingRecord>,
    /// Indices into bindings, selected by each source's range.
    pub binding_refs: SliceView<u64>,
    /// Source-ID pool for sorted mapping contributor sets.
    pub source_ids: SliceView<id::SourceId>,
    /// Operation-ID pool for ordered parents and sorted mapping sets.
    pub operation_ids: SliceView<id::OperationId>,
    /// Transient support locators, not durable authored lineage.
    pub predecessors: SliceView<id::PredecessorRecord>,
    /// Complete unique mapping of the declared output domains.
    pub mappings: SliceView<id::MappingRecord>,
    /// Shared UTF-8 role bytes, selected by binding ranges.
    pub role_bytes: SliceView<u8>,
    /// Explicit retained input contexts/counts for predecessor bounds.
    pub support_tables: SliceView<id::SupportTable>,
}

/// Expected current context and geometry dimensions, separate from untrusted table metadata.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ProvenanceRequest {
    /// Current session UUID owned by the C++ session.
    pub expected_session: [u8; 16],
    /// Current document revision, including valid untouched revision zero.
    pub expected_revision: u64,
    /// Actual domain sizes, supplied by the owning primitive.
    pub counts: id::EntityCounts,
    /// Nonzero known mask bits zero through three (vertex/edge/face/patch).
    pub domains: u32,
    /// Explicit integer boolean, only zero and one accepted.
    pub require_object: u32,
    /// Immutable table catalogs and attribution rows.
    pub table: ProvenanceView,
}

impl BorrowContext {
    // SAFETY: descriptor and each pool must be live immutable foreign storage
    // for this call. All POD fields admit every bit pattern (no raw Rust enums,
    // bools or NonZero types); repr(C)/transparent layouts are tested in C++ too.
    unsafe fn provenance(
        &self,
        input: &ProvenanceRequest,
    ) -> Result<id::ValidationRequest<'_>, Failure> {
        if input.require_object > 1 {
            return Err(Failure::Argument(
                "InvalidBoolean: require_object must be 0 or 1",
            ));
        }
        let table = input.table;
        // SAFETY: each array's null/alignment/overflow/output overlap is checked
        // before borrowing; lifetime is capped by this call-local context.
        unsafe {
            Ok(id::ValidationRequest {
                expected_session: input.expected_session,
                expected_revision: input.expected_revision,
                counts: input.counts,
                domains: input.domains,
                require_object: input.require_object != 0,
                table: id::TableView {
                    context: table.context,
                    sources: self.slice(table.sources)?,
                    operations: self.slice(table.operations)?,
                    objects: self.slice(table.objects)?,
                    bindings: self.slice(table.bindings)?,
                    binding_refs: self.slice(table.binding_refs)?,
                    source_ids: self.slice(table.source_ids)?,
                    operation_ids: self.slice(table.operation_ids)?,
                    predecessors: self.slice(table.predecessors)?,
                    mappings: self.slice(table.mappings)?,
                    role_bytes: self.slice(table.role_bytes)?,
                    support_tables: self.slice(table.support_tables)?,
                },
            })
        }
    }
}

fn diagnostic(error: id::Error) -> u32 {
    match error {
        id::Error::ProvenanceUnattributable => 1,
        id::Error::ProvenanceConflict => 2,
        id::Error::ProvenanceStale => 3,
        id::Error::PatchKeyUnstable => 4,
    }
}

fn context(
    error: *mut u8,
    capacity: usize,
    diagnostic: *mut u32,
) -> Result<BorrowContext, Failure> {
    let error_range = memory_range(error.cast_const(), capacity)?;
    let output_range = memory_range(diagnostic.cast_const(), 1)?;
    if overlaps(error_range, output_range) {
        return Err(Failure::Overlap);
    }
    Ok(BorrowContext {
        error_range,
        output_range,
    })
}

// SAFETY: diagnostic points to validated caller-owned writable storage; every
// immutable foreign borrow has ended before this function writes. No input/error
// storage aliases the diagnostic. The error message is a static Rust string.
unsafe fn complete(result: Result<(), id::Error>, output: *mut u32) -> Result<(), Failure> {
    let code = result.as_ref().err().copied().map_or(0, diagnostic);
    // SAFETY: caller guarantees a live exclusive u32 allocation; shape/overlap
    // were checked before borrowing and all views are dropped by this point.
    unsafe { output.write(code) };
    result.map_err(|error| Failure::State(error.message()))
}

// SAFETY: same live disjoint foreign allocation contract as both exports.
// The operation must finish/drop all immutable input borrows before returning.
unsafe fn validate_call(
    output: *mut u32,
    error: *mut u8,
    capacity: usize,
    operation: impl FnOnce(&BorrowContext) -> Result<Result<(), id::Error>, Failure>,
) -> i32 {
    // SAFETY: the envelope checks writable error shape and catches panics before
    // diagnostic publication. Operation checks every foreign input via context.
    unsafe {
        call(error, capacity, || {
            let result = {
                let context = context(error, capacity, output)?;
                operation(&context)?
            };
            complete(result, output)
        })
    }
}

/// Validate declared structural attribution without changing any input.
///
/// Returns status 2 and diagnostic 1..4 for structural refusal, status 0 and
/// diagnostic 0 on success. Transport/shape errors and panics leave diagnostic
/// unchanged. No persisted identity allocation or geometric contributor inference.
///
/// # Safety
/// All input descriptors/arrays must be initialized, live and immutable for the
/// call. `output` is one live aligned exclusively writable u32; nonempty `error`
/// is live writable storage. Neither writable region may overlap any input or
/// each other. Null/alignment/size checks cannot prove memory allocation validity.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_validate_provenance(
    input: *const ProvenanceRequest,
    output: *mut u32,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: call contains unwinding panics and validates error storage; the
    // caller supplies actual immutable allocations and exclusive output storage.
    unsafe {
        validate_call(output, error, capacity, |context| {
            let input = context.record(input)?;
            let request = context.provenance(&input)?;
            Ok(id::validate_provenance(&request))
        })
    }
}

/// Validate legacy geometry plus vertex/face structural attribution.
///
/// The supported domains are exactly vertex + face (mask 5); other counts must
/// be zero and vertex/face counts must match actual mesh arrays. Shape errors
/// leave diagnostic unchanged; structural count mismatch is diagnostic 1.
///
/// # Safety
/// Same foreign-memory contract as `tapioca_validate_provenance`, additionally
/// covering the mesh descriptor and all coordinate/normal/triangle/sidecar arrays.
/// Output and error storage must not overlap any of those immutable allocations.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_validate_attributed_mesh(
    mesh: *const MeshView,
    input: *const ProvenanceRequest,
    output: *mut u32,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: both outputs and all foreign inputs obey the documented call scope.
    unsafe {
        validate_call(output, error, capacity, |context| {
            let mesh = context.mesh(context.record(mesh)?)?;
            mesh::validate_mesh(&mesh).map_err(validation_failure)?;
            let input = context.record(input)?;
            let request = context.provenance(&input)?;
            let vertices = u64::try_from(mesh.vertices.len() / 3)
                .map_err(|_| Failure::Argument("MeshArrayLength: vertex count exceeds u64"))?;
            let faces = u64::try_from(mesh.triangles.len() / 3)
                .map_err(|_| Failure::Argument("MeshArrayLength: face count exceeds u64"))?;
            if request.domains != 5
                || request.counts.edges != 0
                || request.counts.patches != 0
                || request.counts.vertices != vertices
                || request.counts.faces != faces
            {
                Ok(Err(id::Error::ProvenanceUnattributable))
            } else {
                Ok(id::validate_provenance(&request))
            }
        })
    }
}

#[cfg(test)]
mod tests;
