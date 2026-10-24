//! Reuse of an earlier study's grids: proving its layout partitions them,
//! proving a mesh's range really holds that mesh, and copying the range.

use super::{
    PreviousSampling, SamplingDomain, SamplingMesh, SurfaceMeshSampling, SurfaceSamplingOptions,
};
use crate::patch_sampler::{NO_PATCH, PatchSampleGrid};
use crate::sampler::SampleGrid;

pub(super) fn fits(first: usize, count: usize, size: usize) -> bool {
    first <= size && count <= size - first
}

pub(super) fn face_count(mesh: &SamplingMesh<'_>) -> usize {
    mesh.triangles.len() / 3
}
fn append<T: Copy>(into: &mut Vec<T>, from: &[T], first: usize, count: usize) -> Option<()> {
    into.extend_from_slice(from.get(first..first.checked_add(count)?)?);
    Some(())
}
/// The previous layout partitions its grids in snapshot order (C++
/// `Compatible`).
#[expect(clippy::float_cmp, reason = "settings must be identical, as C++ !=")]
pub(super) fn compatible(
    previous: &PreviousSampling<'_>,
    options: &SurfaceSamplingOptions,
) -> bool {
    let layout = previous.layout;
    let old = &layout.options;
    if !layout.valid
        || previous.domain != options.domain
        || layout.meshes.len() != previous.meshes.len()
        || old.domain != options.domain
        || old.spacing != options.spacing
        || old.normal_offset != options.normal_offset
        || old.jitter != options.jitter
    {
        return false;
    }
    let patch = previous.domain == SamplingDomain::SurfacePatch;
    let count = if patch {
        previous.patch_grid.count()
    } else {
        previous.sample_grid.count()
    };
    let (mut faces, mut samples, mut patches) = (0, 0, 0);
    for (span, mesh) in layout.meshes.iter().zip(previous.meshes) {
        if span.first_face != faces
            || span.first_sample != samples
            || span.first_patch != patches
            || span.face_count != face_count(mesh)
            || (!span.sample_faces.is_empty() && span.sample_faces.len() != span.face_count)
            || !fits(samples, span.sample_count, count)
            || (!span.sampled && span.sample_count != 0)
            || !fits(patches, span.patch_count, previous.patch_grid.spans.len())
        {
            return false;
        }
        faces += span.face_count;
        samples += span.sample_count;
        patches += span.patch_count;
    }
    samples == count
        && patches == previous.patch_grid.spans.len()
        && faces
            == if patch {
                previous.patch_grid.patch_of_triangle.len()
            } else {
                previous.sample_grid.layouts.len()
            }
}

/// The previous grids really hold this mesh's samples at `span` (C++
/// `CanCopy`).
pub(super) fn can_copy(
    previous: &PreviousSampling<'_>,
    mesh: usize,
    span: &SurfaceMeshSampling,
) -> bool {
    let Some(source_mesh) = previous.meshes.get(mesh) else {
        return false;
    };
    if span.face_count != face_count(source_mesh) {
        return false;
    }
    if previous.domain == SamplingDomain::SurfacePatch {
        return can_copy_patches(previous, source_mesh, span);
    }
    let grid = previous.sample_grid;
    let count = grid.count();
    if !grid.valid
        || grid.positions.len() != count * 3
        || grid.normals.len() != count * 3
        || grid.areas.len() != count
        || grid.groups.len() != count
        || grid.cell_columns.len() != count
        || grid.cell_rows.len() != count
        || !fits(span.first_sample, span.sample_count, count)
        || !fits(span.first_face, span.face_count, grid.layouts.len())
    {
        return false;
    }
    let mesh32 = u32::try_from(mesh).unwrap_or(u32::MAX);
    (span.first_sample..span.first_sample + span.sample_count).all(|sample| {
        let (Some(&group), Some(&face)) = (grid.groups.get(sample), grid.faces.get(sample)) else {
            return false;
        };
        let face = usize::try_from(face).unwrap_or(usize::MAX);
        group == mesh32
            && face >= span.first_face
            && face - span.first_face < span.face_count
            && (span.sample_faces.is_empty()
                || span.sample_faces.get(face - span.first_face) != Some(&0))
    })
}

