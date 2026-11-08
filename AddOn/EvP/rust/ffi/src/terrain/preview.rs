//! Unified landscape field preview ABI v1 (#175): the same request as
//! `tapioca_terrain_unified_build`, previewed from its field on a plan grid --
//! a drawable grid surface, contour lines and cut/fill -- with no mesh.
//!
//! One call: the caller passes capacities (its previous preview's counts, say);
//! the counts are always written, the rest only when every capacity suffices
//! (`complete == 1`). Too small is not a refusal: the caller retries with the
//! counts. A refusal leaves every output unchanged. No pointer, Rust allocation
//! or handle is retained.

use super::hybrid::copy;
use super::unified::{UnifiedRequest, evaluate, request_pools};
use super::{BorrowContext, Failure, call, memory_range};
use crate::pools::{self, Checked, Range};
use tapioca_geometry::terrain::{self as kernel};

/// Counts of one preview, written on every successful call.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PreviewCounts {
    /// Surface xyz scalars.
    pub points: usize,
    /// Surface triangle index scalars.
    pub triangles: usize,
    /// Contour lines.
    pub lines: usize,
    /// Contour xyz scalars, all lines.
    pub line_points: usize,
    /// 1 when every array below was written, 0 when a capacity was too small.
    pub complete: u32,
    /// Zero.
    pub reserved: u32,
}

/// The preview's scalars, written with the arrays.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PreviewSummary {
    /// Contour interval used, metres.
    pub interval: f64,
    /// Grid spacing sampled, metres.
    pub step: f64,
    /// Surface node spacing, metres.
    pub surface_step: f64,
    /// Cut volume, cubic metres (positive).
    pub cut: f64,
    /// Fill volume, cubic metres.
    pub fill: f64,
    /// Lowest and highest design heights sampled (`[inf, -inf]` when none).
    pub range: [f64; 2],
    /// The outline faces' reach used, metres (zero without an outline).
    pub reach: f64,
    /// Grid nodes where the outline's faces ran out before the ground, at `reach`.
    pub unreached: u32,
    /// Property-ring samples where the outline's faces end in a step.
    pub clipped: u32,
}

/// One contour line.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PreviewLine {
    /// Level, metres.
    pub level: f64,
    /// First xyz triple in the line point pool.
    pub first: u64,
    /// Points.
    pub count: u64,
    /// 1 when the line closes on itself.
    pub closed: u32,
    /// Zero.
    pub reserved: u32,
}

/// The asked grid and interval, and caller-owned disjoint result storage.
/// `counts` and `summary` are required.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PreviewOutput {
    /// Grid spacing asked, metres (finite and positive; coarsened on a large area).
    pub step: f64,
    /// Contour interval asked, metres; zero or less picks one.
    pub interval: f64,
    /// Widest outline reach to widen to while faces run out before the ground, metres; at or
    /// below the outline's own limit, no widening.
    pub max_reach: f64,
    /// Required counts record.
    pub counts: *mut PreviewCounts,
    /// Required scalars record.
    pub summary: *mut PreviewSummary,
    /// Surface xyz.
    pub points: *mut f64,
    /// Scalar capacity.
    pub point_capacity: usize,
    /// Surface triangle indices.
    pub triangles: *mut u32,
    /// Scalar capacity.
    pub triangle_capacity: usize,
    /// Contour lines.
    pub lines: *mut PreviewLine,
    /// Row capacity.
    pub line_capacity: usize,
    /// Contour xyz, every line's in order.
    pub line_points: *mut f64,
    /// Scalar capacity.
    pub line_point_capacity: usize,
}

fn writable(target: PreviewOutput) -> Vec<Checked> {
    vec![
        memory_range(target.counts.cast_const(), 1),
        memory_range(target.summary.cast_const(), 1),
        memory_range(target.points.cast_const(), target.point_capacity),
        memory_range(target.triangles.cast_const(), target.triangle_capacity),
        memory_range(target.lines.cast_const(), target.line_capacity),
        memory_range(target.line_points.cast_const(), target.line_point_capacity),
    ]
}

/// The preview flattened for publication.
struct Encoded {
    summary: PreviewSummary,
    points: Vec<f64>,
    triangles: Vec<u32>,
    lines: Vec<PreviewLine>,
    line_points: Vec<f64>,
}

