//! Coarse mesh preparation with full-capacity alias preflight and atomic outputs.

use crate::borrow::{BorrowContext, SliceView};
use crate::boundary::{Failure, call, memory_range, overlaps};
use tapioca_geometry::{mesh::MeshView, mesh_prep as prep, value::Aabb};

/// Ordered corner Add calls and body-local Reset boundaries.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct WeldRequest {
    /// Call-scoped corners; normal is GIVEN, not normalized by this operation.
    pub corners: SliceView<prep::WeldCorner>,
    /// Nondecreasing offsets [0,...,corner count], including empty bodies.
    pub body_offsets: SliceView<u64>,
}

/// Complete welding output; buffers have worst-case capacity, lengths in summary.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct WeldOutput {
    /// Required output vertex count (not scalar count).
    pub vertex_count: *mut usize,
    /// World-metre xyz output, capacity at least 3 * corner count.
    pub positions: *mut f64,
    /// Given first normal per output vertex, capacity at least 3 * corner count.
    pub normals: *mut f32,
    /// Shared position/normal scalar capacity, not vertex capacity.
    pub scalar_capacity: usize,
    /// Every corner's output vertex; capacity at least corner count.
    pub corner_vertices: *mut u32,
    /// Corner remap capacity in u32 elements.
    pub corner_capacity: usize,
}

/// Geometry-only analysis. Sidecars/identity remain caller-owned and unchanged.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct MeshAnalysisRequest {
    /// Flat world-metre xyz, including unused vertices.
    pub positions: SliceView<f64>,
    /// Empty or flat per-vertex f32 xyz normals.
    pub normals: SliceView<f32>,
    /// Triangle vertex indices, not scalar offsets.
    pub triangles: SliceView<u32>,
    /// Canonical edge keys, in the SAME vertex-index domain as triangles.
    pub visible_edges: SliceView<u64>,
    /// Empty (global pool), or one locally sorted checked range per triangle.
    pub edge_ranges: SliceView<prep::EdgeRange>,
}

/// Small output record published together with all analysis arrays.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct MeshAnalysisSummary {
    /// Tight supplied-vertex bounds, not retained source-body capture bounds.
    pub bounds: Aabb,
    /// 0 or 1; no native bool in the ABI.
    pub normals_usable: u32,
}

/// Disjoint caller-owned analysis output storage.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct MeshAnalysisOutput {
    /// Required writable summary record, including for empty geometry.
    pub summary: *mut MeshAnalysisSummary,
    /// Smooth fallback xyz normals, capacity at least position scalar count.
    pub smooth_normals: *mut f32,
    /// Normal scalar capacity.
    pub scalar_capacity: usize,
    /// 0/1 roundoff flags, one per triangle; no triangles are removed.
    pub roundoff_zero: *mut u8,
    /// Three barycentric-order visibility bits per triangle.
    pub wire_masks: *mut u8,
    /// Capacity of EACH triangle output buffer, in bytes/elements.
    pub triangle_capacity: usize,
}

type Range = Option<(usize, usize)>;

// Check every known error overlap BEFORE propagating any other malformed range.
// This prevents diagnostics from corrupting an otherwise valid output on refusal.
fn preflight<const I: usize, const O: usize>(
    inputs: [Result<Range, Failure>; I],
    outputs: [Result<Range, Failure>; O],
    error: Range,
) -> Result<(), Failure> {
    if inputs
        .iter()
        .chain(&outputs)
        .flatten()
        .any(|&range| overlaps(range, error))
    {
        return Err(Failure::Overlap);
    }
    let mut input_ranges = [None; I];
    let mut output_ranges = [None; O];
    for (target, range) in input_ranges.iter_mut().zip(inputs) {
        *target = range?;
    }
    for (target, range) in output_ranges.iter_mut().zip(outputs) {
        *target = range?;
    }
    for (index, &output) in output_ranges.iter().enumerate() {
        if input_ranges.iter().any(|&input| overlaps(input, output))
            || output_ranges
                .iter()
                .skip(index + 1)
                .any(|&other| overlaps(output, other))
        {
            return Err(Failure::Overlap);
        }
    }
    Ok(())
}

// SAFETY: caller guarantees live initialized immutable descriptors. Representation
// and error overlap are checked before reading either descriptor by value.
unsafe fn descriptors<I: Copy, O: Copy>(
    input: *const I,
    output: *const O,
    error: Range,
) -> Result<(I, O), Failure> {
    preflight([memory_range(input, 1), memory_range(output, 1)], [], error)?;
    let context = BorrowContext {
        error_range: error,
        output_range: None,
    };
    // SAFETY: descriptor ranges checked; caller guarantees their allocation validity.
    unsafe { Ok((context.record(input)?, context.record(output)?)) }
}

// SAFETY: all nonempty foreign buffers must have live allocations; numeric checks
// cannot establish that. No foreign borrow escapes the returned owned result.
unsafe fn prepare_weld(
    input: *const WeldRequest,
    output: *const WeldOutput,
    error: Range,
) -> Result<(prep::WeldedCorners, WeldOutput), Failure> {
    // SAFETY: caller guarantees descriptor validity, helper checks ranges/overlap.
    let (request, target) = unsafe { descriptors(input, output, error)? };
    preflight(
        [
            memory_range(input, 1),
            memory_range(output, 1),
            memory_range(request.corners.data, request.corners.count),
            memory_range(request.body_offsets.data, request.body_offsets.count),
        ],
        [
            memory_range(target.vertex_count.cast_const(), 1),
            memory_range(target.positions.cast_const(), target.scalar_capacity),
            memory_range(target.normals.cast_const(), target.scalar_capacity),
            memory_range(target.corner_vertices.cast_const(), target.corner_capacity),
        ],
        error,
    )?;
    let scalars = request
        .corners
        .count
        .checked_mul(3)
        .ok_or(Failure::Argument("WeldCountOverflow"))?;
    if target.scalar_capacity < scalars || target.corner_capacity < request.corners.count {
        return Err(Failure::Argument("WeldOutputTooSmall"));
    }
    let context = BorrowContext {
        error_range: error,
        output_range: None,
    };
    // SAFETY: complete input/output/error ranges preflighted and disjoint;
    // temporary safe views last only through the owned computation.
    let result = unsafe {
        prep::weld_corners(&prep::WeldRequest {
            corners: context.slice(request.corners)?,
            body_offsets: context.slice(request.body_offsets)?,
        })
    }
    .map_err(|error| Failure::Argument(error.message()))?;
    Ok((result, target))
}

