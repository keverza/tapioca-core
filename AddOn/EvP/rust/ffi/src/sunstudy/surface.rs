//! `BuildSurfaceSampling` behind a Rust-owned result handle.
//!
//! The result (two grids and a per-mesh layout) is large and its size is
//! known only after sampling, so the build returns a handle: the caller reads
//! the array sizes, sizes its buffers, reads, and destroys the handle. The
//! handle is immutable after the build and may be read from any thread; only
//! destroy needs exclusivity.

use super::Regions;
use super::patches::PatchSpanRecord;
use super::reuse::{CancelCallback, ReuseMeshView};
use super::sampler::FaceLayoutRecord;
use crate::borrow::{BorrowContext, SliceView};
use crate::boundary::{Failure, call, memory_range};
use tapioca_sunstudy::patch_sampler::{PatchKey, PatchSampleGrid, PatchSampleSpan};
use tapioca_sunstudy::sampler::{FaceLayout, SampleGrid};
use tapioca_sunstudy::surface_sampling::{
    PreviousSampling, SamplingDomain, SamplingMesh, SurfaceMeshSampling, SurfaceSamplingLayout,
    SurfaceSamplingOptions, SurfaceSamplingRequest, SurfaceSamplingResult, build_surface_sampling,
};
use tapioca_sunstudy::winding::WindingReport;

/// Opaque result handle.
pub struct SurfaceResultHandle {
    result: SurfaceSamplingResult,
}

/// Sampling settings.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SurfaceOptionsRecord {
    /// Cell pitch, metres.
    pub spacing: f64,
    /// Lift along the normal, metres.
    pub normal_offset: f64,
    /// Triangle-domain jitter.
    pub jitter: f64,
    /// Refusal ceiling.
    pub max_samples: usize,
    /// 0 triangle, 1 patch.
    pub domain: u8,
}

/// One mesh's span of a layout. `sample_faces` is a view on input and an
/// offset/length into the read buffer on output.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct MeshSpanRecord {
    /// First face, face count, first sample, sample count, first patch, patch
    /// count, degenerate, undersized, excluded.
    pub ranges: [usize; 9],
    /// Groups, closed, flipped, flipped triangles.
    pub winding: [usize; 4],
    /// Receiver mask (input view; on output `data` is null and `count` is the
    /// mask length, read from `mask_offset`).
    pub sample_faces: SliceView<u8>,
    /// Output only: offset of this mesh's mask in the read buffer.
    pub mask_offset: usize,
    /// 1 when the mesh is an analysis surface.
    pub sampled: u8,
}

/// A triangle grid by value.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SampleGridView {
    /// xyz positions.
    pub positions: SliceView<f64>,
    /// xyz normals.
    pub normals: SliceView<f64>,
    /// Areas.
    pub areas: SliceView<f64>,
    /// Source face per sample.
    pub faces: SliceView<u32>,
    /// Group per sample.
    pub groups: SliceView<u32>,
    /// Layout per face.
    pub layouts: SliceView<FaceLayoutRecord>,
    /// Cell column per sample.
    pub cell_columns: SliceView<u32>,
    /// Cell row per sample.
    pub cell_rows: SliceView<u32>,
    /// Degenerate, excluded, undersized.
    pub counters: [usize; 3],
    /// 1 when valid.
    pub valid: u8,
}

/// A patch span by value, with its element as text.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PatchSpanView {
    /// Lattice, frame, bounds, area and the key's plane and occurrence.
    pub record: PatchSpanRecord,
    /// The key's element GUID.
    pub element: SliceView<u8>,
}

/// A patch grid by value.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PatchGridView {
    /// xyz positions.
    pub positions: SliceView<f64>,
    /// xyz normals.
    pub normals: SliceView<f64>,
    /// Areas.
    pub areas: SliceView<f64>,
    /// Cell column per sample.
    pub cell_columns: SliceView<u32>,
    /// Cell row per sample.
    pub cell_rows: SliceView<u32>,
    /// Span per sample.
    pub span_of: SliceView<u32>,
    /// Spans.
    pub spans: SliceView<PatchSpanView>,
    /// Span per source face.
    pub patch_of_triangle: SliceView<u32>,
    /// Degenerate faces, centroid patches, excluded patches.
    pub counters: [usize; 3],
    /// 1 when refused for the sample limit.
    pub sample_limit_exceeded: u8,
    /// 1 when valid.
    pub valid: u8,
}

