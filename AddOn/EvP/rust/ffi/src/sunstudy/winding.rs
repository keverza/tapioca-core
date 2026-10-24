//! `tapioca_sunstudy_orient_outward`: per-element outward winding.

use super::Regions;
use crate::borrow::SliceView;
use crate::boundary::{Failure, call};
use tapioca_sunstudy::winding;

/// A merged triangle soup and its element grouping.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct WindingRequest {
    /// xyz f64 world metres; scalar count.
    pub vertices: SliceView<f64>,
    /// Index triples; a trailing partial triple is ignored.
    pub triangles: SliceView<u32>,
    /// One element id per face, or empty for "all one element".
    pub groups: SliceView<u32>,
    /// Weld tolerance, metres; non-positive or NaN selects 1e-4.
    pub weld_tolerance: f64,
}

/// What the proof saw and changed.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct WindingReportOutput {
    /// Distinct groups with a usable face.
    pub groups: usize,
    /// Groups proved closed.
    pub closed: usize,
    /// Groups flipped.
    pub flipped: usize,
    /// Triangles swapped.
    pub flipped_triangles: usize,
    /// Indices written to the output array (0 for empty input).
    pub index_count: usize,
}

/// Caller-owned outputs, unchanged on refusal.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct WindingOutput {
    /// Oriented index triples; null only at zero capacity.
    pub triangles: *mut u32,
    /// Capacity in indices; must hold every complete input triple.
    pub capacity: usize,
    /// Required report record.
    pub report: *mut WindingReportOutput,
}

/// Swap the winding of every closed, inward-wound element.
///
/// # Safety
/// The request, its arrays and the output descriptor must be live, initialized
/// and immutable. The output index capacity and report must be live, aligned,
/// exclusively writable and disjoint from every other region and the error
/// buffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_orient_outward(
    request: *const WindingRequest,
    output: *const WindingOutput,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: the envelope contains unwinding; every region is checked first.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.input(output, 1);
            regions.check()?;
            let context = regions.context();
            // SAFETY: descriptor shapes and error disjointness checked; POD copies.
            let (request, target) = (context.record(request)?, context.record(output)?);
            regions.input(request.vertices.data, request.vertices.count);
            regions.input(request.triangles.data, request.triangles.count);
            regions.input(request.groups.data, request.groups.count);
            regions.output(target.triangles, target.capacity);
            regions.output(target.report, 1);
            regions.check()?;
            if target.capacity < request.triangles.count / 3 * 3 {
                return Err(Failure::Argument("WindingOutputTooSmall"));
            }
            // SAFETY: checked immutable call-scoped inputs; no borrow survives.
            let (vertices, triangles, groups) = (
                context.slice(request.vertices)?,
                context.slice(request.triangles)?,
                context.slice(request.groups)?,
            );
            let groups = (!groups.is_empty()).then_some(groups);
            let (oriented, report) =
                winding::orient_outward(vertices, triangles, groups, request.weld_tolerance)
                    .map_err(|error| Failure::Argument(error.message()))?;
            // SAFETY: oriented.len() <= complete triples <= checked capacity.
            if !oriented.is_empty() {
                std::ptr::copy_nonoverlapping(oriented.as_ptr(), target.triangles, oriented.len());
            }
            target.report.write(WindingReportOutput {
                groups: report.groups,
                closed: report.closed,
                flipped: report.flipped,
                flipped_triangles: report.flipped_triangles,
                index_count: oriented.len(),
            });
            Ok(())
        })
    }
}
