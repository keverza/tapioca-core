//! #140 SDF-1d additive ABI: fixed tagged POD, query/fill and atomic publication.

use crate::borrow::{BorrowContext, SliceView};
use crate::boundary::{Failure, call, memory_range};
use crate::pools::{self, Checked, Range};
use tapioca_geometry::{affine::Vec3, sdf as kernel};

/// Nonthrowing caller cancellation callback. Context validity/lifetime is a
/// caller obligation; no foreign atomic is reinterpreted as a Rust atomic.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct SdfCancelCallback {
    /// Opaque caller context passed unchanged to `is_cancelled`.
    pub context: *mut core::ffi::c_void,
    /// Returns nonzero when cancellation is requested.
    pub is_cancelled: Option<unsafe extern "C" fn(*mut core::ffi::c_void) -> u8>,
}

/// One group or primitive entry. Unused fields must be zero; see the C header.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SdfEntry {
    /// Nonzero durable group/operation ID.
    pub id: u64,
    /// 1 group, 2 sphere, 3 rounded box, 4 capsule, 5 extrude.
    pub kind: u32,
    /// 1 Union, 2 Subtract, 3 Intersect; zero for a group.
    pub operation: u32,
    /// 1 Sharp, 2 Round.
    pub profile: u32,
    /// 0 or 1.
    pub enabled: u32,
    /// 0 or 1 for a group; zero for a primitive.
    pub isolated: u32,
    /// Zero.
    pub reserved: u32,
    /// Nonnegative operation/group blend radius in metres.
    pub blend_radius: f64,
    /// Positive primitive material; zero for a group.
    pub material_id: i32,
    /// Scalar offset into the shared local-xy profile pool.
    pub profile_offset: u32,
    /// Scalar count, even and at least six for extrusion.
    pub profile_count: u32,
    /// Zero.
    pub reserved2: u32,
    /// Sphere center / capsule A / box or extrusion frame origin.
    pub a: [f64; 3],
    /// Capsule B; otherwise zero.
    pub b: [f64; 3],
    /// Box/extrusion local x/y/z axes in world order; otherwise zero.
    pub axes: [f64; 9],
    /// Box outer half extents; otherwise zero.
    pub half: [f64; 3],
    /// Sphere/capsule/box radius.
    pub radius: f64,
    /// Extrusion half height; otherwise zero.
    pub half_height: f64,
}

/// Immutable ordered entries and shared profile pool plus build controls.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SdfRequest {
    /// Ordered group/primitive records.
    pub entries: SliceView<SdfEntry>,
    /// Concatenated local-xy scalar profile pool.
    pub profile_points: SliceView<f64>,
    /// Optional nonthrowing cancellation poll.
    pub cancel: SdfCancelCallback,
    /// Positive final/preview cell size in metres.
    pub voxel_size: f64,
    /// Zero for no solo, otherwise an enabled operation ID.
    pub solo_operation: u64,
    /// Per-job ceilings; the kernel applies its lower hard caps.
    pub max_dense_points: u64,
    /// Stored sparse-point ceiling.
    pub max_sparse_stored_points: u64,
    /// Triangle ceiling.
    pub max_triangles: u64,
    /// 1 dense, 2 sparse.
    pub strategy: u32,
    /// Zero.
    pub reserved: u32,
}

/// Scalar output counts.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SdfCounts {
    /// f64 xyz scalar count.
    pub positions: usize,
    /// f32 xyz scalar count.
    pub normals: usize,
    /// u32 index scalar count.
    pub triangles: usize,
    /// i32 material count, one per triangle.
    pub materials: usize,
}

/// Deterministic geometry hash and complete build diagnostics.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SdfSummary {
    /// Published-mesh-channel FNV-1a content hash.
    pub content_hash: u64,
    /// Total grid nodes.
    pub total_points: u64,
    /// Exactly evaluated nodes.
    pub evaluated_points: u64,
    /// Stored sparse values, excluding sign-only blocks.
    pub stored_sparse_points: u64,
    /// Visited block-tree regions.
    pub regions: u64,
    /// Conservatively skipped regions.
    pub skipped_regions: u64,
    /// Total cells.
    pub cells: u64,
    /// Active cell count.
    pub active_cells: u64,
    /// Emitted triangle count.
    pub triangles: u64,
    /// Omitted degenerate fan triangles.
    pub degenerate_triangles: u64,
    /// Conservative field Lipschitz value.
    pub lipschitz: f64,
    /// Actual per-axis cell sizes in metres.
    pub actual_spacing: [f64; 3],
    /// 1 dense, 2 sparse.
    pub strategy: u32,
    /// Fixed X-slab width in cells.
    pub slab_cells: u32,
}