/// An earlier study.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PreviousView {
    /// Its snapshot meshes.
    pub meshes: SliceView<ReuseMeshView>,
    /// Its layout settings.
    pub options: SurfaceOptionsRecord,
    /// Its per-mesh spans.
    pub spans: SliceView<MeshSpanRecord>,
    /// Its triangle grid.
    pub sample_grid: SampleGridView,
    /// Its patch grid.
    pub patch_grid: PatchGridView,
    /// 1 when its layout is valid.
    pub layout_valid: u8,
    /// Its domain.
    pub domain: u8,
}

/// Build inputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SurfaceBuildRequest {
    /// Snapshot meshes.
    pub meshes: SliceView<ReuseMeshView>,
    /// Per mesh, 1 = analysis surface.
    pub sample_mask: SliceView<u8>,
    /// Per mesh receiver masks; empty for none, an empty entry for every face.
    pub face_masks: SliceView<SliceView<u8>>,
    /// Settings.
    pub options: SurfaceOptionsRecord,
    /// The earlier study (ignored unless `has_previous`).
    pub previous: PreviousView,
    /// Optional cancellation.
    pub cancel: CancelCallback,
    /// 1 when `previous` is supplied.
    pub has_previous: u8,
}

fn domain(value: u8) -> Result<SamplingDomain, Failure> {
    match value {
        0 => Ok(SamplingDomain::TriangleLegacy),
        1 => Ok(SamplingDomain::SurfacePatch),
        _ => Err(Failure::Argument("SurfaceSamplingDomain: expected 0 or 1")),
    }
}

fn options(record: &SurfaceOptionsRecord) -> Result<SurfaceSamplingOptions, Failure> {
    Ok(SurfaceSamplingOptions {
        domain: domain(record.domain)?,
        spacing: record.spacing,
        normal_offset: record.normal_offset,
        jitter: record.jitter,
        max_samples: record.max_samples,
    })
}

/// Registers every array a build reads, checking in stages so nested lists
/// are read only after their own shape was proven.
struct Reader<'r> {
    regions: &'r mut Regions,
}

impl Reader<'_> {
    // SAFETY: the list must be live and immutable for the call.
    unsafe fn list<T: Copy>(&mut self, view: SliceView<T>) -> Result<Vec<T>, Failure> {
        self.regions.input(view.data, view.count);
        self.regions.check()?;
        let context = self.regions.context();
        // SAFETY: shape and disjointness checked just above; copied by value.
        Ok(unsafe { context.slice(view)? }.to_vec())
    }

    fn meshes(&mut self, list: &[ReuseMeshView]) {
        for m in list {
            self.regions.input(m.guid.data, m.guid.count);
            self.regions.input(m.vertices.data, m.vertices.count);
            self.regions.input(m.triangles.data, m.triangles.count);
        }
    }

    fn sample_grid(&mut self, g: &SampleGridView) {
        let r = &mut *self.regions;
        r.input(g.positions.data, g.positions.count);
        r.input(g.normals.data, g.normals.count);
        r.input(g.areas.data, g.areas.count);
        r.input(g.faces.data, g.faces.count);
        r.input(g.groups.data, g.groups.count);
        r.input(g.layouts.data, g.layouts.count);
        r.input(g.cell_columns.data, g.cell_columns.count);
        r.input(g.cell_rows.data, g.cell_rows.count);
    }

    fn patch_grid(&mut self, g: &PatchGridView) {
        let r = &mut *self.regions;
        r.input(g.positions.data, g.positions.count);
        r.input(g.normals.data, g.normals.count);
        r.input(g.areas.data, g.areas.count);
        r.input(g.cell_columns.data, g.cell_columns.count);
        r.input(g.cell_rows.data, g.cell_rows.count);
        r.input(g.span_of.data, g.span_of.count);
        r.input(g.patch_of_triangle.data, g.patch_of_triangle.count);
    }
}

// SAFETY: every mesh array was registered and checked.
unsafe fn meshes_of<'a>(
    context: &'a BorrowContext,
    list: &[ReuseMeshView],
) -> Result<Vec<SamplingMesh<'a>>, Failure> {
    list.iter()
        .map(|m| {
            // SAFETY: forwarded from the caller's checked registration.
            unsafe {
                Ok(SamplingMesh {
                    guid: context.text(m.guid)?,
                    vertices: context.slice(m.vertices)?,
                    triangles: context.slice(m.triangles)?,
                })
            }
        })
        .collect()
}

