//! Checked arc POD ingress and transactional caller-owned xyz publication.

use crate::borrow::BorrowContext;
use crate::boundary::{Failure, call, memory_range, overlaps};
pub use tapioca_geometry::arc::ArcRequest;

// SAFETY: caller owns live initialized request and aligned writable output.
// Numeric/range checks cannot prove foreign allocation validity.
unsafe fn prepare(
    request: *const ArcRequest,
    output: *mut f64,
    scalar_capacity: usize,
    error_range: Option<(usize, usize)>,
) -> Result<Vec<f64>, Failure> {
    let input_range = memory_range(request, 1);
    let output_range = memory_range(output.cast_const(), scalar_capacity);
    // Known error overlap wins even if the other region has a malformed shape:
    // reporting that shape error must not corrupt a live input/output buffer.
    for range in [&input_range, &output_range].into_iter().flatten() {
        if overlaps(*range, error_range) {
            return Err(Failure::Overlap);
        }
    }
    let input_range = input_range?;
    let output_range = output_range?;
    if overlaps(input_range, output_range) {
        return Err(Failure::Overlap);
    }
    let context = BorrowContext {
        error_range,
        output_range,
    };
    // SAFETY: descriptor alignment/range and disjointness checked before POD read.
    let request = unsafe { context.record(request)? };
    if !(2..=4096).contains(&request.segments) {
        return Err(Failure::Argument(
            tapioca_geometry::arc::Error::Segments.message(),
        ));
    }
    let segments =
        usize::try_from(request.segments).map_err(|_| Failure::Argument("ArcSegments"))?;
    if scalar_capacity < (segments + 1) * 3 {
        return Err(Failure::Argument("ArcOutputTooSmall"));
    }
    tapioca_geometry::arc::make_arc(&request).map_err(|error| Failure::Argument(error.message()))
}

/// Construct an arc, writing exactly (segments+1)*3 scalar coordinates on success.
/// No partial output, retained borrow, identity or Rust allocation escapes.
///
/// # Safety
/// Request must be live initialized immutable POD. Output capacity denotes live
/// aligned exclusively writable f64 storage, disjoint from the request and error.
/// Null output requires zero capacity (which then refuses as too small).
/// Foreign validity/OOM/abort/UB are not recoverable. Error/panic envelope matches
/// other exports. Entire declared capacity, not just written prefix, is disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_make_arc(
    request: *const ArcRequest,
    output: *mut f64,
    scalar_capacity: usize,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope checks error shape and contains unwinding; prepare checks
    // all foreign ranges and computes a complete temporary before publication.
    unsafe {
        call(error, error_capacity, || {
            let points = prepare(
                request,
                output,
                scalar_capacity,
                memory_range(error.cast_const(), error_capacity)?,
            )?;
            // SAFETY: nonempty source length fits validated disjoint output;
            // raw copy permits uninitialized output without creating references.
            std::ptr::copy_nonoverlapping(points.as_ptr(), output, points.len());
            Ok(())
        })
    }
}

#[cfg(test)]
mod tests;