/// Caller-owned query/fill output. All outputs stay unchanged on refusal.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct SdfOutput {
    /// Required counts.
    pub counts: *mut SdfCounts,
    /// Required hash/diagnostics.
    pub summary: *mut SdfSummary,
    /// xyz positions and scalar capacity.
    pub positions: *mut f64,
    /// Position scalar capacity.
    pub position_capacity: usize,
    /// xyz normals and scalar capacity.
    pub normals: *mut f32,
    /// Normal scalar capacity.
    pub normal_capacity: usize,
    /// Triangle indices and scalar capacity.
    pub triangles: *mut u32,
    /// Triangle scalar capacity.
    pub triangle_capacity: usize,
    /// Per-triangle materials and capacity.
    pub materials: *mut i32,
    /// Material capacity.
    pub material_capacity: usize,
    /// 1 query (zero capacities), 0 fill.
    pub query_only: u32,
    /// Zero.
    pub reserved: u32,
}

fn writable(target: SdfOutput) -> Vec<Checked> {
    vec![
        memory_range(target.counts.cast_const(), 1),
        memory_range(target.summary.cast_const(), 1),
        memory_range(target.positions.cast_const(), target.position_capacity),
        memory_range(target.normals.cast_const(), target.normal_capacity),
        memory_range(target.triangles.cast_const(), target.triangle_capacity),
        memory_range(target.materials.cast_const(), target.material_capacity),
    ]
}

fn mode(target: &SdfOutput) -> Result<(), Failure> {
    if target.query_only > 1
        || target.reserved != 0
        || (target.query_only == 1
            && (target.position_capacity
                | target.normal_capacity
                | target.triangle_capacity
                | target.material_capacity)
                != 0)
    {
        return Err(Failure::Argument("SdfOutputMode"));
    }
    Ok(())
}

fn point(value: [f64; 3]) -> Vec3 {
    Vec3 {
        x: value[0],
        y: value[1],
        z: value[2],
    }
}

fn frame(raw: &SdfEntry) -> kernel::Frame {
    kernel::Frame {
        origin: point(raw.a),
        axes: [
            point([raw.axes[0], raw.axes[1], raw.axes[2]]),
            point([raw.axes[3], raw.axes[4], raw.axes[5]]),
            point([raw.axes[6], raw.axes[7], raw.axes[8]]),
        ],
    }
}

fn boolean(value: u32) -> Result<kernel::BooleanOperation, Failure> {
    match value {
        1 => Ok(kernel::BooleanOperation::Union),
        2 => Ok(kernel::BooleanOperation::Subtract),
        3 => Ok(kernel::BooleanOperation::Intersect),
        _ => Err(Failure::Argument("SdfBoolean")),
    }
}

fn profile(value: u32) -> Result<kernel::BlendProfile, Failure> {
    match value {
        1 => Ok(kernel::BlendProfile::Sharp),
        2 => Ok(kernel::BlendProfile::Round),
        _ => Err(Failure::Argument("SdfBlendProfile")),
    }
}

fn zero(values: &[f64]) -> bool {
    values.iter().all(|value| *value == 0.0)
}

fn group(raw: &SdfEntry) -> Result<kernel::StackEntry, Failure> {
    if raw.operation != 0
        || raw.material_id != 0
        || raw.profile_offset != 0
        || raw.profile_count != 0
        || raw.reserved2 != 0
        || !zero(&raw.a)
        || !zero(&raw.b)
        || !zero(&raw.axes)
        || !zero(&raw.half)
        || raw.radius != 0.0
        || raw.half_height != 0.0
    {
        return Err(Failure::Argument("SdfGroupFields"));
    }
    Ok(kernel::StackEntry::Group(kernel::GroupHeader {
        id: raw.id,
        enabled: raw.enabled == 1,
        isolated: raw.isolated == 1,
        profile: profile(raw.profile)?,
        blend_radius: raw.blend_radius,
    }))
}

