//! A built terrain at a level of detail (geometry `terrain::detail`), through the terrain
//! query/fill output. A derived display and bake surface: it carries no input provenance, only
//! which built vertex each of its vertices is and which of its edges are kept lines.
use super::{
    Failure, SliceView, TerrainMapping, TerrainOutput, call, memory_range, result, storage,
};
use crate::pools;
use tapioca_geometry::terrain::detail as kernel;

/// The built mesh, what stays exact, and the tolerance.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TerrainDetailRequest {
    /// Flat xyz world metres.
    pub points: SliceView<f64>,
    /// Triangle vertex indices.
    pub triangles: SliceView<u32>,
    /// Vertices that stay whatever the tolerance.
    pub keep: SliceView<u32>,
    /// Flat vertex pairs that stay edges (reduced to `EXACT` along their chained lines).
    pub lines: SliceView<u32>,
    /// Largest height difference, metres, at a built vertex; positive.
    pub tolerance: f64,
}

// SAFETY: all foreign allocations stay live/immutable or exclusively writable
// for the call. Descriptors and full capacities are preflighted before borrowing.
unsafe fn detail(
    input: *const TerrainDetailRequest,
    output: *const TerrainOutput,
    error: storage::Range,
) -> Result<(result::ResultData, TerrainOutput), Failure> {
    // SAFETY: checked POD copies, no borrowed nested array yet.
    let (request, target, context) = unsafe { pools::descriptors(input, output, error)? };
    let mut inputs = vec![memory_range(input, 1), memory_range(output, 1)];
    if let Some(request) = &request {
        inputs.extend([
            memory_range(request.points.data, request.points.count),
            memory_range(request.triangles.data, request.triangles.count),
            memory_range(request.keep.data, request.keep.count),
            memory_range(request.lines.data, request.lines.count),
        ]);
    }
    let target = storage::finish(&inputs, target, error)?;
    let request = request.ok_or(Failure::Argument("TerrainInputDescriptor"))?;
    // SAFETY: nested pools were checked against descriptors, full outputs/error.
    let (points, triangles, keep, lines) = unsafe {
        (
            context.slice(request.points)?,
            context.slice(request.triangles)?,
            context.slice(request.keep)?,
            context.slice(request.lines)?,
        )
    };
    if lines.len() % 2 != 0 {
        return Err(Failure::Argument("DetailLines: expected vertex pairs"));
    }
    let count = points.len() / 3;
    let mut kept = vec![false; count];
    for &v in keep {
        *kept
            .get_mut(v as usize)
            .ok_or(Failure::Argument("DetailKeep: vertex out of range"))? = true;
    }
    let pairs: Vec<[u32; 2]> = lines.as_chunks::<2>().0.to_vec();
    let detailed = kernel::simplify(&kernel::DetailRequest {
        points,
        triangles,
        keep: &kept,
        lines: &pairs,
        tolerance: request.tolerance,
    })
    .map_err(|e| Failure::Argument(e.message()))?;
    let mut mappings = Vec::with_capacity(detailed.source.len() + detailed.lines.len());
    // 24 Detail vertex: index=output vertex, v0=built vertex.
    for (vertex, &built) in (0_u32..).zip(&detailed.source) {
        mappings.push(TerrainMapping {
            kind: 24,
            index: vertex,
            values: [built, 0, 0, 0, 0, 0],
        });
    }
    // 25 Detail line edge: index=ordinal, v0/v1=output endpoints of a kept line or the boundary.
    for (ordinal, &[a, b]) in (0_u32..).zip(&detailed.lines) {
        mappings.push(TerrainMapping {
            kind: 25,
            index: ordinal,
            values: [a, b, 0, 0, 0, 0],
        });
    }
    Ok((
        result::ResultData {
            points: detailed.points,
            triangles: detailed.triangles,
            mappings,
        },
        target,
    ))
}

/// Simplify a built terrain to a vertical tolerance, its kept points and lines exact.
/// Query/fill, atomic publication, no retained handle or semantic ID allocation.
///
/// # Safety
/// Same live/aligned/disjoint/panic contract as `tapioca_terrain_surface`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_terrain_detail(
    input: *const TerrainDetailRequest,
    output: *const TerrainOutput,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: call contains panics; complete preflight precedes any slice/write.
    unsafe {
        call(error, capacity, || {
            let (data, target) =
                detail(input, output, memory_range(error.cast_const(), capacity)?)?;
            result::publish(&data, target)
        })
    }
}
