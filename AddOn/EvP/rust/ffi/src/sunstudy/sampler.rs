//! `tapioca_sunstudy_sample_grid`: the triangle-domain sampler.
//!
//! The sample count is known only after sampling, so the call is used twice:
//! once with zero capacities for the summary, then with buffers sized from it.
//! Each call samples from scratch; a handle that keeps the grid replaces the
//! second pass when the store moves to Rust.

use super::{Regions, flag};
use crate::borrow::SliceView;
use crate::boundary::{Failure, call};
use tapioca_sunstudy::sampler::{self, FaceLayout, SampleGrid, SamplerOptions};

/// Sampler inputs. Empty `groups`, `sample_group` or `sample_face` mean none.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SampleGridRequest {
    /// xyz f64 world metres.
    pub vertices: SliceView<f64>,
    /// Index triples.
    pub triangles: SliceView<u32>,
    /// One group id per face, or empty.
    pub groups: SliceView<u32>,
    /// Per group, 0 = skip.
    pub sample_group: SliceView<u8>,
    /// Per face, 0 = skip.
    pub sample_face: SliceView<u8>,
    /// Cell pitch, metres.
    pub spacing: f64,
    /// Lift along the normal, metres.
    pub normal_offset: f64,
    /// Jitter fraction of a cell.
    pub jitter: f64,
    /// Refusal ceiling on the sample count.
    pub max_samples: usize,
    /// Added to the face index for the jitter seed.
    pub jitter_face_offset: u32,
    /// 1 to produce layouts and cell coordinates, else 0.
    pub want_layouts: u8,
}

/// One face's cell lattice in C layout.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct FaceLayoutRecord {
    /// First corner.
    pub origin: [f64; 3],
    /// Unit u axis.
    pub u_axis: [f64; 3],
    /// Unit v axis.
    pub v_axis: [f64; 3],
    /// First cell along u.
    pub u_start: i64,
    /// First cell along v.
    pub v_start: i64,
    /// Cells along u.
    pub columns: u32,
    /// Cells along v.
    pub rows: u32,
    /// 1 when the face has a cell lattice.
    pub gridded: u8,
}

impl From<&FaceLayout> for FaceLayoutRecord {
    fn from(l: &FaceLayout) -> Self {
        Self {
            origin: l.origin,
            u_axis: l.u_axis,
            v_axis: l.v_axis,
            u_start: l.u_start,
            v_start: l.v_start,
            columns: l.columns,
            rows: l.rows,
            gridded: u8::from(l.gridded),
        }
    }
}

/// Counts and the validity flag; written by every successful call.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SampleGridSummary {
    /// Samples emitted (0 when refused).
    pub sample_count: usize,
    /// Faces in the request (complete triples).
    pub face_count: usize,
    /// Degenerate or out-of-range faces.
    pub degenerate_faces: usize,
    /// Faces skipped by the filters.
    pub excluded_faces: usize,
    /// Faces that emitted a centroid.
    pub undersized_faces: usize,
    /// 1 when the grid is complete, 0 when refused.
    pub valid: u8,
}

/// Caller-owned outputs. With `sample_capacity == 0` only the summary is
/// written; otherwise every per-sample array must hold `sample_count` entries
/// (positions/normals three each), and with layouts requested the cell arrays
/// too, and `layouts` must hold `face_count` records.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SampleGridOutput {
    /// xyz per sample.
    pub positions: *mut f64,
    /// xyz per sample.
    pub normals: *mut f64,
    /// One per sample.
    pub areas: *mut f64,
    /// One per sample.
    pub faces: *mut u32,
    /// One per sample.
    pub groups: *mut u32,
    /// One per sample (layouts only; else may be null).
    pub cell_columns: *mut u32,
    /// One per sample (layouts only; else may be null).
    pub cell_rows: *mut u32,
    /// Capacity in samples.
    pub sample_capacity: usize,
    /// One per face (layouts only).
    pub layouts: *mut FaceLayoutRecord,
    /// Capacity in faces.
    pub layout_capacity: usize,
    /// Required summary record.
    pub summary: *mut SampleGridSummary,
}