fn can_copy_patches(
    previous: &PreviousSampling<'_>,
    mesh: &SamplingMesh<'_>,
    span: &SurfaceMeshSampling,
) -> bool {
    let grid = previous.patch_grid;
    let count = grid.count();
    if !grid.valid
        || grid.positions.len() != count * 3
        || grid.normals.len() != count * 3
        || grid.span_of.len() != count
        || grid.cell_columns.len() != count
        || grid.cell_rows.len() != count
        || !fits(span.first_sample, span.sample_count, count)
        || !fits(
            span.first_face,
            span.face_count,
            grid.patch_of_triangle.len(),
        )
        || !fits(span.first_patch, span.patch_count, grid.spans.len())
    {
        return false;
    }
    let mut cursor = span.first_sample;
    for patch in span.first_patch..span.first_patch + span.patch_count {
        let Some(item) = grid.spans.get(patch) else {
            return false;
        };
        if item.first != cursor
            || !fits(cursor - span.first_sample, item.count, span.sample_count)
            || item.key.element != mesh.guid
        {
            return false;
        }
        let patch32 = u32::try_from(patch).unwrap_or(u32::MAX);
        if !grid
            .span_of
            .get(cursor..cursor + item.count)
            .is_some_and(|s| s.iter().all(|p| *p == patch32))
        {
            return false;
        }
        cursor += item.count;
    }
    if cursor != span.first_sample + span.sample_count {
        return false;
    }
    let in_span = |p: u32| {
        usize::try_from(p)
            .is_ok_and(|p| p >= span.first_patch && p - span.first_patch < span.patch_count)
    };
    let samples_ok = grid
        .span_of
        .get(span.first_sample..cursor)
        .is_some_and(|s| s.iter().all(|p| in_span(*p)));
    let faces_ok = (span.first_face..span.first_face + span.face_count).all(|face| {
        let patch = grid
            .patch_of_triangle
            .get(face)
            .copied()
            .unwrap_or(NO_PATCH);
        patch == NO_PATCH
            || (in_span(patch)
                && (span.sample_faces.is_empty()
                    || span.sample_faces.get(face - span.first_face) != Some(&0)))
    });
    samples_ok && faces_ok
}

pub(super) fn append_triangles(
    into: &mut SampleGrid,
    from: &SampleGrid,
    span: &SurfaceMeshSampling,
    first_face: usize,
    group: u32,
) -> Option<()> {
    let (s, n) = (span.first_sample, span.sample_count);
    append(&mut into.positions, &from.positions, s * 3, n * 3)?;
    append(&mut into.normals, &from.normals, s * 3, n * 3)?;
    append(&mut into.areas, &from.areas, s, n)?;
    append(&mut into.cell_columns, &from.cell_columns, s, n)?;
    append(&mut into.cell_rows, &from.cell_rows, s, n)?;
    append(
        &mut into.layouts,
        &from.layouts,
        span.first_face,
        span.face_count,
    )?;
    for &face in from.faces.get(s..s + n)? {
        let local = usize::try_from(face).ok()?.checked_sub(span.first_face)?;
        into.faces
            .push(u32::try_from(first_face + local).unwrap_or(u32::MAX));
    }
    into.groups.extend(std::iter::repeat_n(group, n));
    into.degenerate_faces += span.degenerate;
    into.undersized_faces += span.undersized;
    into.excluded_faces += span.excluded;
    Some(())
}

pub(super) fn append_patches(
    into: &mut PatchSampleGrid,
    from: &PatchSampleGrid,
    span: &SurfaceMeshSampling,
    guid: &str,
) -> Option<()> {
    let first_sample = into.count();
    let first_patch = into.spans.len();
    let (s, n) = (span.first_sample, span.sample_count);
    append(&mut into.positions, &from.positions, s * 3, n * 3)?;
    append(&mut into.normals, &from.normals, s * 3, n * 3)?;
    append(&mut into.areas, &from.areas, s, n)?;
    append(&mut into.cell_columns, &from.cell_columns, s, n)?;
    append(&mut into.cell_rows, &from.cell_rows, s, n)?;
    for &owner in from.span_of.get(s..s + n)? {
        let local = usize::try_from(owner).ok()?.checked_sub(span.first_patch)?;
        into.span_of
            .push(u32::try_from(first_patch + local).unwrap_or(u32::MAX));
    }
    for item in from
        .spans
        .get(span.first_patch..span.first_patch + span.patch_count)?
    {
        let mut item = item.clone();
        item.first = first_sample + item.first.checked_sub(s)?;
        guid.clone_into(&mut item.key.element);
        into.spans.push(item);
    }
    for &patch in from
        .patch_of_triangle
        .get(span.first_face..span.first_face + span.face_count)?
    {
        into.patch_of_triangle.push(if patch == NO_PATCH {
            NO_PATCH
        } else {
            let local = usize::try_from(patch).ok()?.checked_sub(span.first_patch)?;
            u32::try_from(first_patch + local).unwrap_or(u32::MAX)
        });
    }
    into.degenerate_faces += span.degenerate;
    into.centroid_patches += span.undersized;
    into.excluded_patches += span.excluded;
    Some(())
}
