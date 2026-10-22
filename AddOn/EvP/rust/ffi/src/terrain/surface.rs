//! Attributed native solid -> plan surface, reusing terrain query/fill output.
use super::{
    Failure, ProvenanceRequest, SliceView, TerrainOutput, attribution, call, memory_range, result,
    storage,
};
use crate::pools;
use tapioca_geometry::{identity as id, terrain::surface as kernel};

/// Original native mesh with domains=5 input attribution.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TerrainSurfaceRequest {
    /// Flat xyz world metres.
    pub points: SliceView<f64>,
    /// Native triangle vertex indices.
    pub triangles: SliceView<u32>,
    /// Complete input vertex/face catalog, retained by the caller.
    pub provenance: ProvenanceRequest,
}

fn index(value: usize) -> Result<u32, Failure> {
    u32::try_from(value).map_err(|_| Failure::Argument("TerrainCountOverflow"))
}

// SAFETY: all foreign allocations stay live/immutable or exclusively writable
// for the call. Descriptors and full capacities are preflighted before borrowing.
unsafe fn surface(
    input: *const TerrainSurfaceRequest,
    output: *const TerrainOutput,
    error: storage::Range,
) -> Result<(result::ResultData, TerrainOutput), Failure> {
    // SAFETY: checked POD copies, no borrowed nested array yet.
    let (request, target, context) = unsafe { pools::descriptors(input, output, error)? };
    let mut inputs = vec![memory_range(input, 1), memory_range(output, 1)];
    if let Some(request) = &request {
        inputs.extend(pools::provenance(&request.provenance));
        inputs.extend([
            memory_range(request.points.data, request.points.count),
            memory_range(request.triangles.data, request.triangles.count),
        ]);
    }
    let target = storage::finish(&inputs, target, error)?;
    let request = request.ok_or(Failure::Argument("TerrainInputDescriptor"))?;
    // SAFETY: nested pools were checked against descriptors, full outputs/error.
    let (points, triangles, provenance) = unsafe {
        (
            context.slice(request.points)?,
            context.slice(request.triangles)?,
            context.provenance(&request.provenance)?,
        )
    };
    attribution(
        &provenance,
        id::EntityCounts {
            vertices: u64::try_from(points.len() / 3)
                .map_err(|_| Failure::Argument("TerrainCountOverflow"))?,
            edges: 0,
            faces: u64::try_from(triangles.len() / 3)
                .map_err(|_| Failure::Argument("TerrainCountOverflow"))?,
            patches: 0,
        },
        5,
    )?;
    let surface = kernel::extract(points, triangles).map_err(|e| Failure::Argument(e.message()))?;
    let mut mappings = Vec::with_capacity(surface.vertices.len() + surface.faces.len());
    for [vertex, native] in surface.vertices {
        mappings.push(super::TerrainMapping {
            kind: 18,
            index: vertex,
            values: [native, 0, 0, 0, 0, 0],
        });
    }
    for (face, native) in surface.faces.into_iter().enumerate() {
        mappings.push(super::TerrainMapping {
            kind: 19,
            index: index(face)?,
            values: [native, 0, 0, 0, 0, 0],
        });
    }
    Ok((
        result::ResultData {
            points: surface.points,
            triangles: surface.triangles,
            mappings,
        },
        target,
    ))
}

/// Select a native terrain solid's upward surface, with complete support rows.
/// Query/fill, atomic publication, no retained handle or semantic ID allocation.
///
/// # Safety
/// Same live/aligned/disjoint/panic contract as `tapioca_terrain_triangulate`.
/// Input is a native height-field solid, not arbitrary overlapping sheets.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_terrain_surface(
    input: *const TerrainSurfaceRequest,
    output: *const TerrainOutput,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: call contains panics; complete preflight precedes any slice/write.
    unsafe {
        call(error, capacity, || {
            let (data, target) =
                surface(input, output, memory_range(error.cast_const(), capacity)?)?;
            result::publish(&data, target)
        })
    }
}
