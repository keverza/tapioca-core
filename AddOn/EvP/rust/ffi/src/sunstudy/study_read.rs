//! The study handle's read paths. Every sized read is shape-then-fill: called
//! with zero capacities it reports the sizes in its summary and writes no
//! array; called with capacities at least those sizes it fills them. A domain
//! refusal is a `status` (1 no triangle atlas, 2 no patch atlas, 3 a patch
//! the atlas did not place), never a failure, and writes no array.

use super::Regions;
use super::atlas::{AtlasTileRecord, publish};
use super::sampler::FaceLayoutRecord;
use super::study::{StudyHandle, failure, study};
use crate::boundary::{Failure, call};
use tapioca_sunstudy::study::{AtlasImage, StudyDomain, StudyError};
use tapioca_sunstudy::summary::SunStudyResultSummary;

fn status_of(error: StudyError) -> Result<u8, Failure> {
    match error {
        StudyError::NoTriangleAtlas => Ok(1),
        StudyError::NoPatchAtlas => Ok(2),
        StudyError::UnplacedPatch => Ok(3),
        other => Err(failure(other)),
    }
}

fn fits(capacity: usize, needed: usize) -> Result<bool, Failure> {
    match capacity {
        0 => Ok(false),
        c if c >= needed => Ok(true),
        _ => Err(Failure::Argument("StudyReadOutputTooSmall")),
    }
}

/// The study's shape and metadata.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct StudyInfo {
    /// Samples.
    pub sample_count: usize,
    /// Steps in the day.
    pub step_count: usize,
    /// Mesh-map entries (0 when unknown).
    pub sample_mesh_count: usize,
    /// Reused samples and sample x steps.
    pub reuse: [usize; 2],
    /// Faces a display draws (0 without an atlas).
    pub display_faces: usize,
    /// Grid spacing, metres.
    pub spacing: f64,
    /// The day's possible daylight, hours.
    pub daylight_hours: f64,
    /// Analysed area, m^2 (sample-order sum).
    pub analysed_area: f64,
    /// Display atlas width and height (0 without an atlas).
    pub display_size: [u32; 2],
    /// 0 ground, 1 triangle, 2 patch.
    pub domain: u8,
    /// `DisplayInfo` status (0 ok, 1 no model-surface atlas).
    pub display_status: u8,
}

/// Read the study's shape and metadata.
///
/// # Safety
/// The handle must be live; `out` writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_study_info(
    handle: *const StudyHandle,
    out: *mut StudyInfo,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; the output is checked first.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.output(out, 1);
            regions.check()?;
            let core = study(handle)?;
            let (sample_count, analysed_area) = core.footprint();
            let reuse = core.reuse();
            let mut info = StudyInfo {
                sample_count,
                step_count: core.series().step_count(),
                sample_mesh_count: core.sample_meshes().len(),
                reuse: [reuse.reused_samples, reuse.reused_sample_steps],
                spacing: core.spacing(),
                daylight_hours: core.series().daylight_hours(),
                analysed_area,
                domain: match core.domain() {
                    StudyDomain::Ground { .. } => 0,
                    StudyDomain::Triangle(_) => 1,
                    StudyDomain::Patch(_) => 2,
                },
                ..StudyInfo::default()
            };
            match core.display_info() {
                Ok(d) => {
                    info.display_size = [d.width, d.height];
                    info.display_faces = d.faces;
                }
                Err(e) => info.display_status = status_of(e)?,
            }
            out.write(info);
            Ok(())
        })
    }
}

/// Destinations for per-sample arrays (each capacity 0 to skip).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StudyArraysOutput {
    /// Sun hours per sample.
    pub hours: *mut f64,
    /// Capacity in samples.
    pub hours_capacity: usize,
    /// One byte per (sample, step), sample-major.
    pub step_bits: *mut u8,
    /// Capacity in bytes.
    pub step_bits_capacity: usize,
    /// Mesh index per sample.
    pub sample_meshes: *mut u32,
    /// Capacity in entries.
    pub sample_meshes_capacity: usize,
}

