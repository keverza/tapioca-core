//! Checked neutral plane ingress and atomic frame/projected-point publication.

use crate::borrow::{BorrowContext, SliceView};
use crate::boundary::{Failure, call, memory_range, overlaps};
use tapioca_geometry::plane;

/// Borrowed flat point batch and by-value source plane.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PlaneRequest {
    /// World-metre origin and dimensionless normal/authored tangent, unchanged.
    pub plane: plane::Plane,
    /// Call-scoped flat xyz points. Empty means frame and plane round-trip only.
    pub points: SliceView<f64>,
}

/// Two disjoint caller-owned outputs, unchanged on refusal/contained panic.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PlaneOutput {
    /// Required exclusive writable storage for one complete frame/source-plane record.
    pub frame: *mut plane::PlaneFrame,
    /// Exclusive writable flat xyz storage; null allowed only at zero capacity.
    pub points: *mut f64,
    /// Capacity in scalar f64s, at least the input point scalar count.
    pub scalar_capacity: usize,
}

// SAFETY: caller guarantees live initialized descriptors/point inputs and live
// writable outputs. Shape checks cannot prove foreign allocation validity.
unsafe fn prepare(
    input: *const PlaneRequest,
    output: *const PlaneOutput,
    error_range: Option<(usize, usize)>,
) -> Result<(plane::PlaneProjection, PlaneOutput), Failure> {
    let context = BorrowContext {
        error_range,
        output_range: None,
    };
    let input_range = memory_range(input, 1);
    let output_range = memory_range(output, 1);
    for range in [&input_range, &output_range].into_iter().flatten() {
        if overlaps(*range, error_range) {
            return Err(Failure::Overlap);
        }
    }
    let input_range = input_range?;
    let output_range = output_range?;
    // SAFETY: descriptor ranges/error overlap validated; reads copy only POD.
    let (request, target) = unsafe { (context.record(input)?, context.record(output)?) };
    let points = memory_range(request.points.data, request.points.count);
    let frame = memory_range(target.frame.cast_const(), 1);
    let samples = memory_range(target.points.cast_const(), target.scalar_capacity);
    for range in [&points, &frame, &samples].into_iter().flatten() {
        if overlaps(*range, error_range) {
            return Err(Failure::Overlap);
        }
    }
    let inputs = [input_range, output_range, points?];
    let frame = frame?;
    let samples = samples?;
    if overlaps(frame, samples)
        || [frame, samples]
            .iter()
            .any(|&range| inputs.iter().any(|&input| overlaps(range, input)))
    {
        return Err(Failure::Overlap);
    }
    if target.scalar_capacity < request.points.count {
        return Err(Failure::Argument("PlaneOutputTooSmall"));
    }
    // SAFETY: all ranges checked and disjoint from writable outputs/error; point
    // slice remains immutable and no borrow survives this operation.
    let result = plane::project_to_plane(&request.plane, unsafe { context.slice(request.points)? })
        .map_err(|error| Failure::Argument(error.message()))?;
    Ok((result, target))
}

/// Derive source-preserving plane frame and project a batch. Writes only the
/// requested point prefix, together with one complete frame after full success.
///
/// # Safety
/// Descriptors/point inputs must be live initialized immutable allocations.
/// Output frame and full declared point capacity must be live aligned exclusive
/// writable allocations, disjoint from each other, ALL inputs/descriptors/error.
/// Null point arrays require zero count/capacity; frame remains required. Shared
/// panic envelope does not recover OOM/abort or invalid foreign memory/UB.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_project_to_plane(
    input: *const PlaneRequest,
    output: *const PlaneOutput,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: envelope checks error shape and contains unwind; prepare validates
    // all foreign ranges before a complete temporary is published.
    unsafe {
        call(error, capacity, || {
            let (result, target) =
                prepare(input, output, memory_range(error.cast_const(), capacity)?)?;
            // SAFETY: validated disjoint capacities contain both complete outputs.
            // No immutable foreign borrow remains; raw writes allow uninitialized output.
            if !result.points.is_empty() {
                std::ptr::copy_nonoverlapping(
                    result.points.as_ptr(),
                    target.points,
                    result.points.len(),
                );
            }
            target.frame.write(result.frame);
            Ok(())
        })
    }
}

#[cfg(test)]
mod tests;
