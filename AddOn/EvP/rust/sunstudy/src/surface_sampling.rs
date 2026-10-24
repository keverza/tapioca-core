//! Per-mesh surface sampling with exact reuse of an earlier study's grids.
//!
//! Port of `SunStudy/SunStudySurfaceSampling.cpp`. Each mesh is oriented and
//! sampled on its own and appended in snapshot order; an unchanged mesh (same
//! canonical GUID, identical vertices and triangles, same sampling flags, same
//! jitter seed) copies its samples from the previous study instead, after the
//! previous layout has been proven to partition its grids. Ambiguous GUIDs
//! fall back to sampling the whole scene at once. No sunlight is copied here.
//! Cancellation or refusal returns no partial grid.

use crate::patch_sampler::{
    NO_PATCH, PatchSampleGrid, PatchSamplerOptions, build_patch_sample_grid,
};
use crate::roles::canonical_guid;
use crate::sampler::{SampleGrid, SamplerOptions, build_sample_grid};
use crate::winding::{DEFAULT_WELD_TOLERANCE, WindingReport, orient_outward};
use std::collections::BTreeMap;

mod copy;
use copy::{append_patches, append_triangles, can_copy, compatible, face_count};

/// Which domain a study measures. Discriminants are the C ABI values.
#[repr(u8)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum SamplingDomain {
    /// One grid per source triangle.
    #[default]
    TriangleLegacy = 0,
    /// One grid per coplanar surface.
    SurfacePatch = 1,
}

/// Sampling settings.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SurfaceSamplingOptions {
    /// Domain.
    pub domain: SamplingDomain,
    /// Cell pitch, metres.
    pub spacing: f64,
    /// Lift along the normal, metres.
    pub normal_offset: f64,
    /// Triangle-domain jitter fraction.
    pub jitter: f64,
    /// Refusal ceiling across the whole study.
    pub max_samples: usize,
}

impl Default for SurfaceSamplingOptions {
    fn default() -> Self {
        Self {
            domain: SamplingDomain::TriangleLegacy,
            spacing: 1.0,
            normal_offset: 0.01,
            jitter: 0.0,
            max_samples: 4_000_000,
        }
    }
}

/// One mesh's ranges into the study's grids (not a copy of its samples).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct SurfaceMeshSampling {
    /// First face in snapshot order.
    pub first_face: usize,
    /// Faces in the mesh.
    pub face_count: usize,
    /// First sample.
    pub first_sample: usize,
    /// Samples.
    pub sample_count: usize,
    /// First patch span.
    pub first_patch: usize,
    /// Patch spans.
    pub patch_count: usize,
    /// Degenerate faces.
    pub degenerate: usize,
    /// Undersized faces (centroid patches in the patch domain).
    pub undersized: usize,
    /// Excluded faces (patches in the patch domain).
    pub excluded: usize,
    /// Whether the mesh is an analysis surface.
    pub sampled: bool,
    /// Receiver face mask; empty means every face.
    pub sample_faces: Vec<u8>,
    /// The mesh's winding proof.
    pub winding: WindingReport,
}

/// The per-mesh partition of a study's grids.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SurfaceSamplingLayout {
    /// Settings the grids were built with.
    pub options: SurfaceSamplingOptions,
    /// One per mesh.
    pub meshes: Vec<SurfaceMeshSampling>,
    /// Whether the layout describes valid grids.
    pub valid: bool,
}

/// What a sampling produced.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SurfaceSamplingResult {
    /// Triangle-domain grid.
    pub triangles: SampleGrid,
    /// Patch-domain grid.
    pub patches: PatchSampleGrid,
    /// The per-mesh layout (empty after a whole-scene fallback).
    pub layout: SurfaceSamplingLayout,
    /// Summed winding reports.
    pub winding: WindingReport,
    /// Meshes copied from the previous study.
    pub reused_meshes: usize,
    /// Meshes sampled afresh.
    pub rebuilt_meshes: usize,
    /// Samples copied.
    pub reused_samples: usize,
    /// Samples generated.
    pub generated_samples: usize,
    /// False when refused.
    pub valid: bool,
}

/// One snapshot mesh.
#[derive(Clone, Copy, Debug)]
pub struct SamplingMesh<'a> {
    /// Element GUID, any spelling.
    pub guid: &'a str,
    /// xyz f64 vertices.
    pub vertices: &'a [f64],
    /// Index triples.
    pub triangles: &'a [u32],
}

/// An earlier study whose grids may be reused.
#[derive(Clone, Copy, Debug)]
pub struct PreviousSampling<'a> {
    /// Its snapshot meshes.
    pub meshes: &'a [SamplingMesh<'a>],
    /// Its domain.
    pub domain: SamplingDomain,
    /// Its layout.
    pub layout: &'a SurfaceSamplingLayout,
    /// Its triangle grid.
    pub sample_grid: &'a SampleGrid,
    /// Its patch grid.
    pub patch_grid: &'a PatchSampleGrid,
}

