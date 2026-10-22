//! Terrain ABI v1: flat input pools, retained input attribution and support rows.

use crate::borrow::{BorrowContext, SliceView};
use crate::boundary::{Failure, call, memory_range};
use crate::identity::ProvenanceRequest;
use tapioca_geometry::{identity as id, terrain as kernel};

mod edit;
mod hybrid;
mod result;
mod sample;
mod storage;
mod volume;

pub use edit::{
    TerrainDaylight, TerrainEdit, TerrainEditOutput, TerrainEditReport, TerrainEditRequest,
    TerrainFaceOwner, TerrainRing, TerrainSlope, TerrainSurfaceView, tapioca_terrain_edit,
};
pub use hybrid::{
    TerrainHybridCounts, TerrainHybridLine, TerrainHybridOutput, TerrainHybridRequest,
    TerrainHybridSide, TerrainHybridVolumes, TerrainStation, tapioca_terrain_hybrid_build,
};
pub use sample::{
    TerrainFieldSummary, TerrainSample, TerrainSampleOutput, TerrainSampleRequest,
    tapioca_terrain_sample,
};
pub use volume::{
    TerrainEditVolumes, TerrainVolumeOutput, TerrainVolumeRequest, TerrainVolumes,
    tapioca_terrain_volume,
};
#[cfg(test)]
mod tests;

/// Constraint descriptor into a single flat input-index pool.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TerrainConstraint {
    /// 1 boundary, 2 hole, 3 breakline.
    pub role: u32,
    /// Scalar offset in `constraint_vertices`.
    pub offset: u32,
    /// Number of vertex indices; rings are implicitly closed.
    pub count: u32,
}

/// Optional bounded refinement; ignored numeric fields when disabled.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct TerrainRefinement {
    /// Minimum plan angle in degrees.
    pub min_angle_degrees: f64,
    /// Optional maximum plan area in square metres.
    pub max_area_square_metres: f64,
    /// New-vertex budget, not a latency bound.
    pub max_added_vertices: u32,
    /// 0 or 1.
    pub enabled: u32,
    /// 0 or 1 when enabled.
    pub has_max_area: u32,
}

/// Terrain TIN input and schema-v1 input provenance.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TerrainRequest {
    /// xyz world metres.
    pub points: SliceView<f64>,
    /// Constraints in stable caller order.
    pub constraints: SliceView<TerrainConstraint>,
    /// Input vertex indices, selected by constraint ranges.
    pub constraint_vertices: SliceView<u32>,
    /// Absolute height tolerance in metres.
    pub z_tolerance: f64,
    /// Optional refinement.
    pub refinement: TerrainRefinement,
    /// Domains exactly vertex+edge (3): vertices are input points; edges are
    /// constraint segments flattened in constraint order. No faces or patches.
    pub provenance: ProvenanceRequest,
}

/// Baseline mesh and closed property line with input provenance.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TerrainSplitRequest {
    /// Baseline xyz world metres.
    pub points: SliceView<f64>,
    /// Baseline CCW plan triangles.
    pub triangles: SliceView<u32>,
    /// Implicitly closed xyz property line (z ignored).
    pub line: SliceView<f64>,
    /// Empty or one radian angle per line segment; nonzero refuses.
    pub arc_angles: SliceView<f64>,
    /// Domains exactly vertex+edge+face (7): mesh then line vertices,
    /// line segments, baseline triangle faces; no patches.
    pub provenance: ProvenanceRequest,
}

/// Scalar element counts of a complete terrain result.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TerrainCounts {
    /// xyz scalar count.
    pub points: usize,
    /// Triangle index scalar count.
    pub triangles: usize,
    /// Support row count.
    pub mappings: usize,
}

/// Tagged support row. See the C header for each kind's exact field contract.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TerrainMapping {
    /// Stable kind 1..11; never a native enum across C.
    pub kind: u32,
    /// Entity/input index in the domain defined by kind.
    pub index: u32,
    /// Kind-specific support indices/flags, unused values zero.
    pub values: [u32; 6],
}

/// Caller-owned disjoint result arrays and one required counts record.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TerrainOutput {
    /// Required even for a size query. Unchanged on any refusal.
    pub counts: *mut TerrainCounts,
    /// Caller-owned xyz capacity.
    pub points: *mut f64,
    /// Scalar capacity.
    pub point_capacity: usize,
    /// Caller-owned triangle indices.
    pub triangles: *mut u32,
    /// Scalar capacity.
    pub triangle_capacity: usize,
    /// Caller-owned support rows.
    pub mappings: *mut TerrainMapping,
    /// Row capacity.
    pub mapping_capacity: usize,
    /// 1 computes and publishes only counts; all capacities must be zero.
    /// 0 requires enough capacity for the complete deterministic result.
    pub query_only: u32,
}

fn attribution(
    input: &id::ValidationRequest<'_>,
    counts: id::EntityCounts,
    domains: u32,
) -> Result<(), Failure> {
    if input.counts != counts || input.domains != domains {
        return Err(Failure::State(
            "ProvenanceConflict: terrain input domain counts disagree",
        ));
    }
    id::validate_provenance(input).map_err(|error| Failure::State(error.message()))
}