/// Copy hours, step bits and/or the sample-to-mesh map out.
///
/// # Safety
/// The handle must be live; the descriptor live; every non-skipped
/// destination writable for its capacity and disjoint from every other region.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_study_arrays(
    handle: *const StudyHandle,
    output: *const StudyArraysOutput,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(output, 1);
            regions.check()?;
            let t = regions.context().record(output)?;
            regions.output(t.hours, t.hours_capacity);
            regions.output(t.step_bits, t.step_bits_capacity);
            regions.output(t.sample_meshes, t.sample_meshes_capacity);
            regions.check()?;
            let core = study(handle)?;
            if t.hours_capacity != 0 {
                let hours = core.sun_hours().map_err(failure)?;
                if fits(t.hours_capacity, hours.len())? {
                    publish(&hours, t.hours);
                }
            }
            if t.step_bits_capacity != 0 {
                let bits = core.step_bits().map_err(failure)?;
                if fits(t.step_bits_capacity, bits.len())? {
                    publish(&bits, t.step_bits);
                }
            }
            let meshes = core.sample_meshes();
            if fits(t.sample_meshes_capacity, meshes.len())? {
                publish(meshes, t.sample_meshes);
            }
            Ok(())
        })
    }
}

/// The result summary (cached per generation and resolved steps).
///
/// # Safety
/// The handle must be live; `out` writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_study_summary(
    handle: *const StudyHandle,
    out: *mut SunStudyResultSummary,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; the output is checked first.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.output(out, 1);
            regions.check()?;
            out.write(study(handle)?.summary().map_err(failure)?);
            Ok(())
        })
    }
}

/// Which image a display read wants.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StudyImageOutput {
    /// 0 triangle atlas image, 1 patch atlas image, 2 display data (per-face
    /// tiles and layouts with the image of the study's own domain).
    pub kind: u8,
    /// Texels out.
    pub image: *mut f32,
    /// Capacity in texels.
    pub image_capacity: usize,
    /// Display data only: one tile per face.
    pub tiles: *mut AtlasTileRecord,
    /// Display data only: one layout per face.
    pub layouts: *mut FaceLayoutRecord,
    /// Capacity in faces (both arrays).
    pub face_capacity: usize,
}

/// Sizes and status of an image read.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct StudyImageSummary {
    /// Texels (`width * height`).
    pub texels: usize,
    /// Faces (display data only).
    pub faces: usize,
    /// Texels across.
    pub width: u32,
    /// Texels down.
    pub height: u32,
    /// 0 ok; otherwise the refusal (see the module note).
    pub status: u8,
}

/// Read an atlas image or the display data (shape-then-fill).
///
/// # Safety
/// The handle must be live; the descriptor live; destinations writable for
/// their capacities; `summary` writable; all disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_study_image(
    handle: *const StudyHandle,
    output: *const StudyImageOutput,
    summary: *mut StudyImageSummary,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(output, 1);
            regions.output(summary, 1);
            regions.check()?;
            let t = regions.context().record(output)?;
            regions.output(t.image, t.image_capacity);
            regions.output(t.tiles, t.face_capacity);
            regions.output(t.layouts, t.face_capacity);
            regions.check()?;
            let core = study(handle)?;
            let read = match t.kind {
                0 => core.atlas_image().map(|i| (i, Vec::new(), Vec::new())),
                1 => core
                    .patch_atlas_image()
                    .map(|i| (i, Vec::new(), Vec::new())),
                2 => core.display_data().map(|d| (d.image, d.tiles, d.layouts)),
                _ => return Err(Failure::Argument("StudyImageOutput: kind")),
            };
            let (image, tiles, layouts): (AtlasImage, Vec<_>, Vec<_>) = match read {
                Ok(parts) => parts,
                Err(e) => {
                    summary.write(StudyImageSummary {
                        status: status_of(e)?,
                        ..StudyImageSummary::default()
                    });
                    return Ok(());
                }
            };
            if fits(t.image_capacity, image.image.len())? {
                publish(&image.image, t.image);
            }
            if fits(t.face_capacity, tiles.len())? {
                let tiles: Vec<AtlasTileRecord> = tiles.iter().map(Into::into).collect();
                let layouts: Vec<FaceLayoutRecord> = layouts.iter().map(Into::into).collect();
                publish(&tiles, t.tiles);
                publish(&layouts, t.layouts);
            }
            summary.write(StudyImageSummary {
                texels: image.image.len(),
                faces: tiles.len(),
                width: image.width,
                height: image.height,
                status: 0,
            });
            Ok(())
        })
    }
}

