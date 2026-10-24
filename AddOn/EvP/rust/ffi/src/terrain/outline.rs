//! Terrain outline grading ABI: the terrain graded from a paved area's TIN
//! (`terrain::outline`). A preview result: proposed points, base heights,
//! triangles, the region ring and exact volumes; no support rows.

use super::edit::{TerrainSlope, slope};
use super::hybrid::TerrainHybridVolumes;
use super::{BorrowContext, Failure, SliceView, call, memory_range};
use crate::pools::{self, Range};
use tapioca_geometry::terrain::edit::Effect;
use tapioca_geometry::terrain::outline::{Grading, OutlineRequest, Outlined, build};
use tapioca_geometry::terrain::volume::exact::TriangleVolumes;

/// The base mesh, the region, the paved surface, the plan breaklines and
/// points, and the faces. `line_counts` gives each breakline's vertex count in
/// `line_points` (flat xyz, z ignored). `effect`: 1 cut, 2 fill, 3 both.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TerrainOutlineRequest {
    /// Flat xyz base vertices, world metres.
    pub points: SliceView<f64>,
    /// Counter-clockwise base vertex triples.
    pub triangles: SliceView<u32>,
    /// Implicitly closed flat xyz region ring (z ignored).
    pub region: SliceView<f64>,
    /// Flat xyz paved surface vertices.
    pub surface_points: SliceView<f64>,
    /// Counter-clockwise paved surface triples.
    pub surface_triangles: SliceView<u32>,
    /// Every breakline's flat xyz vertices, one after another.
    pub line_points: SliceView<f64>,
    /// Vertices per breakline.
    pub line_counts: SliceView<u32>,
    /// Isolated flat xyz points (z ignored).
    pub spots: SliceView<f64>,
    /// Cut face grade.
    pub cut: TerrainSlope,
    /// Fill face grade.
    pub fill: TerrainSlope,
    /// Plan reach of a face, metres.
    pub limit: f64,
    /// Plan length a face rounds over at its top and toe, metres.
    pub rounding: f64,
    /// At-grade height band, metres.
    pub tolerance: f64,
    /// Effect.
    pub effect: u32,
    /// Zero.
    pub reserved: u32,
}

/// Scalar element counts of a complete outline result.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TerrainOutlineCounts {
    /// xyz scalar count.
    pub points: usize,
    /// Triangle index scalar count.
    pub triangles: usize,
    /// Region ring vertex count.
    pub ring: usize,
    /// Vertices whose face ran out at the limit before meeting the ground.
    pub unreached: u32,
    /// Zero.
    pub reserved: u32,
}

/// Caller-owned disjoint result storage; query mode publishes only `counts`
/// and needs zero capacities.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TerrainOutlineOutput {
    /// Required counts record.
    pub counts: *mut TerrainOutlineCounts,
    /// Required site (inside the region) volumes.
    pub site: *mut TerrainHybridVolumes,
    /// Required context (outside) volumes.
    pub context: *mut TerrainHybridVolumes,
    /// Proposed xyz.
    pub points: *mut f64,
    /// Scalar capacity.
    pub point_capacity: usize,
    /// Base height per output vertex.
    pub base: *mut f64,
    /// Capacity, at least the vertex count.
    pub base_capacity: usize,
    /// Output vertex triples.
    pub triangles: *mut u32,
    /// Scalar capacity.
    pub triangle_capacity: usize,
    /// The region ring as a closed chain of output vertices.
    pub ring: *mut u32,
    /// Capacity.
    pub ring_capacity: usize,
    /// 1 publishes only counts; 0 publishes the complete result.
    pub query_only: u32,
    /// Zero.
    pub reserved: u32,
}

const fn volumes(value: TriangleVolumes) -> TerrainHybridVolumes {
    TerrainHybridVolumes {
        cut: value.cut,
        fill: value.fill,
        net: value.net,
        area: value.area,
        triangles: value.triangles,
        reserved: 0,
    }
}