/// Inputs of one sampling.
#[derive(Clone, Copy)]
pub struct SurfaceSamplingRequest<'a> {
    /// Snapshot meshes.
    pub meshes: &'a [SamplingMesh<'a>],
    /// Per mesh, 1 = analysis surface.
    pub sample_mask: &'a [u8],
    /// Settings.
    pub options: SurfaceSamplingOptions,
    /// An earlier study to reuse from.
    pub previous: Option<PreviousSampling<'a>>,
    /// Per mesh receiver masks (empty = none; an empty entry = every face).
    pub face_masks: &'a [Vec<u8>],
    /// Polled between meshes.
    pub is_cancelled: Option<&'a dyn Fn() -> bool>,
}

fn cancelled(request: &SurfaceSamplingRequest<'_>) -> bool {
    request.is_cancelled.is_some_and(|f| f())
}

fn index_meshes(meshes: &[SamplingMesh<'_>]) -> Option<BTreeMap<String, usize>> {
    let mut index = BTreeMap::new();
    for (i, mesh) in meshes.iter().enumerate() {
        let guid = canonical_guid(mesh.guid);
        if guid.is_empty() || index.insert(guid, i).is_some() {
            return None;
        }
    }
    Some(index)
}

#[expect(
    clippy::cast_possible_truncation,
    reason = "mirrors static_cast<uint32_t>(faceOffset): the jitter seed wraps"
)]
fn wrap32(value: usize) -> u32 {
    value as u32
}

fn triangle_options(
    options: &SurfaceSamplingOptions,
    face_offset: usize,
) -> SamplerOptions<'static> {
    SamplerOptions {
        spacing: options.spacing,
        normal_offset: options.normal_offset,
        jitter: options.jitter,
        jitter_face_offset: wrap32(face_offset),
        max_samples: options.max_samples,
        want_layouts: true,
        sample_group: None,
        sample_face: None,
    }
}

fn patch_options(options: &SurfaceSamplingOptions) -> PatchSamplerOptions<'static> {
    PatchSamplerOptions {
        spacing: options.spacing,
        normal_offset: options.normal_offset,
        max_samples: options.max_samples,
        ..PatchSamplerOptions::default()
    }
}

/// Sample the whole scene at once (the ambiguous-GUID fallback).
fn fresh_scene(request: &SurfaceSamplingRequest<'_>) -> SurfaceSamplingResult {
    let mut result = SurfaceSamplingResult::default();
    let (mut vertices, mut triangles, mut groups, mut elements, mut faces) =
        (Vec::new(), Vec::new(), Vec::new(), Vec::new(), Vec::new());
    for (index, mesh) in request.meshes.iter().enumerate() {
        let base = u32::try_from(vertices.len() / 3).unwrap_or(u32::MAX);
        vertices.extend_from_slice(mesh.vertices);
        triangles.extend(mesh.triangles.iter().map(|v| base.wrapping_add(*v)));
        groups.resize(
            triangles.len() / 3,
            u32::try_from(index).unwrap_or(u32::MAX),
        );
        elements.push(mesh.guid);
        if !request.face_masks.is_empty() {
            match request.face_masks.get(index) {
                Some(mask) if !mask.is_empty() => faces.extend_from_slice(mask),
                _ => faces.extend(std::iter::repeat_n(1, face_count(mesh))),
            }
        }
    }
    let Ok((oriented, winding)) =
        orient_outward(&vertices, &triangles, Some(&groups), DEFAULT_WELD_TOLERANCE)
    else {
        return SurfaceSamplingResult::default();
    };
    result.winding = winding;
    let face_filter = (!request.face_masks.is_empty()).then_some(faces.as_slice());
    if request.options.domain == SamplingDomain::SurfacePatch {
        let settings = PatchSamplerOptions {
            sample_group: Some(request.sample_mask),
            sample_face: face_filter,
            ..patch_options(&request.options)
        };
        result.patches =
            build_patch_sample_grid(&vertices, &oriented, Some(&groups), &elements, &settings);
        result.valid = result.patches.valid;
        result.generated_samples = result.patches.count();
    } else {
        let settings = SamplerOptions {
            sample_group: Some(request.sample_mask),
            sample_face: face_filter,
            ..triangle_options(&request.options, 0)
        };
        result.triangles = build_sample_grid(&vertices, &oriented, Some(&groups), &settings);
        result.valid = result.triangles.valid;
        result.generated_samples = result.triangles.count();
    }
    result.rebuilt_meshes = request.meshes.len();
    if result.valid {
        result
    } else {
        SurfaceSamplingResult::default()
    }
}

