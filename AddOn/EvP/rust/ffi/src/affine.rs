//! Coarse S2b affine vector batch, with caller-owned atomic output publication.

use crate::borrow::{BorrowContext, SliceView};
use crate::boundary::{Failure, call, memory_range, overlaps};
use tapioca_geometry::affine;

/// Borrowed matrix and interleaved vector inputs; no topology/identity is changed.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AffineRequest {
    /// Row-major 3x4 affine matrix; translations are world metres.
    pub transform: affine::Transform,
    /// 1 point, 2 direction, 3 inverse-transpose normal; unknown values refuse.
    pub mode: u32,
    /// Borrowed interleaved xyz scalars, never retained.
    pub values: SliceView<f64>,
}

/// Caller-owned output regions; the descriptor itself is an immutable borrow.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AffineOutput {
    /// Live aligned writable scalar storage; may be null only at capacity zero.
    pub data: *mut f64,
    /// Available scalar capacity, at least the input scalar count.
    pub capacity: usize,
    /// Required exclusive writable u32 storage for orientation (0 or 1).
    pub flips_orientation: *mut u32,
}

/// Tagged builder POD. Only the selected variant's fields are consumed.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TransformBuilderRequest {
    /// 1 translation, 2 rotation, 3 scaling, 4 mirroring. Unknown tags refuse.
    pub kind: u32,
    /// World-metre pivot for rotation/scaling/mirroring; ignored for translation.
    pub origin: affine::Vec3,
    /// Displacement metres, rotation axis, dimensionless factors, or mirror normal.
    pub vector: affine::Vec3,
    /// Radians for rotation only; ignored for all other tags.
    pub radians: f64,
}

fn builder(request: TransformBuilderRequest) -> Result<affine::TransformBuilder, Failure> {
    let TransformBuilderRequest {
        kind,
        origin,
        vector,
        radians,
    } = request;
    match kind {
        1 => Ok(affine::TransformBuilder::Translation(vector)),
        2 => Ok(affine::TransformBuilder::Rotation {
            origin,
            axis: vector,
            radians,
        }),
        3 => Ok(affine::TransformBuilder::Scaling {
            origin,
            factors: vector,
        }),
        4 => Ok(affine::TransformBuilder::Mirroring {
            origin,
            normal: vector,
        }),
        _ => Err(Failure::Argument(
            "TransformBuildKind: expected translation, rotation, scaling or mirroring",
        )),
    }
}

fn mode(value: u32) -> Result<affine::ApplyMode, Failure> {
    match value {
        1 => Ok(affine::ApplyMode::Point),
        2 => Ok(affine::ApplyMode::Direction),
        3 => Ok(affine::ApplyMode::Normal),
        _ => Err(Failure::Argument(
            "AffineModeInvalid: expected point, direction or normal",
        )),
    }
}

// SAFETY: all descriptors/input arrays are initialized live immutable allocations;
// output regions are live writable allocations, not aliases of immutable input.
unsafe fn prepare(
    input: *const AffineRequest,
    output: *const AffineOutput,
    error_range: Option<(usize, usize)>,
) -> Result<(affine::AppliedVectors, AffineOutput), Failure> {
    let context = BorrowContext {
        error_range,
        output_range: None,
    };
    // SAFETY: shape/output overlap is checked before each descriptor read. The
    // copies contain only all-bit-valid POD fields, not references or Rust enums.
    let (request, target) = unsafe { (context.record(input)?, context.record(output)?) };
    let coordinates = memory_range(target.data.cast_const(), target.capacity)?;
    let orientation = memory_range(target.flips_orientation.cast_const(), 1)?;
    let inputs = [
        memory_range(input, 1)?,
        memory_range(output, 1)?,
        memory_range(request.values.data, request.values.count)?,
    ];
    if overlaps(coordinates, orientation)
        || [coordinates, orientation].iter().any(|&range| {
            overlaps(range, error_range) || inputs.iter().any(|&input| overlaps(range, input))
        })
    {
        return Err(Failure::Overlap);
    }
    if target.capacity < request.values.count {
        return Err(Failure::Argument(
            "AffineOutputTooSmall: scalar capacity is smaller than input",
        ));
    }
    // SAFETY: input/output/error shape and pairwise output overlap were checked;
    // caller guarantees the immutable allocation remains valid through the call.
    let values = unsafe { context.slice(request.values)? };
    let result = affine::apply_vectors(&request.transform, mode(request.mode)?, values)
        .map_err(|error| Failure::Argument(error.message()))?;
    if result.values.len() != request.values.count {
        return Err(Failure::State(
            "AffineResultShape: kernel result scalar count mismatch",
        ));
    }
    Ok((result, target))
}