fn primitive(raw: &SdfEntry, profiles: &[f64]) -> Result<kernel::Primitive, Failure> {
    let no_profile = raw.profile_offset == 0 && raw.profile_count == 0;
    match raw.kind {
        2 if no_profile
            && zero(&raw.b)
            && zero(&raw.axes)
            && zero(&raw.half)
            && raw.half_height == 0.0 =>
        {
            Ok(kernel::Primitive::Sphere {
                center: point(raw.a),
                radius: raw.radius,
            })
        }
        3 if no_profile && zero(&raw.b) && raw.half_height == 0.0 => {
            Ok(kernel::Primitive::RoundedBox {
                frame: frame(raw),
                half: point(raw.half),
                radius: raw.radius,
            })
        }
        4 if no_profile && zero(&raw.axes) && zero(&raw.half) && raw.half_height == 0.0 => {
            Ok(kernel::Primitive::Capsule {
                a: point(raw.a),
                b: point(raw.b),
                radius: raw.radius,
            })
        }
        5 if zero(&raw.b) && zero(&raw.half) && raw.radius == 0.0 => {
            let start = usize::try_from(raw.profile_offset)
                .map_err(|_| Failure::Argument("SdfProfileRange"))?;
            let count = usize::try_from(raw.profile_count)
                .map_err(|_| Failure::Argument("SdfProfileRange"))?;
            let end = start
                .checked_add(count)
                .ok_or(Failure::Argument("SdfProfileRange"))?;
            let values = profiles
                .get(start..end)
                .ok_or(Failure::Argument("SdfProfileRange"))?;
            if count < 6 || !count.is_multiple_of(2) {
                return Err(Failure::Argument("SdfProfileShape"));
            }
            Ok(kernel::Primitive::Extrude {
                frame: frame(raw),
                profile: values.as_chunks::<2>().0.to_vec(),
                half_height: raw.half_height,
            })
        }
        2..=5 => Err(Failure::Argument("SdfPrimitiveFields")),
        _ => Err(Failure::Argument("SdfEntryKind")),
    }
}

fn convert_entry(raw: &SdfEntry, profiles: &[f64]) -> Result<kernel::StackEntry, Failure> {
    if raw.enabled > 1 || raw.isolated > 1 || raw.reserved != 0 {
        return Err(Failure::Argument("SdfEntryFlags"));
    }
    if raw.kind == 1 {
        return group(raw);
    }
    if raw.isolated != 0 {
        return Err(Failure::Argument("SdfPrimitiveFields"));
    }
    Ok(kernel::StackEntry::Operation(kernel::StackOperation {
        id: raw.id,
        primitive: primitive(raw, profiles)?,
        operation: boolean(raw.operation)?,
        profile: profile(raw.profile)?,
        blend_radius: raw.blend_radius,
        material_id: raw.material_id,
        enabled: raw.enabled == 1,
    }))
}

// SAFETY: the caller guarantees callback context validity and nonthrowing C
// behavior. The geometry crate receives only a boolean poll, never the pointer.
unsafe fn evaluate(
    context: &BorrowContext,
    request: &SdfRequest,
) -> Result<kernel::MeshResult, Failure> {
    // SAFETY: all pools were preflighted against every output/error range.
    let (raw_entries, profiles) = unsafe {
        (
            context.slice(request.entries)?,
            context.slice(request.profile_points)?,
        )
    };
    let entries = raw_entries
        .iter()
        .map(|entry| convert_entry(entry, profiles))
        .collect::<Result<Vec<_>, _>>()?;
    let field = kernel::PreparedField::compile(
        &entries,
        kernel::CompileOptions {
            solo_operation: (request.solo_operation != 0).then_some(request.solo_operation),
            max_operations: 256,
        },
    )
    .map_err(|error| Failure::Argument(error.message()))?;
    let strategy = match request.strategy {
        1 => kernel::SamplingStrategy::Dense,
        2 => kernel::SamplingStrategy::Sparse,
        _ => return Err(Failure::Argument("SdfSamplingStrategy")),
    };
    if request.reserved != 0 {
        return Err(Failure::Argument("SdfRequestReserved"));
    }
    let callback = request.cancel;
    let poll = || {
        callback.is_cancelled.is_some_and(|is_cancelled| {
            // SAFETY: the caller supplies a live nonthrowing callback/context for this call.
            (unsafe { is_cancelled(callback.context) }) != 0
        })
    };
    kernel::build_mesh_with_cancellation(
        &field,
        request.voxel_size,
        strategy,
        kernel::BuildLimits {
            sampling: kernel::SamplingLimits {
                max_dense_points: request.max_dense_points,
                max_sparse_stored_points: request.max_sparse_stored_points,
            },
            meshing: kernel::MeshingLimits {
                max_triangles: request.max_triangles,
            },
        },
        Some(&poll),
    )
    .map_err(|error| Failure::Argument(error.message()))
}