fn build(
    request: &SampleGridRequest,
    inputs: [&[u8]; 2],
    vertices: &[f64],
    triangles: &[u32],
    groups: &[u32],
) -> Result<SampleGrid, Failure> {
    let [sample_group, sample_face] = inputs;
    let options = SamplerOptions {
        spacing: request.spacing,
        normal_offset: request.normal_offset,
        jitter: request.jitter,
        jitter_face_offset: request.jitter_face_offset,
        max_samples: request.max_samples,
        want_layouts: flag(
            request.want_layouts,
            "SampleGridWantLayouts: expected 0 or 1",
        )?,
        sample_group: (!sample_group.is_empty()).then_some(sample_group),
        sample_face: (!sample_face.is_empty()).then_some(sample_face),
    };
    let groups = (!groups.is_empty()).then_some(groups);
    Ok(sampler::build_sample_grid(
        vertices, triangles, groups, &options,
    ))
}

// SAFETY: destination holds at least `source.len()` elements, checked disjoint.
unsafe fn publish<T: Copy>(source: &[T], destination: *mut T) {
    if !source.is_empty() {
        // SAFETY: forwarded from the caller.
        unsafe { std::ptr::copy_nonoverlapping(source.as_ptr(), destination, source.len()) };
    }
}

/// Grid every selected face of a triangle soup.
///
/// # Safety
/// The request, its arrays and the output descriptor must be live, initialized
/// and immutable. Every output array at its declared capacity and the summary
/// must be live, aligned, exclusively writable and disjoint from every other
/// region and the error buffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_sample_grid(
    request: *const SampleGridRequest,
    output: *const SampleGridOutput,
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
            let samples = target.sample_capacity;
            let scalars = samples
                .checked_mul(3)
                .ok_or(Failure::Argument("SampleGridCapacity"))?;
            regions.output(target.positions, scalars);
            regions.output(target.normals, scalars);
            regions.output(target.areas, samples);
            regions.output(target.faces, samples);
            regions.output(target.groups, samples);
            let want_layouts = request.want_layouts == 1;
            let cells = if want_layouts { samples } else { 0 };
            regions.output(target.cell_columns, cells);
            regions.output(target.cell_rows, cells);
            regions.output(target.layouts, target.layout_capacity);
            regions.output(target.summary, 1);
            regions.check()?;
            let context = regions.context();
            let grid = build(
                &request,
                [
                    context.slice(request.sample_group)?,
                    context.slice(request.sample_face)?,
                ],
                context.slice(request.vertices)?,
                context.slice(request.triangles)?,
                context.slice(request.groups)?,
            )?;
            let face_count = request.triangles.count / 3;
            let count = if grid.valid { grid.count() } else { 0 };
            if samples != 0 && grid.valid {
                if samples < count || (want_layouts && target.layout_capacity < face_count) {
                    return Err(Failure::Argument("SampleGridOutputTooSmall"));
                }
                publish(&grid.positions, target.positions);
                publish(&grid.normals, target.normals);
                publish(&grid.areas, target.areas);
                publish(&grid.faces, target.faces);
                publish(&grid.groups, target.groups);
                if want_layouts {
                    publish(&grid.cell_columns, target.cell_columns);
                    publish(&grid.cell_rows, target.cell_rows);
                    let layouts: Vec<FaceLayoutRecord> =
                        grid.layouts.iter().map(FaceLayoutRecord::from).collect();
                    publish(&layouts, target.layouts);
                }
            }
            target.summary.write(SampleGridSummary {
                sample_count: count,
                face_count,
                degenerate_faces: grid.degenerate_faces,
                excluded_faces: grid.excluded_faces,
                undersized_faces: grid.undersized_faces,
                valid: u8::from(grid.valid),
            });
            Ok(())
        })
    }
}