fn layout_of(record: &FaceLayoutRecord) -> FaceLayout {
    FaceLayout {
        origin: record.origin,
        u_axis: record.u_axis,
        v_axis: record.v_axis,
        u_start: record.u_start,
        v_start: record.v_start,
        columns: record.columns,
        rows: record.rows,
        gridded: record.gridded != 0,
    }
}

// SAFETY: the grid's arrays were registered and checked.
unsafe fn sample_grid_of(
    context: &BorrowContext,
    g: &SampleGridView,
) -> Result<SampleGrid, Failure> {
    // SAFETY: forwarded from the caller's checked registration.
    unsafe {
        let [degenerate_faces, excluded_faces, undersized_faces] = g.counters;
        Ok(SampleGrid {
            positions: context.slice(g.positions)?.to_vec(),
            normals: context.slice(g.normals)?.to_vec(),
            areas: context.slice(g.areas)?.to_vec(),
            faces: context.slice(g.faces)?.to_vec(),
            groups: context.slice(g.groups)?.to_vec(),
            layouts: context.slice(g.layouts)?.iter().map(layout_of).collect(),
            cell_columns: context.slice(g.cell_columns)?.to_vec(),
            cell_rows: context.slice(g.cell_rows)?.to_vec(),
            degenerate_faces,
            excluded_faces,
            undersized_faces,
            valid: g.valid != 0,
        })
    }
}

// SAFETY: the grid's arrays and every span's text were registered and checked.
unsafe fn patch_grid_of(
    context: &BorrowContext,
    g: &PatchGridView,
    spans: &[PatchSpanView],
) -> Result<PatchSampleGrid, Failure> {
    // SAFETY: forwarded from the caller's checked registration.
    unsafe {
        let spans = spans
            .iter()
            .map(|s| {
                let r = &s.record;
                Ok(PatchSampleSpan {
                    key: PatchKey {
                        element: context.text(s.element)?.to_owned(),
                        plane: r.plane,
                        occurrence: r.occurrence,
                    },
                    group: r.group,
                    first: r.first,
                    count: r.count,
                    columns: r.columns,
                    rows: r.rows,
                    origin: r.frame[0],
                    u_axis: r.frame[1],
                    v_axis: r.frame[2],
                    normal: r.frame[3],
                    bounds_min: r.bounds[0],
                    bounds_max: r.bounds[1],
                    area: r.area,
                })
            })
            .collect::<Result<Vec<_>, Failure>>()?;
        let [degenerate_faces, centroid_patches, excluded_patches] = g.counters;
        Ok(PatchSampleGrid {
            positions: context.slice(g.positions)?.to_vec(),
            normals: context.slice(g.normals)?.to_vec(),
            areas: context.slice(g.areas)?.to_vec(),
            cell_columns: context.slice(g.cell_columns)?.to_vec(),
            cell_rows: context.slice(g.cell_rows)?.to_vec(),
            span_of: context.slice(g.span_of)?.to_vec(),
            spans,
            patch_of_triangle: context.slice(g.patch_of_triangle)?.to_vec(),
            degenerate_faces,
            centroid_patches,
            excluded_patches,
            sample_limit_exceeded: g.sample_limit_exceeded != 0,
            valid: g.valid != 0,
        })
    }
}

// SAFETY: every span's mask was registered and checked.
unsafe fn layout_from(
    context: &BorrowContext,
    options: SurfaceSamplingOptions,
    spans: &[MeshSpanRecord],
    valid: bool,
) -> Result<SurfaceSamplingLayout, Failure> {
    let meshes = spans
        .iter()
        .map(|s| {
            let [
                first_face,
                face_count,
                first_sample,
                sample_count,
                first_patch,
                patch_count,
                degenerate,
                undersized,
                excluded,
            ] = s.ranges;
            let [groups, closed, flipped, flipped_triangles] = s.winding;
            Ok(SurfaceMeshSampling {
                first_face,
                face_count,
                first_sample,
                sample_count,
                first_patch,
                patch_count,
                degenerate,
                undersized,
                excluded,
                sampled: s.sampled != 0,
                // SAFETY: forwarded from the caller's checked registration.
                sample_faces: unsafe { context.slice(s.sample_faces)? }.to_vec(),
                winding: WindingReport {
                    groups,
                    closed,
                    flipped,
                    flipped_triangles,
                },
            })
        })
        .collect::<Result<Vec<_>, Failure>>()?;
    Ok(SurfaceSamplingLayout {
        options,
        meshes,
        valid,
    })
}