fn same_mesh(a: &SamplingMesh<'_>, b: &SamplingMesh<'_>) -> bool {
    a.vertices == b.vertices && a.triangles == b.triangles
}

fn inputs_valid(request: &SurfaceSamplingRequest<'_>) -> bool {
    let o = &request.options;
    o.spacing > 0.0
        && o.spacing.is_finite()
        && o.normal_offset.is_finite()
        && o.jitter.is_finite()
        && request.sample_mask.len() == request.meshes.len()
        && (request.face_masks.is_empty() || request.face_masks.len() == request.meshes.len())
        && request
            .face_masks
            .iter()
            .zip(request.meshes)
            .all(|(mask, mesh)| mask.is_empty() || mask.len() == face_count(mesh))
}

/// Faces and vertices of a well-formed snapshot; `None` when malformed,
/// cancelled, empty, or past the 32-bit index range.
fn totals(request: &SurfaceSamplingRequest<'_>) -> Option<(usize, usize)> {
    let (mut faces, mut vertices) = (0_usize, 0_usize);
    for mesh in request.meshes {
        if cancelled(request) || mesh.vertices.len() % 3 != 0 || mesh.triangles.len() % 3 != 0 {
            return None;
        }
        let count = mesh.vertices.len() / 3;
        if !mesh.vertices.iter().all(|c| c.is_finite())
            || !mesh
                .triangles
                .iter()
                .all(|v| usize::try_from(*v).is_ok_and(|v| v < count))
        {
            return None;
        }
        faces += face_count(mesh);
        vertices += count;
        if faces >= u32::MAX as usize || vertices > u32::MAX as usize {
            return None;
        }
    }
    (faces != 0 && vertices != 0).then_some((faces, vertices))
}

/// Per-mesh state the loop threads through.
struct Builder<'r, 'a> {
    request: &'r SurfaceSamplingRequest<'a>,
    old: Option<(PreviousSampling<'a>, BTreeMap<String, usize>)>,
    result: SurfaceSamplingResult,
}

impl Builder<'_, '_> {
    /// Copy `mesh` from the previous study when its identity is exact.
    /// `Some(true)` copied, `Some(false)` not reusable, `None` refused.
    fn try_copy(&mut self, mesh: usize, span: &mut SurfaceMeshSampling) -> Option<bool> {
        let item = self.request.meshes.get(mesh)?;
        let Some((previous, index)) = self.old.as_ref() else {
            return Some(false);
        };
        let Some(&found) = index.get(&canonical_guid(item.guid)) else {
            return Some(false);
        };
        let (Some(old_mesh), Some(old_span)) = (
            previous.meshes.get(found),
            previous.layout.meshes.get(found),
        ) else {
            return Some(false);
        };
        let options = &self.request.options;
        let same_seed = options.domain == SamplingDomain::SurfacePatch
            || options.jitter <= 0.0
            || old_span.first_face == span.first_face;
        if !(same_mesh(item, old_mesh)
            && old_span.sampled == span.sampled
            && old_span.sample_faces == span.sample_faces
            && same_seed
            && can_copy(previous, found, old_span))
        {
            return Some(false);
        }
        if old_span.sample_count > options.max_samples.wrapping_sub(span.first_sample) {
            return None;
        }
        if options.domain == SamplingDomain::SurfacePatch {
            append_patches(
                &mut self.result.patches,
                previous.patch_grid,
                old_span,
                item.guid,
            )?;
        } else {
            let group = u32::try_from(mesh).unwrap_or(u32::MAX);
            append_triangles(
                &mut self.result.triangles,
                previous.sample_grid,
                old_span,
                span.first_face,
                group,
            )?;
        }
        span.sample_count = old_span.sample_count;
        span.patch_count = old_span.patch_count;
        span.degenerate = old_span.degenerate;
        span.undersized = old_span.undersized;
        span.excluded = old_span.excluded;
        span.winding = old_span.winding;
        self.result.reused_meshes += 1;
        self.result.reused_samples += span.sample_count;
        Some(true)
    }

