//! `tapioca_sunstudy_patch_grid`: the surface-patch sampler.
//!
//! Shape-then-fill like the triangle sampler: a call with zero capacities
//! returns the summary, a second call with buffers sized from it fills them.
//! Span keys cross as (group, plane, occurrence); the caller maps the group to
//! its element GUID exactly as the key did (a group past `element_of` keys on
//! the empty string).

use super::Regions;
use crate::borrow::{BorrowContext, SliceView};
use crate::boundary::{Failure, call};
use tapioca_sunstudy::patch_sampler::{
    self, PatchSampleGrid, PatchSampleSpan, PatchSamplerOptions,
};
use tapioca_sunstudy::surface_patch::SurfacePatchOptions;

/// Patch sampler inputs. Empty `groups`, `sample_group`, `sample_face` mean
/// none.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PatchGridRequest {
    /// xyz f64 world metres.
    pub vertices: SliceView<f64>,
    /// Index triples.
    pub triangles: SliceView<u32>,
    /// One group id per face, or empty.
    pub groups: SliceView<u32>,
    /// Element GUID per group (UTF-8).
    pub element_of: SliceView<SliceView<u8>>,
    /// Per group, 0 = context.
    pub sample_group: SliceView<u8>,
    /// Per face, 0 = not a receiver; also splits patches.
    pub sample_face: SliceView<u8>,
    /// Cell pitch, metres.
    pub spacing: f64,
    /// Lift along the patch normal, metres.
    pub normal_offset: f64,
    /// Coplanarity angle, degrees (C++ default 1).
    pub coplanar_degrees: f64,
    /// Plane offset tolerance, metres (C++ default 1e-3).
    pub plane_tolerance_m: f64,
    /// Refusal ceiling.
    pub max_samples: usize,
}

/// One span in C layout.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PatchSpanRecord {
    /// First sample.
    pub first: usize,
    /// Sample count.
    pub count: usize,
    /// Key plane hash.
    pub plane: u64,
    /// Key occurrence.
    pub occurrence: u32,
    /// Owning group (element index).
    pub group: u32,
    /// Lattice columns.
    pub columns: u32,
    /// Lattice rows.
    pub rows: u32,
    /// Lattice origin, u axis, v axis, normal.
    pub frame: [[f64; 3]; 4],
    /// World AABB minimum and maximum.
    pub bounds: [[f64; 3]; 2],
    /// Source-triangle area, m^2.
    pub area: f64,
}

impl From<&PatchSampleSpan> for PatchSpanRecord {
    fn from(s: &PatchSampleSpan) -> Self {
        Self {
            first: s.first,
            count: s.count,
            plane: s.key.plane,
            occurrence: s.key.occurrence,
            group: s.group,
            columns: s.columns,
            rows: s.rows,
            frame: [s.origin, s.u_axis, s.v_axis, s.normal],
            bounds: [s.bounds_min, s.bounds_max],
            area: s.area,
        }
    }
}

/// Counts and flags; written by every successful call.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PatchGridSummary {
    /// Samples (0 when refused or empty).
    pub sample_count: usize,
    /// Spans.
    pub span_count: usize,
    /// Entries of `patch_of_triangle` (the face count, or 0 when refused).
    pub face_count: usize,
    /// Always 0 from this builder, as in C++.
    pub degenerate_faces: usize,
    /// Patches that fell back to a centroid.
    pub centroid_patches: usize,
    /// Context patches.
    pub excluded_patches: usize,
    /// 1 when refused for exceeding `max_samples`.
    pub sample_limit_exceeded: u8,
    /// 1 when at least one span was produced.
    pub valid: u8,
}

/// Caller-owned outputs. Zero capacities ask for the summary only; otherwise
/// each array must hold the summary's count.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PatchGridOutput {
    /// xyz per sample.
    pub positions: *mut f64,
    /// xyz per sample.
    pub normals: *mut f64,
    /// One per sample.
    pub areas: *mut f64,
    /// One per sample.
    pub cell_columns: *mut u32,
    /// One per sample.
    pub cell_rows: *mut u32,
    /// One per sample.
    pub span_of: *mut u32,
    /// Capacity in samples.
    pub sample_capacity: usize,
    /// One per span.
    pub spans: *mut PatchSpanRecord,
    /// Capacity in spans.
    pub span_capacity: usize,
    /// One per source face.
    pub patch_of_triangle: *mut u32,
    /// Capacity in faces.
    pub face_capacity: usize,
    /// Required summary record.
    pub summary: *mut PatchGridSummary,
}