/// Sample a snapshot (`BuildSurfaceSampling`) and return a result handle.
///
/// # Safety
/// The request and every nested array, text and the cancellation context must
/// be live and immutable for the call; `out` must be one writable pointer
/// slot, disjoint from the error buffer. Release the handle with
/// `tapioca_sunstudy_surface_destroy`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_surface_build(
    request: *const SurfaceBuildRequest,
    out: *mut *mut SurfaceResultHandle,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.output(out, 1);
            regions.check()?;
            let request = regions.context().record(request)?;
            let mut reader = Reader {
                regions: &mut regions,
            };
            let meshes = reader.list(request.meshes)?;
            reader.meshes(&meshes);
            let face_masks = reader.list(request.face_masks)?;
            for mask in &face_masks {
                reader.regions.input(mask.data, mask.count);
            }
            reader
                .regions
                .input(request.sample_mask.data, request.sample_mask.count);
            let has_previous = request.has_previous != 0;
            let p = request.previous;
            let (old_meshes, old_spans, old_patch_spans) = if has_previous {
                let old_meshes = reader.list(p.meshes)?;
                reader.meshes(&old_meshes);
                let old_spans = reader.list(p.spans)?;
                for span in &old_spans {
                    reader
                        .regions
                        .input(span.sample_faces.data, span.sample_faces.count);
                }
                reader.sample_grid(&p.sample_grid);
                reader.patch_grid(&p.patch_grid);
                let old_patch_spans = reader.list(p.patch_grid.spans)?;
                for span in &old_patch_spans {
                    reader.regions.input(span.element.data, span.element.count);
                }
                (old_meshes, old_spans, old_patch_spans)
            } else {
                (Vec::new(), Vec::new(), Vec::new())
            };
            regions.check()?;
            let context = regions.context();
            let current = meshes_of(&context, &meshes)?;
            let masks = face_masks
                .iter()
                .map(|m| Ok(context.slice(*m)?.to_vec()))
                .collect::<Result<Vec<_>, Failure>>()?;
            let previous_meshes = meshes_of(&context, &old_meshes)?;
            let previous_parts = if has_previous {
                Some((
                    layout_from(
                        &context,
                        options(&p.options)?,
                        &old_spans,
                        p.layout_valid != 0,
                    )?,
                    sample_grid_of(&context, &p.sample_grid)?,
                    patch_grid_of(&context, &p.patch_grid, &old_patch_spans)?,
                    domain(p.domain)?,
                ))
            } else {
                None
            };
            let previous =
                previous_parts
                    .as_ref()
                    .map(
                        |(layout, sample_grid, patch_grid, domain)| PreviousSampling {
                            meshes: &previous_meshes,
                            domain: *domain,
                            layout,
                            sample_grid,
                            patch_grid,
                        },
                    );
            let cancel = request.cancel;
            // The caller guarantees the poll and its context for the call; the
            // call runs inside this export's outer unsafe block.
            let poll = || cancel.is_cancelled.is_some_and(|f| f(cancel.context) != 0);
            let result = build_surface_sampling(&SurfaceSamplingRequest {
                meshes: &current,
                sample_mask: context.slice(request.sample_mask)?,
                options: options(&request.options)?,
                previous,
                face_masks: &masks,
                is_cancelled: Some(&poll),
            });
            out.write(Box::into_raw(Box::new(SurfaceResultHandle { result })));
            Ok(())
        })
    }
}

/// Release a result handle. Null is accepted.
///
/// # Safety
/// `handle` must be null or a live handle that no thread is using and that is
/// never used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_surface_destroy(
    handle: *mut SurfaceResultHandle,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; ownership returns to Rust once.
    unsafe {
        call(error, error_capacity, || {
            if !handle.is_null() {
                memory_range(handle.cast_const(), 1)?;
                // SAFETY: the caller transfers the live Box allocation back.
                drop(Box::from_raw(handle));
            }
            Ok(())
        })
    }
}