// SAFETY: request pools, outputs and error are preflighted disjoint first.
unsafe fn compute(
    input: *const TerrainOutlineRequest,
    output: *const TerrainOutlineOutput,
    error: Range,
) -> Result<(Outlined, TerrainOutlineOutput), Failure> {
    // SAFETY: descriptors validates both POD shapes before copying them.
    let (request, target, context): (_, _, BorrowContext) =
        unsafe { pools::descriptors(input, output, error)? };
    let mut inputs = vec![memory_range(input, 1), memory_range(output, 1)];
    if let Some(r) = &request {
        inputs.extend([
            memory_range(r.points.data, r.points.count),
            memory_range(r.triangles.data, r.triangles.count),
            memory_range(r.region.data, r.region.count),
            memory_range(r.surface_points.data, r.surface_points.count),
            memory_range(r.surface_triangles.data, r.surface_triangles.count),
            memory_range(r.line_points.data, r.line_points.count),
            memory_range(r.line_counts.data, r.line_counts.count),
            memory_range(r.spots.data, r.spots.count),
        ]);
    }
    let outputs = target.map_or_else(Vec::new, |t| {
        vec![
            memory_range(t.counts.cast_const(), 1),
            memory_range(t.site.cast_const(), 1),
            memory_range(t.context.cast_const(), 1),
            memory_range(t.points.cast_const(), t.point_capacity),
            memory_range(t.base.cast_const(), t.base_capacity),
            memory_range(t.triangles.cast_const(), t.triangle_capacity),
            memory_range(t.ring.cast_const(), t.ring_capacity),
        ]
    });
    pools::preflight(&inputs, &outputs, error)?;
    let request = request.ok_or(Failure::Argument("TerrainInputDescriptor"))?;
    let target = target.ok_or(Failure::Argument("TerrainOutputDescriptor"))?;
    if request.reserved != 0
        || target.reserved != 0
        || target.query_only > 1
        || (target.query_only == 1
            && (target.point_capacity
                | target.base_capacity
                | target.triangle_capacity
                | target.ring_capacity)
                != 0)
    {
        return Err(Failure::Argument("TerrainOutputMode"));
    }
    // SAFETY: every pool was preflighted against outputs, descriptors and error.
    let (
        points,
        triangles,
        region,
        surface_points,
        surface_triangles,
        line_points,
        line_counts,
        spots,
    ) = unsafe {
        (
            context.slice(request.points)?,
            context.slice(request.triangles)?,
            context.slice(request.region)?,
            context.slice(request.surface_points)?,
            context.slice(request.surface_triangles)?,
            context.slice(request.line_points)?,
            context.slice(request.line_counts)?,
            context.slice(request.spots)?,
        )
    };
    let mut lines = Vec::with_capacity(line_counts.len());
    let mut at = 0_usize;
    for &count in line_counts {
        let end = (count as usize)
            .checked_mul(3)
            .and_then(|n| n.checked_add(at))
            .ok_or(Failure::Argument("TerrainOutlineLines"))?;
        lines.push(
            line_points
                .get(at..end)
                .ok_or(Failure::Argument("TerrainOutlineLines"))?,
        );
        at = end;
    }
    if at != line_points.len() {
        return Err(Failure::Argument("TerrainOutlineLines"));
    }
    let effect = match request.effect {
        1 => Effect::Cut,
        2 => Effect::Fill,
        3 => Effect::Both,
        _ => return Err(Failure::Argument("TerrainOutlineEffect")),
    };
    let built = build(&OutlineRequest {
        points,
        triangles,
        region,
        surface_points,
        surface_triangles,
        breaklines: &lines,
        spots,
        grading: Grading {
            cut: slope(request.cut)?,
            fill: slope(request.fill)?,
            limit: request.limit,
            rounding: request.rounding,
            effect,
            tolerance: request.tolerance,
        },
    })
    .map_err(|error| Failure::Argument(error.message()))?;
    Ok((built, target))
}

// SAFETY: target ranges were preflighted disjoint from every input, the error
// buffer and each other.
unsafe fn publish(built: &Outlined, target: TerrainOutlineOutput) -> Result<(), Failure> {
    let base: Vec<f64> = built
        .mesh
        .points
        .as_chunks::<3>()
        .0
        .iter()
        .map(|&[_, _, z]| z)
        .collect();
    let counts = TerrainOutlineCounts {
        points: built.points.len(),
        triangles: built.mesh.triangles.len(),
        ring: built.mesh.ring.len(),
        unreached: built.unreached,
        reserved: 0,
    };
    if target.query_only == 0
        && (target.point_capacity < counts.points
            || target.base_capacity < base.len()
            || target.triangle_capacity < counts.triangles
            || target.ring_capacity < counts.ring)
    {
        return Err(Failure::Argument("TerrainOutputTooSmall"));
    }
    // SAFETY: every capacity was verified before ANY write; POD copies cannot unwind.
    unsafe {
        if target.query_only == 0 {
            super::hybrid::copy(&built.points, target.points);
            super::hybrid::copy(&base, target.base);
            super::hybrid::copy(&built.mesh.triangles, target.triangles);
            super::hybrid::copy(&built.mesh.ring, target.ring);
            target.site.write(volumes(built.volumes.site));
            target.context.write(volumes(built.volumes.context));
        }
        target.counts.write(counts);
    }
    Ok(())
}

/// Grade a base mesh inside a region ring from a paved area's TIN.
/// Query mode computes the same result and publishes only counts.
///
/// # Safety
/// Same live/aligned/disjoint/unchanged-on-refusal contract as
/// `tapioca_terrain_triangulate`; refusals are ARG with the kernel's stable code.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_terrain_outline_build(
    input: *const TerrainOutlineRequest,
    output: *const TerrainOutlineOutput,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: call validates error storage and contains panics; full preflight
    // precedes computation and capacity checks precede every write.
    unsafe {
        call(error, capacity, || {
            let (built, target) =
                compute(input, output, memory_range(error.cast_const(), capacity)?)?;
            publish(&built, target)
        })
    }
}