// SAFETY: descriptors and every pool/capacity are preflighted before borrowing;
// the computed result owns all Rust storage and no borrow survives the call.
unsafe fn compute(
    input: *const SdfRequest,
    output: *const SdfOutput,
    error: Range,
) -> Result<(kernel::MeshResult, SdfOutput), Failure> {
    // SAFETY: descriptor shapes are validated and copied once.
    let (request, target, context): (_, _, BorrowContext) =
        unsafe { pools::descriptors(input, output, error)? };
    let mut inputs = vec![memory_range(input, 1), memory_range(output, 1)];
    if let Some(request) = &request {
        inputs.extend([
            memory_range(request.entries.data, request.entries.count),
            memory_range(request.profile_points.data, request.profile_points.count),
        ]);
    }
    let outputs = target.map_or_else(Vec::new, writable);
    pools::preflight(&inputs, &outputs, error)?;
    let request = request.ok_or(Failure::Argument("SdfInputDescriptor"))?;
    let target = target.ok_or(Failure::Argument("SdfOutputDescriptor"))?;
    mode(&target)?;
    // SAFETY: the preflight above covers all borrowed inputs and writable outputs.
    let result = unsafe { evaluate(&context, &request) }?;
    Ok((result, target))
}

fn summary(result: &kernel::MeshResult) -> SdfSummary {
    SdfSummary {
        content_hash: result.content_hash,
        total_points: result.sampling.total_points,
        evaluated_points: result.sampling.evaluated_points,
        stored_sparse_points: result.sampling.stored_sparse_points,
        regions: result.sampling.regions,
        skipped_regions: result.sampling.skipped_regions,
        cells: result.stats.cells,
        active_cells: result.stats.active_cells,
        triangles: result.stats.triangles,
        degenerate_triangles: result.stats.degenerate_triangles,
        lipschitz: result.sampling.lipschitz,
        actual_spacing: result.sampling.actual_spacing,
        strategy: match result.sampling.strategy {
            kernel::SamplingStrategy::Dense => 1,
            kernel::SamplingStrategy::Sparse => 2,
        },
        slab_cells: result.stats.slab_cells,
    }
}

// SAFETY: all capacities/ranges were preflighted; every capacity is checked
// before any write and POD copies cannot unwind.
unsafe fn publish(result: &kernel::MeshResult, target: SdfOutput) -> Result<(), Failure> {
    let counts = SdfCounts {
        positions: result.positions.len(),
        normals: result.normals.len(),
        triangles: result.triangles.len(),
        materials: result.tri_material.len(),
    };
    if target.query_only == 0
        && (target.position_capacity < counts.positions
            || target.normal_capacity < counts.normals
            || target.triangle_capacity < counts.triangles
            || target.material_capacity < counts.materials)
    {
        return Err(Failure::Argument("SdfOutputTooSmall"));
    }
    // SAFETY: ranges are live, aligned, exclusive and disjoint by the caller and
    // preflight; complete capacities were checked above.
    unsafe {
        if target.query_only == 0 {
            copy(&result.positions, target.positions);
            copy(&result.normals, target.normals);
            copy(&result.triangles, target.triangles);
            copy(&result.tri_material, target.materials);
        }
        target.summary.write(summary(result));
        target.counts.write(counts);
    }
    Ok(())
}

// SAFETY: source/target are nonoverlapping by preflight and have values.len()
// initialized/writable elements respectively. No reference survives the call.
unsafe fn copy<T: Copy>(values: &[T], target: *mut T) {
    if !values.is_empty() {
        // SAFETY: justified by the caller and the complete preflight above.
        unsafe { std::ptr::copy_nonoverlapping(values.as_ptr(), target, values.len()) };
    }
}

/// Build one deterministic volumetric detail with query/fill atomic output.
///
/// # Safety
/// Every input/output must be live, aligned and initialized/writable as
/// applicable; writable ranges must be disjoint from all inputs, each other and
/// error storage. Callback context is live, nonthrowing and does not interfere
/// with operation storage. Null arrays require zero count/capacity. Foreign
/// validity, OOM, abort and UB are not recoverable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sdf_build(
    input: *const SdfRequest,
    output: *const SdfOutput,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: call validates error storage and contains unwinding; preflight and
    // complete publication rules are implemented by compute/publish above.
    unsafe {
        call(error, capacity, || {
            let (result, target) =
                compute(input, output, memory_range(error.cast_const(), capacity)?)?;
            publish(&result, target)
        })
    }
}

#[cfg(test)]
mod tests;
