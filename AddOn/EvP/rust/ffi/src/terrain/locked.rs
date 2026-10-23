//! Additive fixed-control hybrid ABI. One mandatory provenance catalog covers
//! mesh/ring inputs followed by line vertices then independent control points;
//! ring edges are followed by control segments in line order. No identity is
//! synthesized and all control contributors are returned as mapping rows.

use super::conflict::{self, TerrainRowsOutput};
use super::edit::TerrainRing;
use super::hybrid::{self, Shape, TerrainHybridOutput, TerrainHybridRequest};
use super::{Failure, SliceView, call, memory_range};
use crate::pools::{self, Range};
use tapioca_geometry::terrain::hybrid::{self as kernel, HybridError, LockRef, Locked};

/// The pinned band request plus fixed controls. Line descriptors partition
/// `line_points` in order (xyz scalar offsets/counts). Independent points follow
/// all line vertices in `band.provenance`; edges follow the ring's edges.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TerrainHybridLockedRequest {
    /// Base, region, stations and the complete combined provenance (domains=7).
    pub band: TerrainHybridRequest,
    /// All open line xyz vertices, world metres.
    pub line_points: SliceView<f64>,
    /// Ordered, contiguous scalar ranges, each with at least two xyz vertices.
    pub lines: SliceView<TerrainRing>,
    /// Independent fixed xyz points, world metres.
    pub points: SliceView<f64>,
}

/// Band diagnostics (codes 1..5) and the first fixed-control conflict (code 6).
/// For code 6 station is MAX, side 0, hard 1; the control kinds are 1 segment,
/// 2 point, 0 absent. Indices are line/point locators; segments unused for points.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TerrainHybridDiagnostic {
    /// World position at the fixed height for a fixed-control conflict.
    pub position: [f64; 3],
    /// Signed height mismatch for code 6, other magnitudes as in `TerrainConflict`.
    pub magnitude: f64,
    /// Stable code.
    pub code: u32,
    /// Band station or MAX for a control.
    pub station: u32,
    /// Band hand or 0 for a control.
    pub side: u32,
    /// Whether the build refuses.
    pub hard: u32,
    /// Candidate output vertex for code 6, MAX otherwise.
    pub vertex: u32,
    /// First control kind.
    pub control_kind: u32,
    /// First control index.
    pub control_index: u32,
    /// First control segment.
    pub control_segment: u32,
    /// Other control kind, 0 when grading disagrees.
    pub other_kind: u32,
    /// Other control index.
    pub other_index: u32,
    /// Other control segment.
    pub other_segment: u32,
    /// Zero.
    pub reserved: u32,
}

/// Caller-owned diagnostic rows; query/fill, atomic on refusal.
pub type TerrainHybridDiagnosticOutput = TerrainRowsOutput<TerrainHybridDiagnostic>;

pub(super) const fn control(value: LockRef) -> [u32; 3] {
    match value {
        LockRef::Segment { line, segment } => [1, line, segment],
        LockRef::Point(point) => [2, point, 0],
    }
}

fn lines<'a>(
    pool: &'a [f64],
    descriptors: &[TerrainRing],
) -> Result<(Vec<&'a [f64]>, u64), Failure> {
    let mut end = 0;
    let mut edges = 0_u64;
    let mut lines = Vec::new();
    for line in descriptors {
        let first = line.offset as usize;
        let count = line.count as usize;
        if first != end || count < 6 || !count.is_multiple_of(3) {
            return Err(Failure::Argument(
                "LockedParameters: control lines must partition their xyz pool",
            ));
        }
        end = first
            .checked_add(count)
            .ok_or(Failure::Argument("TerrainCountOverflow"))?;
        lines.push(
            pool.get(first..end)
                .ok_or(Failure::Argument("LockedParameters: control line range"))?,
        );
        edges = edges
            .checked_add(
                u64::try_from(count / 3 - 1)
                    .map_err(|_| Failure::Argument("TerrainCountOverflow"))?,
            )
            .ok_or(Failure::Argument("TerrainCountOverflow"))?;
    }
    if end != pool.len() {
        return Err(Failure::Argument(
            "LockedParameters: unused control line vertices",
        ));
    }
    Ok((lines, edges))
}

