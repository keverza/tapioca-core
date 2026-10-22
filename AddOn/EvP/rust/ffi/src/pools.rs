//! Shared full-capacity and nested-pool preflight before any foreign borrow.
//!
//! Every export that takes a request descriptor, nested input pools and a
//! caller-owned output descriptor with writable arrays checks all of them here
//! first: writable ranges must be disjoint from every input, from each other and
//! from the error buffer, and overlap findings win over a malformed peer so a
//! diagnostic can never be written over valid storage.

use crate::borrow::BorrowContext;
use crate::boundary::{Failure, memory_range, overlaps};
use crate::identity::ProvenanceRequest;

/// A recognized byte range, or `None` for an empty pool.
pub(crate) type Range = Option<(usize, usize)>;
/// A pool's range, or why it is malformed.
pub(crate) type Checked = Result<Range, Failure>;

/// Refuse overlaps of writable ranges with inputs, each other or the error
/// buffer, then any malformed range.
///
/// # Errors
/// [`Failure::Overlap`] or a storage [`Failure::Argument`].
pub(crate) fn preflight(
    inputs: &[Checked],
    outputs: &[Checked],
    error: Range,
) -> Result<(), Failure> {
    if inputs
        .iter()
        .chain(outputs)
        .flatten()
        .any(|&range| overlaps(range, error))
    {
        return Err(Failure::Overlap);
    }
    for &output in outputs.iter().flatten() {
        if inputs
            .iter()
            .flatten()
            .any(|&input| overlaps(input, output))
        {
            return Err(Failure::Overlap);
        }
    }
    for (index, &left) in outputs.iter().flatten().enumerate() {
        if outputs
            .iter()
            .flatten()
            .skip(index + 1)
            .any(|&right| overlaps(left, right))
        {
            return Err(Failure::Overlap);
        }
    }
    // Overlaps of all recognized ranges win over a malformed peer.
    if inputs.iter().chain(outputs).any(Result::is_err) {
        return Err(Failure::Argument(
            "TerrainStorage: null, misaligned or overflowing range",
        ));
    }
    Ok(())
}

/// Ranges of every pool of a schema-v1 provenance table.
pub(crate) fn provenance(request: &ProvenanceRequest) -> Vec<Checked> {
    let t = request.table;
    vec![
        memory_range(t.sources.data, t.sources.count),
        memory_range(t.operations.data, t.operations.count),
        memory_range(t.objects.data, t.objects.count),
        memory_range(t.bindings.data, t.bindings.count),
        memory_range(t.binding_refs.data, t.binding_refs.count),
        memory_range(t.source_ids.data, t.source_ids.count),
        memory_range(t.operation_ids.data, t.operation_ids.count),
        memory_range(t.predecessors.data, t.predecessors.count),
        memory_range(t.mappings.data, t.mappings.count),
        memory_range(t.role_bytes.data, t.role_bytes.count),
        memory_range(t.support_tables.data, t.support_tables.count),
    ]
}

/// Copy both descriptors once their own ranges are known not to cover the
/// error buffer; an unreadable descriptor is deferred (`None`) until every peer
/// pool has been scanned, so a diagnostic cannot corrupt valid peer storage.
///
/// # Safety
/// Descriptors must be live initialized immutable POD. Inputs may alias each
/// other, but no writable count/array/error may alias either descriptor.
pub(crate) unsafe fn descriptors<I: Copy, O: Copy>(
    input: *const I,
    output: *const O,
    error: Range,
) -> Result<(Option<I>, Option<O>, BorrowContext), Failure> {
    let input_range = memory_range(input, 1);
    let output_range = memory_range(output, 1);
    if [&input_range, &output_range]
        .into_iter()
        .flatten()
        .any(|&range| overlaps(range, error))
    {
        return Err(Failure::Overlap);
    }
    let context = BorrowContext {
        error_range: error,
        output_range: None,
    };
    let request = if input_range.is_ok() {
        // SAFETY: input descriptor shape/disjointness accepted; caller owns its allocation.
        Some(unsafe { context.record(input)? })
    } else {
        None
    };
    let target = if output_range.is_ok() {
        // SAFETY: output descriptor shape/disjointness accepted; caller owns its allocation.
        Some(unsafe { context.record(output)? })
    } else {
        None
    };
    Ok((request, target, context))
}