// SAFETY: `handle` must be null or a live result handle not being destroyed.
unsafe fn result<'a>(
    handle: *const SurfaceResultHandle,
) -> Result<&'a SurfaceSamplingResult, Failure> {
    memory_range(handle, 1)?;
    // SAFETY: non-null and aligned (checked); liveness is the caller's contract.
    unsafe { handle.as_ref() }
        .map(|h| &h.result)
        .ok_or(Failure::Argument("SurfaceResultHandle: null"))
}

/// Array sizes and counters of a result.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SurfaceResultSummary {
    /// Triangle samples, triangle layouts, patch samples, patch spans, patch
    /// faces (`patch_of_triangle`), layout meshes, mask bytes, span text bytes.
    pub sizes: [usize; 8],
    /// Reused meshes, rebuilt meshes, reused samples, generated samples.
    pub counts: [usize; 4],
    /// Summed winding: groups, closed, flipped, flipped triangles.
    pub winding: [usize; 4],
    /// Triangle grid counters: degenerate, excluded, undersized.
    pub triangle_counters: [usize; 3],
    /// Patch grid counters: degenerate faces, centroid patches, excluded.
    pub patch_counters: [usize; 3],
    /// The layout's settings.
    pub options: SurfaceOptionsRecord,
    /// Result, triangle grid, patch grid, layout validity; patch limit flag.
    pub flags: [u8; 5],
}

/// Read a result's sizes and counters.
///
/// # Safety
/// The handle must be live; `out` writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_surface_summary(
    handle: *const SurfaceResultHandle,
    out: *mut SurfaceResultSummary,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; the output is checked first.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.output(out, 1);
            regions.check()?;
            let res = result(handle)?;
            let (tri, pat, lay) = (&res.triangles, &res.patches, &res.layout);
            let mask_bytes = lay.meshes.iter().map(|m| m.sample_faces.len()).sum();
            let text_bytes = pat.spans.iter().map(|s| s.key.element.len()).sum();
            let opt = &lay.options;
            out.write(SurfaceResultSummary {
                sizes: [
                    tri.count(),
                    tri.layouts.len(),
                    pat.count(),
                    pat.spans.len(),
                    pat.patch_of_triangle.len(),
                    lay.meshes.len(),
                    mask_bytes,
                    text_bytes,
                ],
                counts: [
                    res.reused_meshes,
                    res.rebuilt_meshes,
                    res.reused_samples,
                    res.generated_samples,
                ],
                winding: [
                    res.winding.groups,
                    res.winding.closed,
                    res.winding.flipped,
                    res.winding.flipped_triangles,
                ],
                triangle_counters: [
                    tri.degenerate_faces,
                    tri.excluded_faces,
                    tri.undersized_faces,
                ],
                patch_counters: [
                    pat.degenerate_faces,
                    pat.centroid_patches,
                    pat.excluded_patches,
                ],
                options: SurfaceOptionsRecord {
                    spacing: opt.spacing,
                    normal_offset: opt.normal_offset,
                    jitter: opt.jitter,
                    max_samples: opt.max_samples,
                    domain: opt.domain as u8,
                },
                flags: [
                    u8::from(res.valid),
                    u8::from(tri.valid),
                    u8::from(pat.valid),
                    u8::from(lay.valid),
                    u8::from(pat.sample_limit_exceeded),
                ],
            });
            Ok(())
        })
    }
}

/// Destinations for every array of a result. Each must hold the size the
/// summary reported; xyz arrays hold three entries per sample.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SurfaceReadOutput {
    /// Triangle grid: positions, normals, areas.
    pub triangle_reals: [*mut f64; 3],
    /// Triangle grid: faces, groups, cell columns, cell rows.
    pub triangle_indices: [*mut u32; 4],
    /// Triangle grid layouts.
    pub triangle_layouts: *mut FaceLayoutRecord,
    /// Patch grid: positions, normals, areas.
    pub patch_reals: [*mut f64; 3],
    /// Patch grid: cell columns, cell rows, span per sample, span per face.
    pub patch_indices: [*mut u32; 4],
    /// Patch spans (record.group etc.; element is `text[offset..]`).
    pub spans: *mut PatchSpanRecord,
    /// Element offset per span into `text`.
    pub span_text_offsets: *mut usize,
    /// Concatenated span element GUIDs.
    pub text: *mut u8,
    /// Layout spans (masks as offsets into `masks`).
    pub mesh_spans: *mut MeshSpanRecord,
    /// Concatenated receiver masks.
    pub masks: *mut u8,
}

