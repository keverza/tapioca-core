//! Checked call-scoped polyline inputs and atomic caller-owned batch outputs.

use crate::borrow::{BorrowContext, SliceView};
use crate::boundary::{Failure, call, memory_range, overlaps};
use tapioca_geometry::polyline;

/// Immutable flat xyz world-metre points and finite length-fraction parameters.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PolylineRequest {
    /// At least two points; scalar count must be a multiple of three.
    pub points: SliceView<f64>,
    /// Finite length fractions, clamped to [0,1]. Empty means measure only.
    pub parameters: SliceView<f64>,
}

/// Disjoint caller-owned outputs, all unchanged on refusal/contained panic.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PolylineOutput {
    /// One metre distance per input vertex.
    pub cumulative: *mut f64,
    /// Capacity in scalar distances, at least points.count / 3.
    pub cumulative_capacity: usize,
    /// Interleaved xyz sampled positions in world metres.
    pub points: *mut f64,
    /// Interleaved xyz dimensionless sampled C++-normalized tangents.
    pub tangents: *mut f64,
    /// Scalar capacity of EACH sample buffer, at least parameters.count * 3.
    pub sample_scalar_capacity: usize,
    /// Required storage for one total world-metre length.
    pub total_length: *mut f64,
}

// SAFETY: caller provides live initialized immutable descriptors/inputs and live
// exclusive writable outputs. Shape checks cannot prove allocation validity.
unsafe fn prepare(
    input: *const PolylineRequest,
    output: *const PolylineOutput,
    error_range: Option<(usize, usize)>,
) -> Result<(polyline::PolylineSamples, PolylineOutput), Failure> {
    let context = BorrowContext {
        error_range,
        output_range: None,
    };
    // SAFETY: descriptor shape and error overlap checked before POD reads.
    let (request, target) = unsafe { (context.record(input)?, context.record(output)?) };
    let inputs = [
        memory_range(input, 1)?,
        memory_range(output, 1)?,
        memory_range(request.points.data, request.points.count)?,
        memory_range(request.parameters.data, request.parameters.count)?,
    ];
    if inputs.iter().any(|&range| overlaps(range, error_range)) {
        return Err(Failure::Overlap);
    }
    let outputs = [
        memory_range(target.cumulative.cast_const(), target.cumulative_capacity)?,
        memory_range(target.points.cast_const(), target.sample_scalar_capacity)?,
        memory_range(target.tangents.cast_const(), target.sample_scalar_capacity)?,
        memory_range(target.total_length.cast_const(), 1)?,
    ];
    for (index, &range) in outputs.iter().enumerate() {
        if overlaps(range, error_range)
            || inputs.iter().any(|&input| overlaps(range, input))
            || outputs
                .iter()
                .skip(index + 1)
                .any(|&other| overlaps(range, other))
        {
            return Err(Failure::Overlap);
        }
    }
    let samples = request
        .parameters
        .count
        .checked_mul(3)
        .ok_or(Failure::Argument("PolylineOutputOverflow"))?;
    if target.cumulative_capacity < request.points.count / 3
        || target.sample_scalar_capacity < samples
    {
        return Err(Failure::Argument("PolylineOutputTooSmall"));
    }
    // SAFETY: ranges are checked and disjoint from all writable regions; no
    // borrow escapes this call. Inputs may overlap each other (immutable only).
    let result = unsafe {
        polyline::sample_polyline(
            context.slice(request.points)?,
            context.slice(request.parameters)?,
        )
    }
    .map_err(|error| Failure::Argument(error.message()))?;
    Ok((result, target))
}

/// Measure and sample a polyline. Writes only the requested output prefixes.
/// No allocation, reference or semantic identity escapes; outputs publish only
/// after the complete finite result is computed. Empty samples still measure.
///
/// # Safety
/// Nonempty inputs/descriptors must be initialized live immutable allocations.
/// Output capacities denote live aligned exclusively writable allocations,
/// mutually disjoint and disjoint from ALL inputs/descriptors/error storage.
/// Null arrays require zero capacity/count; `total_length` is always required.
/// No foreign memory validity, OOM/abort or UB recovery is claimed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sample_polyline(
    input: *const PolylineRequest,
    output: *const PolylineOutput,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: shared envelope validates error shape and contains unwinding panics;
    // prepare checks every input/output range before any geometry borrow/write.
    unsafe {
        call(error, capacity, || {
            let (result, target) =
                prepare(input, output, memory_range(error.cast_const(), capacity)?)?;
            // SAFETY: kernel sizes are exactly vertex count and sample count*3,
            // bounded by checked capacities; no foreign immutable borrow remains.
            // Raw copies permit uninitialized f64 output without making references.
            for (source, destination) in [
                (result.cumulative.as_slice(), target.cumulative),
                (result.points.as_slice(), target.points),
                (result.tangents.as_slice(), target.tangents),
            ] {
                if !source.is_empty() {
                    std::ptr::copy_nonoverlapping(source.as_ptr(), destination, source.len());
                }
            }
            target.total_length.write(result.total_length);
            Ok(())
        })
    }
}

#[cfg(test)]
mod tests;