fn encode(preview: kernel::preview::Preview) -> Result<Encoded, Failure> {
    let mut lines = Vec::with_capacity(preview.contours.lines.len());
    let mut line_points = Vec::new();
    for line in &preview.contours.lines {
        let first = u64::try_from(line_points.len() / 3)
            .map_err(|_| Failure::Argument("TerrainPreviewSize"))?;
        let count = u64::try_from(line.points.len())
            .map_err(|_| Failure::Argument("TerrainPreviewSize"))?;
        line_points.extend(line.points.iter().flatten());
        lines.push(PreviewLine {
            level: line.level,
            first,
            count,
            closed: u32::from(line.closed),
            reserved: 0,
        });
    }
    Ok(Encoded {
        summary: PreviewSummary {
            interval: preview.contours.interval,
            step: preview.contours.step,
            surface_step: preview.surface_step,
            cut: preview.cut,
            fill: preview.fill,
            range: preview.contours.range,
            reach: preview.reach,
            unreached: preview.unreached,
            clipped: preview.clipped,
        },
        points: preview.points,
        triangles: preview.triangles,
        lines,
        line_points,
    })
}

// SAFETY: as `unified::compute`; the output descriptor's ranges are preflighted
// disjoint from every input pool, the error buffer and each other.
unsafe fn compute(
    input: *const UnifiedRequest,
    output: *const PreviewOutput,
    error: Range,
) -> Result<(Encoded, PreviewOutput), Failure> {
    // SAFETY: descriptors validates both POD shapes before copying them.
    let (request, target, context): (_, _, BorrowContext) =
        unsafe { pools::descriptors(input, output, error)? };
    let mut inputs = vec![memory_range(input, 1), memory_range(output, 1)];
    if let Some(request) = &request {
        inputs.extend(request_pools(request));
    }
    let outputs = target.map_or_else(Vec::new, writable);
    pools::preflight(&inputs, &outputs, error)?;
    let request = request.ok_or(Failure::Argument("TerrainInputDescriptor"))?;
    let target = target.ok_or(Failure::Argument("TerrainOutputDescriptor"))?;
    if !target.step.is_finite()
        || target.step <= 0.0
        || !target.interval.is_finite()
        || !target.max_reach.is_finite()
    {
        return Err(Failure::Argument("TerrainPreviewParameters"));
    }
    let (step, interval, max_reach) = (target.step, target.interval, target.max_reach);
    // SAFETY: all pools were preflighted above; no borrow escapes this call.
    let preview = unsafe {
        evaluate(&context, &request, |baseline, plan| {
            kernel::field_preview(baseline, plan, step, interval, max_reach)
        })
    }?;
    Ok((encode(preview)?, target))
}

// SAFETY: target ranges were preflighted disjoint from every input, the error
// buffer and each other; every capacity is checked before an array write.
unsafe fn publish(data: &Encoded, target: PreviewOutput) {
    let complete = target.point_capacity >= data.points.len()
        && target.triangle_capacity >= data.triangles.len()
        && target.line_capacity >= data.lines.len()
        && target.line_point_capacity >= data.line_points.len();
    let counts = PreviewCounts {
        points: data.points.len(),
        triangles: data.triangles.len(),
        lines: data.lines.len(),
        line_points: data.line_points.len(),
        complete: u32::from(complete),
        reserved: 0,
    };
    // SAFETY: every capacity was verified before an array write; POD copies cannot unwind.
    unsafe {
        if complete {
            copy(&data.points, target.points);
            copy(&data.triangles, target.triangles);
            copy(&data.lines, target.lines);
            copy(&data.line_points, target.line_points);
            target.summary.write(data.summary);
        }
        target.counts.write(counts);
    }
}

/// Preview one terrain design from its field (#175): the unified build's request,
/// sampled on a plan grid instead of meshed.
///
/// # Safety
/// Same live/aligned/disjoint/unchanged-on-refusal contract as
/// `tapioca_terrain_unified_build`; see the module documentation for the
/// one-call capacity protocol.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_terrain_unified_preview(
    input: *const UnifiedRequest,
    output: *const PreviewOutput,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: call validates error storage and contains panics; full preflight
    // precedes computation and capacity checks precede every write.
    unsafe {
        call(error, capacity, || {
            let (data, target) =
                compute(input, output, memory_range(error.cast_const(), capacity)?)?;
            publish(&data, target);
            Ok(())
        })
    }
}