// SAFETY: `destination` holds `source.len()` elements, checked disjoint.
unsafe fn publish<T: Copy>(source: &[T], destination: *mut T) {
    if !source.is_empty() {
        // SAFETY: forwarded from the caller.
        unsafe { std::ptr::copy_nonoverlapping(source.as_ptr(), destination, source.len()) };
    }
}

fn span_record(s: &PatchSampleSpan) -> PatchSpanRecord {
    PatchSpanRecord {
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

/// Copy every array of a result out.
///
/// # Safety
/// The handle must be live; the descriptor live; every destination writable
/// for the size `tapioca_sunstudy_surface_summary` reported and disjoint from
/// every other region (null only where that size is zero).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_surface_read(
    handle: *const SurfaceResultHandle,
    output: *const SurfaceReadOutput,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(output, 1);
            regions.check()?;
            let target = regions.context().record(output)?;
            let r = result(handle)?;
            let (t, p, l) = (&r.triangles, &r.patches, &r.layout);
            let layouts: Vec<FaceLayoutRecord> =
                t.layouts.iter().map(FaceLayoutRecord::from).collect();
            let spans: Vec<PatchSpanRecord> = p.spans.iter().map(span_record).collect();
            let mut text = Vec::new();
            let mut offsets = Vec::with_capacity(p.spans.len());
            for span in &p.spans {
                offsets.push(text.len());
                text.extend_from_slice(span.key.element.as_bytes());
            }
            let mut masks = Vec::new();
            let mesh_spans: Vec<MeshSpanRecord> = l
                .meshes
                .iter()
                .map(|m| {
                    let offset = masks.len();
                    masks.extend_from_slice(&m.sample_faces);
                    MeshSpanRecord {
                        ranges: [
                            m.first_face,
                            m.face_count,
                            m.first_sample,
                            m.sample_count,
                            m.first_patch,
                            m.patch_count,
                            m.degenerate,
                            m.undersized,
                            m.excluded,
                        ],
                        winding: [
                            m.winding.groups,
                            m.winding.closed,
                            m.winding.flipped,
                            m.winding.flipped_triangles,
                        ],
                        sample_faces: SliceView {
                            data: std::ptr::null(),
                            count: m.sample_faces.len(),
                        },
                        mask_offset: offset,
                        sampled: u8::from(m.sampled),
                    }
                })
                .collect();
            let [tp, tn, ta] = target.triangle_reals;
            let [tf, tg, tc, tr] = target.triangle_indices;
            let [pp, pn, pa] = target.patch_reals;
            let [pc, pr, ps, pt] = target.patch_indices;
            regions.output(tp, t.positions.len());
            regions.output(tn, t.normals.len());
            regions.output(ta, t.areas.len());
            regions.output(tf, t.faces.len());
            regions.output(tg, t.groups.len());
            regions.output(tc, t.cell_columns.len());
            regions.output(tr, t.cell_rows.len());
            regions.output(target.triangle_layouts, layouts.len());
            regions.output(pp, p.positions.len());
            regions.output(pn, p.normals.len());
            regions.output(pa, p.areas.len());
            regions.output(pc, p.cell_columns.len());
            regions.output(pr, p.cell_rows.len());
            regions.output(ps, p.span_of.len());
            regions.output(pt, p.patch_of_triangle.len());
            regions.output(target.spans, spans.len());
            regions.output(target.span_text_offsets, offsets.len());
            regions.output(target.text, text.len());
            regions.output(target.mesh_spans, mesh_spans.len());
            regions.output(target.masks, masks.len());
            regions.check()?;
            publish(&t.positions, tp);
            publish(&t.normals, tn);
            publish(&t.areas, ta);
            publish(&t.faces, tf);
            publish(&t.groups, tg);
            publish(&t.cell_columns, tc);
            publish(&t.cell_rows, tr);
            publish(&layouts, target.triangle_layouts);
            publish(&p.positions, pp);
            publish(&p.normals, pn);
            publish(&p.areas, pa);
            publish(&p.cell_columns, pc);
            publish(&p.cell_rows, pr);
            publish(&p.span_of, ps);
            publish(&p.patch_of_triangle, pt);
            publish(&spans, target.spans);
            publish(&offsets, target.span_text_offsets);
            publish(&text, target.text);
            publish(&mesh_spans, target.mesh_spans);
            publish(&masks, target.masks);
            Ok(())
        })
    }
}
