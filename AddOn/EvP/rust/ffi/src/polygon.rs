//! Complete attributed contour ingress, checked before atomic coordinate copying.

use crate::borrow::{BorrowContext, SliceView};
use crate::boundary::{Failure, call, memory_range, overlaps};
use crate::identity::ProvenanceRequest;
use tapioca_geometry::polygon::{self, PolygonView};

/// Single stored contour plus its complete retained schema-v1 attribution.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AttributedPolygonRequest {
    /// Call-scoped flat xyz world-metre vertices, no implicit welding/repair.
    pub points: SliceView<f64>,
    /// Immutable provenance; actual counts must be N vertices/N edges/one face
    /// (all zero for empty), exactly domains mask 7, no patches.
    pub provenance: ProvenanceRequest,
}

// SAFETY: foreign descriptors and pools are live immutable initialized storage;
// output is live aligned exclusive writable capacity. Shape checks cannot prove
// allocation validity. No foreign borrow survives the returned temporary.
unsafe fn prepare(
    input: *const AttributedPolygonRequest,
    output: *mut f64,
    scalar_capacity: usize,
    error_range: Option<(usize, usize)>,
) -> Result<Vec<f64>, Failure> {
    let descriptor = memory_range(input, 1);
    let target = memory_range(output.cast_const(), scalar_capacity);
    for range in [&descriptor, &target].into_iter().flatten() {
        if overlaps(*range, error_range) {
            return Err(Failure::Overlap);
        }
    }
    let descriptor = descriptor?;
    let target = target?;
    if overlaps(descriptor, target) {
        return Err(Failure::Overlap);
    }
    let context = BorrowContext {
        error_range,
        output_range: target,
    };
    // SAFETY: descriptor shape/disjointness checked; POD copied, not retained.
    let input = unsafe { context.record(input)? };
    let table = input.provenance.table;
    // Scan every available nested range for writable aliasing before reporting a
    // malformed peer. Otherwise diagnostics could overwrite a later live pool.
    let pools = [
        memory_range(input.points.data, input.points.count),
        memory_range(table.sources.data, table.sources.count),
        memory_range(table.operations.data, table.operations.count),
        memory_range(table.objects.data, table.objects.count),
        memory_range(table.bindings.data, table.bindings.count),
        memory_range(table.binding_refs.data, table.binding_refs.count),
        memory_range(table.source_ids.data, table.source_ids.count),
        memory_range(table.operation_ids.data, table.operation_ids.count),
        memory_range(table.predecessors.data, table.predecessors.count),
        memory_range(table.mappings.data, table.mappings.count),
        memory_range(table.role_bytes.data, table.role_bytes.count),
        memory_range(table.support_tables.data, table.support_tables.count),
    ];
    for range in pools.iter().flatten() {
        if overlaps(*range, error_range) || overlaps(*range, target) {
            return Err(Failure::Overlap);
        }
    }
    for range in pools {
        range?;
    }
    if scalar_capacity < input.points.count {
        return Err(Failure::Argument("PolygonOutputTooSmall"));
    }
    // SAFETY: all input/pool shapes and writable disjointness checked; borrows
    // remain capped by context and finish before output publication.
    let (points, provenance) = unsafe {
        (
            context.slice(input.points)?,
            context.provenance(&input.provenance)?,
        )
    };
    polygon::copy_attributed_polygon(&PolygonView {
        points,
        provenance: &provenance,
    })
    .map_err(|error| match error {
        polygon::Error::Provenance(_) => Failure::State(error.message()),
        _ => Failure::Argument(error.message()),
    })
}

/// Round-trip a contour only after complete value and attribution validation.
/// No provenance is generated, rewritten or returned via newly allocated tables:
/// caller retains the exact immutable input table for the unchanged entity order.
/// Only the input scalar count prefix publishes; refusals preserve all output.
///
/// # Safety
/// Descriptor and every point/provenance pool must be live initialized immutable
/// allocations for the call. Entire output scalar capacity and error storage
/// must be live aligned exclusively writable, mutually disjoint and disjoint
/// from ALL descriptor/pool storage. Null arrays require zero count/capacity.
/// Shared unwind envelope does not recover OOM/abort/invalid foreign memory/UB.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_copy_attributed_polygon(
    input: *const AttributedPolygonRequest,
    output: *mut f64,
    scalar_capacity: usize,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope checks error shape and contains unwind; prepare preflights
    // all regions and owns complete coordinates before caller output is written.
    unsafe {
        call(error, error_capacity, || {
            let points = prepare(
                input,
                output,
                scalar_capacity,
                memory_range(error.cast_const(), error_capacity)?,
            )?;
            if !points.is_empty() {
                std::ptr::copy_nonoverlapping(points.as_ptr(), output, points.len());
            }
            Ok(())
        })
    }
}

#[cfg(test)]
mod tests;