    /// Orient and sample `mesh` afresh; `None` refuses the whole study.
    fn rebuild(&mut self, mesh: usize, span: &mut SurfaceMeshSampling) -> Option<()> {
        let item = self.request.meshes.get(mesh)?;
        let (oriented, winding) =
            orient_outward(item.vertices, item.triangles, None, DEFAULT_WELD_TOLERANCE).ok()?;
        span.winding = winding;
        let mut remaining = self.request.options;
        remaining.max_samples = remaining.max_samples.wrapping_sub(span.first_sample);
        let mask = [u8::from(span.sampled)];
        let face_filter = (!span.sample_faces.is_empty()).then_some(span.sample_faces.as_slice());
        let mut local = SurfaceMeshSampling {
            face_count: span.face_count,
            ..SurfaceMeshSampling::default()
        };
        if self.request.options.domain == SamplingDomain::SurfacePatch {
            let settings = PatchSamplerOptions {
                sample_group: Some(&mask),
                sample_face: face_filter,
                ..patch_options(&remaining)
            };
            // A local group 0 is still THIS element's GUID, never its index.
            let mut grid =
                build_patch_sample_grid(item.vertices, &oriented, None, &[item.guid], &settings);
            if grid.sample_limit_exceeded {
                return None;
            }
            if grid.patch_of_triangle.is_empty() {
                grid.patch_of_triangle = vec![NO_PATCH; span.face_count];
            }
            local.sample_count = grid.count();
            local.patch_count = grid.spans.len();
            local.degenerate = grid.degenerate_faces;
            local.undersized = grid.centroid_patches;
            local.excluded = grid.excluded_patches;
            append_patches(&mut self.result.patches, &grid, &local, item.guid)?;
        } else {
            let settings = SamplerOptions {
                sample_group: Some(&mask),
                sample_face: face_filter,
                ..triangle_options(&remaining, span.first_face)
            };
            let groups = vec![0_u32; span.face_count];
            let grid = build_sample_grid(item.vertices, &oriented, Some(&groups), &settings);
            if span.face_count != 0 && !grid.valid {
                return None;
            }
            local.sample_count = grid.count();
            local.degenerate = grid.degenerate_faces;
            local.undersized = grid.undersized_faces;
            local.excluded = grid.excluded_faces;
            let group = u32::try_from(mesh).unwrap_or(u32::MAX);
            append_triangles(
                &mut self.result.triangles,
                &grid,
                &local,
                span.first_face,
                group,
            )?;
        }
        span.sample_count = local.sample_count;
        span.patch_count = local.patch_count;
        span.degenerate = local.degenerate;
        span.undersized = local.undersized;
        span.excluded = local.excluded;
        self.result.rebuilt_meshes += 1;
        self.result.generated_samples += span.sample_count;
        Some(())
    }
}

/// Sample every mesh, reusing an earlier study's grids where identity is
/// exact. An invalid result (refusal, malformed input, cancellation) is the
/// default value: no partial grid.
#[must_use]
pub fn build_surface_sampling(request: &SurfaceSamplingRequest<'_>) -> SurfaceSamplingResult {
    if !inputs_valid(request) {
        return SurfaceSamplingResult::default();
    }
    if totals(request).is_none() {
        return SurfaceSamplingResult::default();
    }
    if index_meshes(request.meshes).is_none() {
        let result = fresh_scene(request);
        return if cancelled(request) {
            SurfaceSamplingResult::default()
        } else {
            result
        };
    }
    let old = request.previous.and_then(|previous| {
        (compatible(&previous, &request.options))
            .then(|| index_meshes(previous.meshes))
            .flatten()
            .map(|index| (previous, index))
    });
    let mut builder = Builder {
        request,
        old,
        result: SurfaceSamplingResult::default(),
    };
    builder.result.layout.options = request.options;
    let mut first_face = 0;
    for (mesh, item) in request.meshes.iter().enumerate() {
        if cancelled(request) {
            return SurfaceSamplingResult::default();
        }
        let mut span = SurfaceMeshSampling {
            first_face,
            face_count: face_count(item),
            first_sample: builder.result.triangles.count() + builder.result.patches.count(),
            first_patch: builder.result.patches.spans.len(),
            sampled: request.sample_mask.get(mesh).is_some_and(|m| *m != 0),
            sample_faces: request.face_masks.get(mesh).cloned().unwrap_or_default(),
            ..SurfaceMeshSampling::default()
        };
        let Some(copied) = builder.try_copy(mesh, &mut span) else {
            return SurfaceSamplingResult::default();
        };
        if !copied && builder.rebuild(mesh, &mut span).is_none() {
            return SurfaceSamplingResult::default();
        }
        let w = &mut builder.result.winding;
        w.groups += span.winding.groups;
        w.closed += span.winding.closed;
        w.flipped += span.winding.flipped;
        w.flipped_triangles += span.winding.flipped_triangles;
        first_face += span.face_count;
        builder.result.layout.meshes.push(span);
    }
    if cancelled(request) {
        return SurfaceSamplingResult::default();
    }
    let mut result = builder.result;
    result.triangles.valid = request.options.domain == SamplingDomain::TriangleLegacy;
    result.patches.valid = !result.patches.spans.is_empty();
    result.valid = result.triangles.valid || result.patches.valid;
    result.layout.valid = result.valid;
    if result.valid {
        result
    } else {
        SurfaceSamplingResult::default()
    }
}

#[cfg(test)]
mod tests;