// SAFETY: same caller-owned live allocation contract as prepare_weld.
unsafe fn prepare_analysis(
    input: *const MeshAnalysisRequest,
    output: *const MeshAnalysisOutput,
    error: Range,
) -> Result<(prep::MeshAnalysis, MeshAnalysisOutput), Failure> {
    // SAFETY: caller guarantees descriptor validity, helper checks ranges/overlap.
    let (request, target) = unsafe { descriptors(input, output, error)? };
    preflight(
        [
            memory_range(input, 1),
            memory_range(output, 1),
            memory_range(request.positions.data, request.positions.count),
            memory_range(request.normals.data, request.normals.count),
            memory_range(request.triangles.data, request.triangles.count),
            memory_range(request.visible_edges.data, request.visible_edges.count),
            memory_range(request.edge_ranges.data, request.edge_ranges.count),
        ],
        [
            memory_range(target.summary.cast_const(), 1),
            memory_range(target.smooth_normals.cast_const(), target.scalar_capacity),
            memory_range(target.roundoff_zero.cast_const(), target.triangle_capacity),
            memory_range(target.wire_masks.cast_const(), target.triangle_capacity),
        ],
        error,
    )?;
    if target.scalar_capacity < request.positions.count
        || target.triangle_capacity < request.triangles.count / 3
    {
        return Err(Failure::Argument("MeshAnalysisOutputTooSmall"));
    }
    let context = BorrowContext {
        error_range: error,
        output_range: None,
    };
    // SAFETY: all complete capacities checked disjoint before any slice borrow.
    // No input borrow is retained by the newly owned result.
    let result = unsafe {
        prep::analyze_mesh(&prep::AnalysisRequest {
            mesh: MeshView {
                guid: "",
                elem_type: 0,
                vertices: context.slice(request.positions)?,
                normals: context.slice(request.normals)?,
                triangles: context.slice(request.triangles)?,
                tri_material: &[],
                tri_wire_edges: &[],
                tri_source_body: &[],
                tri_source_pgon: &[],
                tri_surface_attribute: &[],
            },
            visible_edges: context.slice(request.visible_edges)?,
            edge_ranges: context.slice(request.edge_ranges)?,
        })
    }
    .map_err(|error| Failure::Argument(error.message()))?;
    Ok((result, target))
}

// SAFETY: destination has validated disjoint writable capacity for all values.
unsafe fn publish<T: Copy>(values: &[T], output: *mut T) {
    if !values.is_empty() {
        // SAFETY: source is owned live storage; destination was preflighted.
        unsafe {
            std::ptr::copy_nonoverlapping(values.as_ptr(), output, values.len());
        }
    }
}

/// Body-local welding with complete corner remap, no semantic ID allocation.
/// All buffers/count publish together only after full successful computation.
///
/// # Safety
/// Descriptors/inputs must be live aligned initialized immutable allocations.
/// Full output capacities must be live exclusive writable allocations, mutually
/// disjoint and disjoint from ALL descriptors/inputs/error. Count output required
/// even for empty. Null arrays accepted only for zero count/capacity. Shared
/// panic envelope cannot recover OOM/abort/invalid foreign allocation or UB.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_weld_corners(
    input: *const WeldRequest,
    output: *const WeldOutput,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: envelope validates error range and contains unwinds; prepare checks
    // every foreign range before computation. Raw publication cannot unwind.
    unsafe {
        call(error, capacity, || {
            let (result, target) =
                prepare_weld(input, output, memory_range(error.cast_const(), capacity)?)?;
            publish(&result.positions, target.positions);
            publish(&result.normals, target.normals);
            publish(&result.corner_vertices, target.corner_vertices);
            target.vertex_count.write(result.positions.len() / 3);
            Ok(())
        })
    }
}

/// Analyze without changing topology, source sidecars or identity. Bounds are
/// tight SUPPLIED-vertex bounds, never a replacement for retained capture bounds.
/// All summary/array outputs publish together on success; tails are untouched.
///
/// # Safety
/// Same live aligned immutable input/exclusive full-capacity output/disjointness
/// obligations as [`tapioca_weld_corners`]. Summary required even for empty.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_analyze_mesh(
    input: *const MeshAnalysisRequest,
    output: *const MeshAnalysisOutput,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: range preflight precedes computation and atomic raw publication.
    unsafe {
        call(error, capacity, || {
            let (result, target) =
                prepare_analysis(input, output, memory_range(error.cast_const(), capacity)?)?;
            publish(&result.smooth_normals, target.smooth_normals);
            publish(&result.roundoff_zero, target.roundoff_zero);
            publish(&result.wire_masks, target.wire_masks);
            target.summary.write(MeshAnalysisSummary {
                bounds: result.bounds,
                normals_usable: u32::from(result.normals_usable),
            });
            Ok(())
        })
    }
}

#[cfg(test)]
mod tests;
