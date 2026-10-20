//! Bounded segment-count division, borrowing input only until atomic xyz publication.

use crate::borrow::{BorrowContext, SliceView};
use crate::boundary::{Failure, call, memory_range, overlaps};
use tapioca_geometry::polyline;

/// Immutable xyz input and explicit segment-count/end-inclusion policy.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct DividePolylineRequest {
    /// Flat xyz world metres, at least two finite vertices.
    pub points: SliceView<f64>,
    /// Existing C++ segment bound 1..=100000, not number of result points.
    pub count: i32,
    /// Only 0/1 are valid; no native bool crosses the boundary.
    pub include_ends: u32,
}

// SAFETY: descriptor/pool are live initialized immutable storage, output/error
// are live aligned exclusive writable storage. Range checks do not prove foreign
// allocation validity; all borrows finish before returning the owned temporary.
unsafe fn prepare(
    input: *const DividePolylineRequest,
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
    let known_target = target.as_ref().ok().copied().flatten();
    if overlaps(descriptor, known_target) {
        return Err(Failure::Overlap);
    }
    let context = BorrowContext {
        error_range,
        output_range: known_target,
    };
    // SAFETY: descriptor shape/writable disjointness checked before its POD read.
    let request = unsafe { context.record(input)? };
    let points = memory_range(request.points.data, request.points.count);
    if let Ok(range) = &points
        && (overlaps(*range, error_range) || overlaps(*range, known_target))
    {
        return Err(Failure::Overlap);
    }
    // A known nested alias wins even when the output shape is malformed.
    target?;
    points?;
    let include_ends = match request.include_ends {
        0 => false,
        1 => true,
        _ => {
            return Err(Failure::Argument(
                "PolylineDivideIncludeEnds: expected 0 or 1",
            ));
        }
    };
    let count = u32::try_from(request.count)
        .map_err(|_| Failure::Argument(polyline::Error::DivideCount.message()))?;
    let samples = polyline::division_point_count(count, include_ends)
        .map_err(|error| Failure::Argument(error.message()))?;
    let scalars = samples
        .checked_mul(3)
        .ok_or(Failure::Argument("PolylineOutputOverflow"))?;
    if scalar_capacity < scalars {
        return Err(Failure::Argument("PolylineOutputTooSmall"));
    }
    // SAFETY: complete input/output/error range preflight has finished; immutable
    // input is call-local and no reference survives the owned result computation.
    let values = unsafe { context.slice(request.points)? };
    polyline::divide_polyline(values, count, include_ends)
        .map_err(|error| Failure::Argument(error.message()))
}

/// Divide one polyline into equal arc-length segments; publish only xyz prefix.
///
/// # Safety
/// Descriptor/xyz storage must be live initialized immutable allocations. Entire
/// output capacity and error must be live aligned exclusive writable, mutually
/// disjoint and disjoint from ALL input storage. Null arrays require zero count
/// or capacity. Output remains unchanged on refusal/contained unwind. No foreign
/// allocation validity, OOM/abort or UB recovery is claimed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_divide_polyline(
    input: *const DividePolylineRequest,
    output: *mut f64,
    scalar_capacity: usize,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: shared error/unwind envelope; preparation preflights all regions
    // and owns complete finite coordinates before publishing any caller output.
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
