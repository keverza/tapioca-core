//! `tapioca_sunstudy_class_areas`: area-weighted sunlight classes (#278).

use super::Regions;
use crate::borrow::SliceView;
use crate::boundary::{Failure, call};
use tapioca_sunstudy::light_class::{self, ClassRefusal};

/// Classification inputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ClassAreasRequest {
    /// Hours per sample.
    pub hours: SliceView<f64>,
    /// Weight (m^2) per sample.
    pub areas: SliceView<f64>,
    /// Strictly ascending class edges, hours; class = edges reached (`H >= e`).
    pub edges: SliceView<f64>,
}

/// Totals; written by every successful call.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ClassAreasSummary {
    /// `edges.count + 1`.
    pub classes: usize,
    /// Summed weight of classified samples.
    pub evaluated_area: f64,
    /// Samples with non-finite hours or weight, or a negative weight.
    pub unclassified: usize,
}

/// Caller-owned outputs: `capacity` must be at least `edges.count + 1`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ClassAreasOutput {
    /// Summed weight per class, lowest first.
    pub areas: *mut f64,
    /// Samples per class.
    pub counts: *mut usize,
    /// Capacity in classes.
    pub capacity: usize,
    /// Required summary record.
    pub summary: *mut ClassAreasSummary,
}

/// Classify samples into area-weighted sunlight classes.
///
/// # Safety
/// The request, its arrays and the output descriptor must be live, initialized
/// and immutable. Both output arrays at `capacity` and the summary must be
/// live, aligned, exclusively writable and disjoint from every other region and
/// the error buffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_class_areas(
    request: *const ClassAreasRequest,
    output: *const ClassAreasOutput,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.input(output, 1);
            regions.check()?;
            let (request, target) = (
                regions.context().record(request)?,
                regions.context().record(output)?,
            );
            regions.input(request.hours.data, request.hours.count);
            regions.input(request.areas.data, request.areas.count);
            regions.input(request.edges.data, request.edges.count);
            regions.output(target.areas, target.capacity);
            regions.output(target.counts, target.capacity);
            regions.output(target.summary, 1);
            regions.check()?;
            let context = regions.context();
            let out = light_class::class_areas(
                context.slice(request.hours)?,
                context.slice(request.areas)?,
                context.slice(request.edges)?,
            )
            .map_err(|reason| match reason {
                ClassRefusal::Shape => Failure::Argument("ClassAreasShape"),
                ClassRefusal::Edges => Failure::Argument("ClassAreasEdges"),
            })?;
            let classes = out.areas.len();
            if target.capacity < classes {
                return Err(Failure::Argument("ClassAreasOutputTooSmall"));
            }
            // SAFETY: classes <= checked disjoint capacity.
            std::ptr::copy_nonoverlapping(out.areas.as_ptr(), target.areas, classes);
            // SAFETY: as above.
            std::ptr::copy_nonoverlapping(out.counts.as_ptr(), target.counts, classes);
            target.summary.write(ClassAreasSummary {
                classes,
                evaluated_area: out.evaluated_area,
                unclassified: out.unclassified,
            });
            Ok(())
        })
    }
}