/// Destinations for the step masks.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StudyStepMaskOutput {
    /// Mask words, slice-major (`words` slices of `width * height`).
    pub masks: *mut u32,
    /// Capacity in words.
    pub mask_capacity: usize,
    /// Minutes of each step.
    pub minutes: *mut u16,
    /// Capacity in steps.
    pub minute_capacity: usize,
}

/// Sizes and status of a step-mask read.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct StudyStepMaskSummary {
    /// Mask words.
    pub mask_count: usize,
    /// Steps.
    pub minute_count: usize,
    /// Width, height, words per texel, steps, solar-noon step.
    pub shape: [u32; 5],
    /// 0 ok; otherwise the refusal.
    pub status: u8,
}

/// Read the step masks (shape-then-fill).
///
/// # Safety
/// As [`tapioca_sunstudy_study_image`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_study_step_masks(
    handle: *const StudyHandle,
    output: *const StudyStepMaskOutput,
    summary: *mut StudyStepMaskSummary,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(output, 1);
            regions.output(summary, 1);
            regions.check()?;
            let t = regions.context().record(output)?;
            regions.output(t.masks, t.mask_capacity);
            regions.output(t.minutes, t.minute_capacity);
            regions.check()?;
            let masks = match study(handle)?.step_masks() {
                Ok(m) => m,
                Err(e) => {
                    summary.write(StudyStepMaskSummary {
                        status: status_of(e)?,
                        ..StudyStepMaskSummary::default()
                    });
                    return Ok(());
                }
            };
            if fits(t.mask_capacity, masks.atlas.masks.len())? {
                publish(&masks.atlas.masks, t.masks);
            }
            if fits(t.minute_capacity, masks.minutes.len())? {
                publish(&masks.minutes, t.minutes);
            }
            let a = &masks.atlas;
            summary.write(StudyStepMaskSummary {
                mask_count: a.masks.len(),
                minute_count: masks.minutes.len(),
                shape: [a.width, a.height, a.words, a.steps, masks.noon_step],
                status: 0,
            });
            Ok(())
        })
    }
}

/// One hover reading.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct StudyReadingRecord {
    /// Hours of the chosen sample.
    pub hours: f64,
    /// The chosen sample.
    pub sample: usize,
    /// Cell column and row under the point.
    pub cell: [u32; 2],
    /// 1 when the face carries a measurement.
    pub measured: u8,
    /// 1 when the nearest sample stood in for an empty cell.
    pub nearest: u8,
}

/// A hover question: a point on one source face.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct StudyReadAtRequest {
    /// Source face of the snapshot the study ran on.
    pub face: usize,
    /// World point, metres.
    pub point: [f64; 3],
}

/// The study's value at a point on a source face.
///
/// # Safety
/// The handle must be live; `request` readable; `out` writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_study_read_at(
    handle: *const StudyHandle,
    request: *const StudyReadAtRequest,
    out: *mut StudyReadingRecord,
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
            let r = study(handle)?
                .read_at(request.face, &request.point)
                .map_err(failure)?;
            out.write(StudyReadingRecord {
                hours: r.hours,
                sample: r.sample,
                cell: [r.column, r.row],
                measured: u8::from(r.measured),
                nearest: u8::from(r.nearest),
            });
            Ok(())
        })
    }
}