// SAFETY: destination holds at least `source.len()` elements, checked disjoint.
unsafe fn publish<T: Copy>(source: &[T], destination: *mut T) {
    if !source.is_empty() {
        // SAFETY: forwarded from the caller.
        unsafe { std::ptr::copy_nonoverlapping(source.as_ptr(), destination, source.len()) };
    }
}

// SAFETY: every view was registered and checked through `Regions`.
unsafe fn sample(
    context: &BorrowContext,
    request: &PatchGridRequest,
    elements: &[SliceView<u8>],
) -> Result<PatchSampleGrid, Failure> {
    // SAFETY: forwarded from the caller's checked registration.
    unsafe {
        let names = elements
            .iter()
            .map(|text| context.text(*text))
            .collect::<Result<Vec<_>, _>>()?;
        let groups = context.slice(request.groups)?;
        let sample_group = context.slice(request.sample_group)?;
        let sample_face = context.slice(request.sample_face)?;
        let options = PatchSamplerOptions {
            spacing: request.spacing,
            normal_offset: request.normal_offset,
            max_samples: request.max_samples,
            patch: SurfacePatchOptions {
                coplanar_degrees: request.coplanar_degrees,
                plane_tolerance_m: request.plane_tolerance_m,
            },
            sample_group: (!sample_group.is_empty()).then_some(sample_group),
            sample_face: (!sample_face.is_empty()).then_some(sample_face),
        };
        Ok(patch_sampler::build_patch_sample_grid(
            context.slice(request.vertices)?,
            context.slice(request.triangles)?,
            (!groups.is_empty()).then_some(groups),
            &names,
            &options,
        ))
    }
}

/// Build the surface-patch sample set.
///
/// # Safety
/// The request, its arrays and texts and the output descriptor must be live,
/// initialized and immutable. Every output array at its declared capacity and
/// the summary must be live, aligned, exclusively writable and disjoint from
/// every other region and the error buffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_patch_grid(
    request: *const PatchGridRequest,
    output: *const PatchGridOutput,
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
            regions.input(request.vertices.data, request.vertices.count);
            regions.input(request.triangles.data, request.triangles.count);
            regions.input(request.groups.data, request.groups.count);
            regions.input(request.sample_group.data, request.sample_group.count);
            regions.input(request.sample_face.data, request.sample_face.count);
            let elements = regions.text_list(request.element_of)?;
            let samples = target.sample_capacity;
            let scalars = samples
                .checked_mul(3)
                .ok_or(Failure::Argument("PatchGridCapacity"))?;
            regions.output(target.positions, scalars);
            regions.output(target.normals, scalars);
            regions.output(target.areas, samples);
            regions.output(target.cell_columns, samples);
            regions.output(target.cell_rows, samples);
            regions.output(target.span_of, samples);
            regions.output(target.spans, target.span_capacity);
            regions.output(target.patch_of_triangle, target.face_capacity);
            regions.output(target.summary, 1);
            regions.check()?;
            let context = regions.context();
            let grid = sample(&context, &request, &elements)?;
            let filling = samples != 0 || target.span_capacity != 0 || target.face_capacity != 0;
            if filling {
                if samples < grid.count()
                    || target.span_capacity < grid.spans.len()
                    || target.face_capacity < grid.patch_of_triangle.len()
                {
                    return Err(Failure::Argument("PatchGridOutputTooSmall"));
                }
                publish(&grid.positions, target.positions);
                publish(&grid.normals, target.normals);
                publish(&grid.areas, target.areas);
                publish(&grid.cell_columns, target.cell_columns);
                publish(&grid.cell_rows, target.cell_rows);
                publish(&grid.span_of, target.span_of);
                let spans: Vec<PatchSpanRecord> =
                    grid.spans.iter().map(PatchSpanRecord::from).collect();
                publish(&spans, target.spans);
                publish(&grid.patch_of_triangle, target.patch_of_triangle);
            }
            target.summary.write(PatchGridSummary {
                sample_count: grid.count(),
                span_count: grid.spans.len(),
                face_count: grid.patch_of_triangle.len(),
                degenerate_faces: grid.degenerate_faces,
                centroid_patches: grid.centroid_patches,
                excluded_patches: grid.excluded_patches,
                sample_limit_exceeded: u8::from(grid.sample_limit_exceeded),
                valid: u8::from(grid.valid),
            });
            Ok(())
        })
    }
}