// SAFETY: storage preflights every pool/capacity before any foreign borrow.
unsafe fn triangulate(
    input: *const TerrainRequest,
    output: *const TerrainOutput,
    error: storage::Range,
) -> Result<(result::ResultData, TerrainOutput), Failure> {
    // SAFETY: descriptors are immutable POD; helper validates all writable aliases.
    let (request, target, context) = unsafe { storage::tin(input, output, error)? };
    // SAFETY: all nested input pools have been checked against complete outputs/error.
    let (points, descriptors, vertices, provenance) = unsafe {
        (
            context.slice(request.points)?,
            context.slice(request.constraints)?,
            context.slice(request.constraint_vertices)?,
            context.provenance(&request.provenance)?,
        )
    };
    let mut constraints = Vec::with_capacity(descriptors.len());
    let mut segments = 0_u64;
    for descriptor in descriptors {
        let role = match descriptor.role {
            1 => kernel::Role::Boundary,
            2 => kernel::Role::Hole,
            3 => kernel::Role::Breakline,
            _ => return Err(Failure::Argument("TerrainConstraintRole")),
        };
        let start = descriptor.offset as usize;
        let end = start
            .checked_add(descriptor.count as usize)
            .ok_or(Failure::Argument("TerrainConstraintRange"))?;
        let indices = vertices
            .get(start..end)
            .ok_or(Failure::Argument("TerrainConstraintRange"))?;
        let count = u64::from(descriptor.count)
            .checked_sub(u64::from(role == kernel::Role::Breakline))
            .ok_or(Failure::Argument("TerrainConstraintShape"))?;
        segments = segments
            .checked_add(count)
            .ok_or(Failure::Argument("TerrainCountOverflow"))?;
        constraints.push(kernel::Constraint {
            role,
            vertices: indices,
        });
    }
    attribution(
        &provenance,
        id::EntityCounts {
            vertices: u64::try_from(points.len() / 3)
                .map_err(|_| Failure::Argument("TerrainCountOverflow"))?,
            edges: segments,
            faces: 0,
            patches: 0,
        },
        3,
    )?;
    let refinement = request.refinement;
    if refinement.enabled > 1 || (refinement.enabled == 1 && refinement.has_max_area > 1) {
        return Err(Failure::Argument("TerrainRefinementFlag"));
    }
    let tin = kernel::triangulate(&kernel::Request {
        points,
        constraints: &constraints,
        z_tolerance: request.z_tolerance,
        refinement: (refinement.enabled == 1).then_some(kernel::Refinement {
            min_angle_degrees: refinement.min_angle_degrees,
            max_area_square_metres: (refinement.has_max_area == 1)
                .then_some(refinement.max_area_square_metres),
            max_added_vertices: refinement.max_added_vertices,
        }),
    })
    .map_err(|error| Failure::Argument(error.message()))?;
    Ok((result::tin(tin)?, target))
}

// SAFETY: same complete checked foreign storage contract as triangulate.
unsafe fn split(
    input: *const TerrainSplitRequest,
    output: *const TerrainOutput,
    error: storage::Range,
) -> Result<(result::ResultData, TerrainOutput), Failure> {
    // SAFETY: helpers preflight descriptors and all nested pools before borrows.
    let (request, target, context) = unsafe { storage::split(input, output, error)? };
    // SAFETY: no borrow escapes; computation owns all output before publication.
    let (points, triangles, line, arc_angles, provenance) = unsafe {
        (
            context.slice(request.points)?,
            context.slice(request.triangles)?,
            context.slice(request.line)?,
            context.slice(request.arc_angles)?,
            context.provenance(&request.provenance)?,
        )
    };
    attribution(
        &provenance,
        id::EntityCounts {
            vertices: u64::try_from(points.len() / 3)
                .ok()
                .and_then(|n| {
                    u64::try_from(line.len() / 3)
                        .ok()
                        .and_then(|m| n.checked_add(m))
                })
                .ok_or(Failure::Argument("TerrainCountOverflow"))?,
            edges: u64::try_from(line.len() / 3)
                .map_err(|_| Failure::Argument("TerrainCountOverflow"))?,
            faces: u64::try_from(triangles.len() / 3)
                .map_err(|_| Failure::Argument("TerrainCountOverflow"))?,
            patches: 0,
        },
        7,
    )?;
    let mesh = kernel::split_mesh(&kernel::SplitRequest {
        points,
        triangles,
        line,
        arc_angles,
    })
    .map_err(|error| Failure::Argument(error.message()))?;
    Ok((result::split(mesh, points.len() / 3)?, target))
}

/// Triangulate with complete input attribution and returned support mappings.
/// Query mode computes the same result, returning only sizes (no retained handle).
///
/// # Safety
/// Inputs/descriptors/pools must be live aligned immutable initialized allocations.
/// Full output capacities/counts/error must be live exclusive writable allocations,
/// mutually disjoint and disjoint from every input/descriptor. Null arrays require
/// zero count/capacity. No foreign borrow escapes. Unwind is contained, not OOM,
/// abort, dangling storage or UB. Refusals preserve all result storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_terrain_triangulate(
    input: *const TerrainRequest,
    output: *const TerrainOutput,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: call validates error storage and contains panics; full preflight
    // precedes all computation and non-panicking raw result publication.
    unsafe {
        call(error, capacity, || {
            let (data, target) =
                triangulate(input, output, memory_range(error.cast_const(), capacity)?)?;
            result::publish(&data, target)
        })
    }
}

/// Split a mesh without losing parent/inside/vertex/line support mappings.
///
/// # Safety
/// Same live/disjoint/allocation/panic contract as [`tapioca_terrain_triangulate`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_terrain_split_mesh(
    input: *const TerrainSplitRequest,
    output: *const TerrainOutput,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: full checked call-scoped ingress and atomic publication as above.
    unsafe {
        call(error, capacity, || {
            let (data, target) = split(input, output, memory_range(error.cast_const(), capacity)?)?;
            result::publish(&data, target)
        })
    }
}