// SAFETY: every descriptor and complete pool/capacity is preflighted before
// any foreign borrow. Nested line records contain offsets, not pointers.
unsafe fn with_locked<O: Copy, R>(
    input: *const TerrainHybridLockedRequest,
    output: *const O,
    error: Range,
    shape: Shape<O>,
    run: impl FnOnce(&kernel::BuildRequest<'_>, Locked<'_>) -> Result<R, Failure>,
) -> Result<(R, O), Failure> {
    // SAFETY: descriptor shapes are checked before copying.
    let (request, target, context) = unsafe { pools::descriptors(input, output, error)? };
    let mut inputs = vec![memory_range(input, 1), memory_range(output, 1)];
    if let Some(r) = &request {
        inputs.extend(pools::provenance(&r.band.provenance));
        inputs.extend([
            memory_range(r.band.points.data, r.band.points.count),
            memory_range(r.band.triangles.data, r.band.triangles.count),
            memory_range(r.band.region.data, r.band.region.count),
            memory_range(r.band.stations.data, r.band.stations.count),
            memory_range(r.line_points.data, r.line_points.count),
            memory_range(r.lines.data, r.lines.count),
            memory_range(r.points.data, r.points.count),
        ]);
    }
    pools::preflight(
        &inputs,
        &target.map_or_else(Vec::new, shape.writable),
        error,
    )?;
    let request = request.ok_or(Failure::Argument("TerrainInputDescriptor"))?;
    let target = target.ok_or(Failure::Argument("TerrainOutputDescriptor"))?;
    (shape.mode)(&target)?;
    // SAFETY: complete disjointness preflight preceded every borrow.
    let (geometry, descriptors, points) = unsafe {
        (
            context.slice(request.line_points)?,
            context.slice(request.lines)?,
            context.slice(request.points)?,
        )
    };
    if !points.len().is_multiple_of(3) {
        return Err(Failure::Argument(
            "LockedParameters: whole xyz points required",
        ));
    }
    let (lines, edges) = lines(geometry, descriptors)?;
    let vertices = geometry
        .len()
        .checked_add(points.len())
        .and_then(|n| u64::try_from(n / 3).ok())
        .ok_or(Failure::Argument("TerrainCountOverflow"))?;
    // SAFETY: the band's nested pools were preflighted with the controls above.
    let data = unsafe {
        hybrid::evaluate_band(&context, &request.band, [vertices, edges], |band| {
            run(
                band,
                Locked {
                    lines: &lines,
                    points,
                },
            )
        })?
    };
    Ok((data, target))
}

/// Build the fixed-control hybrid, with the existing hybrid output record and
/// additive line kind 6 and mapping kinds 20..22. Query publishes only counts.
///
/// # Safety
/// Same caller-owned/disjoint/live-storage contract as the legacy hybrid build;
/// full-capacity preflight, caught unwind and atomic refusal apply.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_terrain_hybrid_locked_build(
    input: *const TerrainHybridLockedRequest,
    output: *const TerrainHybridOutput,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: shared envelope, then all pools preflighted before borrowing.
    unsafe {
        call(error, capacity, || {
            let shape = Shape {
                writable: hybrid::writable,
                mode: hybrid::mode,
            };
            let (data, target) = with_locked(
                input,
                output,
                memory_range(error.cast_const(), capacity)?,
                shape,
                |band, controls| {
                    hybrid::encode_build(
                        kernel::build_locked(band, controls)
                            .map_err(|e| Failure::Argument(e.message()))?,
                        true,
                    )
                },
            )?;
            hybrid::publish(&data, target)
        })
    }
}

fn assess(
    band: &kernel::BuildRequest<'_>,
    controls: Locked<'_>,
) -> Result<Vec<TerrainHybridDiagnostic>, Failure> {
    let mut rows: Vec<_> = kernel::assess(band)
        .map_err(|e| Failure::Argument(e.message()))?
        .iter()
        .map(|c| TerrainHybridDiagnostic {
            position: c.position,
            magnitude: c.magnitude,
            code: conflict::code(c.code),
            station: c.station,
            side: match c.side {
                tapioca_geometry::terrain::edit::Hand::Left => 1,
                tapioca_geometry::terrain::edit::Hand::Right => 2,
            },
            hard: u32::from(c.code.hard()),
            vertex: u32::MAX,
            ..TerrainHybridDiagnostic::default()
        })
        .collect();
    match kernel::build_locked(band, controls) {
        Ok(_) => {}
        Err(HybridError::LockedConflict(c)) => {
            let [control_kind, control_index, control_segment] = control(c.control);
            let [other_kind, other_index, other_segment] = c.other.map_or([0; 3], control);
            rows.push(TerrainHybridDiagnostic {
                position: c.position,
                magnitude: c.magnitude,
                code: 6,
                station: u32::MAX,
                side: 0,
                hard: 1,
                vertex: c.vertex,
                control_kind,
                control_index,
                control_segment,
                other_kind,
                other_index,
                other_segment,
                reserved: 0,
            });
        }
        Err(
            HybridError::DaylightNotReached { .. }
            | HybridError::Vertical { .. }
            | HybridError::DaylightTopology { .. },
        ) if rows.iter().any(|c| c.hard == 1) => {}
        Err(error) => return Err(Failure::Argument(error.message())),
    }
    Ok(rows)
}

/// Assess all station-side diagnostics and the first fixed-control conflict.
/// Control assessment requires a TIN build (worker-classified), not a cheap query.
///
/// # Safety
/// Same preflight/atomic query/fill contract as the fixed-control build.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_terrain_hybrid_locked_assess(
    input: *const TerrainHybridLockedRequest,
    output: *const TerrainHybridDiagnosticOutput,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: shared envelope and complete preflight before borrowing.
    unsafe {
        call(error, capacity, || {
            let shape = Shape {
                writable: conflict::writable,
                mode: conflict::mode,
            };
            let (rows, target) = with_locked(
                input,
                output,
                memory_range(error.cast_const(), capacity)?,
                shape,
                assess,
            )?;
            conflict::publish(&rows, target)
        })
    }
}