/// Transform one batch of affine positions, directions or normals.
///
/// All output regions remain unchanged on refusal or a contained unwinding
/// panic. A private Rust temporary holds the completed finite result; no Rust
/// allocation or borrow escapes. Only the requested prefix is written.
/// Exact singular normal transforms preserve the input, matching the C++ oracle.
///
/// # Safety
/// Both descriptors and all nonempty inputs must be initialized live immutable
/// allocations. Both output regions and nonempty error storage must be live,
/// aligned, exclusively writable, mutually disjoint and disjoint from ALL inputs
/// and descriptors. Shape checks cannot establish allocation validity. Abort/OOM
/// and invalid foreign memory are not contained by the panic envelope.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_transform_vectors(
    input: *const AffineRequest,
    output: *const AffineOutput,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: shared envelope checks error shape and contains unwinding panics;
    // prepare checks all input/output ranges before any write or array borrow.
    unsafe {
        call(error, capacity, || {
            let (result, target) =
                prepare(input, output, memory_range(error.cast_const(), capacity)?)?;
            // SAFETY: completed count equals input count <= checked capacity.
            // All immutable foreign borrows have ended; the temporary is a live
            // Rust allocation disjoint from caller-owned live writable storage.
            // Raw writes permit uninitialized output storage without creating a
            // reference to uninitialized f64/u32 values.
            if !result.values.is_empty() {
                std::ptr::copy_nonoverlapping(
                    result.values.as_ptr(),
                    target.data,
                    result.values.len(),
                );
            }
            target
                .flips_orientation
                .write(u32::from(result.flips_orientation));
            Ok(())
        })
    }
}

/// Compose a chain of row-major 3x4 matrices in application order.
///
/// Empty input returns identity. Input and result coefficients/determinants must
/// be finite. Output is committed only after the entire chain succeeds.
///
/// # Safety
/// Nonempty input must be live initialized immutable storage for `count`
/// transforms. Output must be live aligned exclusive writable storage for one
/// transform, disjoint from input and nonempty error storage. Null input is
/// accepted only at count zero. No allocation, borrow or host callback escapes.
/// Shape checks cannot prove allocation validity; abort/OOM/UB are not contained.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_compose_transforms(
    transforms: *const affine::Transform,
    count: usize,
    output: *mut affine::Transform,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: the caller guarantees live call-scoped allocations. Shared checks
    // reject malformed/overlapping ranges before borrowing or publishing output.
    unsafe {
        call(error, capacity, || {
            let output_range = memory_range(output.cast_const(), 1)?;
            let error_range = memory_range(error.cast_const(), capacity)?;
            if overlaps(output_range, error_range) {
                return Err(Failure::Overlap);
            }
            let context = BorrowContext {
                error_range,
                output_range,
            };
            let input = context.slice(SliceView {
                data: transforms,
                count,
            })?;
            let result = affine::compose_transforms(input)
                .map_err(|error| Failure::Argument(error.message()))?;
            // SAFETY: all foreign immutable borrows end before this single raw
            // write to checked disjoint caller-owned output (may be uninitialized).
            output.write(result);
            Ok(())
        })
    }
}

/// Build a finite 3x4 transform with no allocation or retained borrow.
/// Output is unchanged on refusal/contained panic; unused tagged fields are ignored.
///
/// # Safety
/// Request is live initialized immutable POD. Output is live aligned exclusively
/// writable storage for one transform, disjoint from request/error. Known error
/// overlap wins even if another region is malformed. Caller guarantees allocation
/// validity/lifetimes; null/shape checks do not recover foreign UB or OOM/abort.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_build_transform(
    input: *const TransformBuilderRequest,
    output: *mut affine::Transform,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: shared envelope checks error shape and contains unwinding. Known
    // range overlaps are refused before a descriptor read or diagnostic write.
    unsafe {
        call(error, capacity, || {
            let error_range = memory_range(error.cast_const(), capacity)?;
            let input_range = memory_range(input, 1);
            let output_range = memory_range(output.cast_const(), 1);
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
            let request = context.record(input)?;
            let result = affine::build_transform(builder(request)?)
                .map_err(|error| Failure::Argument(error.message()))?;
            // SAFETY: all foreign borrows end before one raw write to checked
            // disjoint caller-owned storage, which may initially be uninitialized.
            output.write(result);
            Ok(())
        })
    }
}

#[cfg(test)]
mod builder_tests;
#[cfg(test)]
mod tests;
