//! The surface-patch domain as the engine's sample set.
//!
//! Port of `SunStudy/SunStudyPatchSampler.cpp` plus the live `PatchKey` /
//! `MakePatchKey` helpers of `SunStudyCache` (the rest of that cache is not
//! wired into production and is not ported). A second front end, not a second
//! engine: only the geometry-to-samples stage differs from the triangle path,
//! which stays as the oracle.

use crate::raster::{Vec3, cpp_max, cpp_min};
use crate::surface_patch::{
    PatchGrid, SurfacePatch, SurfacePatchOptions, build_patch_grid, build_surface_patches, corners,
    vertex_at,
};
use std::collections::BTreeMap;

/// Patch identity that survives an edit: element GUID, quantised plane and
/// position, and the occurrence among identical keys of one element.
/// Ordering is element bytes, then plane, then occurrence (as C++).
#[derive(Clone, Debug, Default, PartialEq, Eq, PartialOrd, Ord)]
pub struct PatchKey {
    /// Owning element's GUID (not its mesh index).
    pub element: String,
    /// FNV-1a of the quantised normal, plane offset and origin.
    pub plane: u64,
    /// Occurrence among patches of one element sharing `plane`.
    pub occurrence: u32,
}

/// Key quantum, metres: far below modelling precision, far above noise.
pub const PATCH_KEY_QUANTUM_M: f64 = 1.0e-3;

fn mix(hash: &mut u64, value: u64) {
    for byte in value.to_le_bytes() {
        *hash ^= u64::from(byte);
        *hash = hash.wrapping_mul(1_099_511_628_211);
    }
}

/// Rounded (half away from zero), not truncated; out-of-range values
/// saturate where `llround` is implementation-defined.
#[expect(
    clippy::cast_possible_truncation,
    reason = "saturating llround equivalent for in-range quantised coordinates"
)]
fn quantise(value: f64) -> i64 {
    (value / PATCH_KEY_QUANTUM_M).round() as i64
}

#[expect(
    clippy::cast_sign_loss,
    reason = "mirrors static_cast<uint64_t>(int64_t): two's-complement reinterpretation"
)]
fn bits(value: i64) -> u64 {
    value as u64
}

/// The key for one patch of one element (occurrence 0).
#[must_use]
pub fn make_patch_key(element: &str, patch: &SurfacePatch) -> PatchKey {
    let mut hash = 1_469_598_103_934_665_603_u64;
    for axis in patch.normal {
        mix(&mut hash, bits(quantise(axis)));
    }
    let offset = patch.origin[0] * patch.normal[0]
        + patch.origin[1] * patch.normal[1]
        + patch.origin[2] * patch.normal[2];
    mix(&mut hash, bits(quantise(offset)));
    for axis in patch.origin {
        mix(&mut hash, bits(quantise(axis)));
    }
    PatchKey {
        element: element.to_owned(),
        plane: hash,
        occurrence: 0,
    }
}

/// One patch's samples, a contiguous range of the flat arrays.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct PatchSampleSpan {
    /// Identity.
    pub key: PatchKey,
    /// The patch's group (element index); not in the C++ span, carried so the
    /// C ABI can name the element without a string per span.
    pub group: u32,
    /// First sample index.
    pub first: usize,
    /// Sample count.
    pub count: usize,
    /// Lattice columns.
    pub columns: u32,
    /// Lattice rows.
    pub rows: u32,
    /// Lattice origin.
    pub origin: Vec3,
    /// Lattice u axis.
    pub u_axis: Vec3,
    /// Lattice v axis.
    pub v_axis: Vec3,
    /// Patch normal.
    pub normal: Vec3,
    /// World AABB minimum over cell centres and the patch's own corners.
    pub bounds_min: Vec3,
    /// World AABB maximum.
    pub bounds_max: Vec3,
    /// Source-triangle area, m^2.
    pub area: f64,
}

/// The patch-domain sample set, in patch order.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct PatchSampleGrid {
    /// xyz, lifted along the patch normal.
    pub positions: Vec<f64>,
    /// xyz unit.
    pub normals: Vec<f64>,
    /// Clipped cell areas, m^2.
    pub areas: Vec<f64>,
    /// Cell column per sample.
    pub cell_columns: Vec<u32>,
    /// Cell row per sample.
    pub cell_rows: Vec<u32>,
    /// Owning span per sample.
    pub span_of: Vec<u32>,
    /// One per sampled patch.
    pub spans: Vec<PatchSampleSpan>,
    /// Per source triangle, its span or [`NO_PATCH`].
    pub patch_of_triangle: Vec<u32>,
    /// Always 0 from this builder, as in C++; the surface-sampling aggregate
    /// adds per-mesh counts into it.
    pub degenerate_faces: usize,
    /// Patches that fell back to a centroid.
    pub centroid_patches: usize,
    /// Context patches: formed, given no samples.
    pub excluded_patches: usize,
    /// Refused because the projected count exceeded `max_samples`.
    pub sample_limit_exceeded: bool,
    /// At least one span was produced.
    pub valid: bool,
}

/// A triangle no patch claimed.
pub const NO_PATCH: u32 = u32::MAX;

impl PatchSampleGrid {
    /// Samples emitted.
    #[must_use]
    pub fn count(&self) -> usize {
        self.areas.len()
    }

    /// Append one patch's cells, lifted along the span normal, and start the
    /// span's bounds at the cell centres.
    fn emit_cells(&mut self, span: &mut PatchSampleSpan, lattice: &PatchGrid, normal_offset: f64) {
        let span_index = u32::try_from(self.spans.len()).unwrap_or(u32::MAX);
        for (index, cell) in lattice.cells.iter().enumerate() {
            for (centre, normal) in cell.centre.iter().zip(&span.normal) {
                self.positions.push(centre + normal * normal_offset);
            }
            self.normals.extend_from_slice(&span.normal);
            self.areas.push(cell.area);
            self.cell_columns.push(cell.column);
            self.cell_rows.push(cell.row);
            self.span_of.push(span_index);
            if index == 0 {
                span.bounds_min = cell.centre;
                span.bounds_max = cell.centre;
            } else {
                grow(span, &cell.centre);
            }
        }
    }

    /// Sum of sample areas in patch order (fixed order keeps the last bits).
    #[must_use]
    pub fn total_area(&self) -> f64 {
        self.areas.iter().fold(0.0, |sum, area| sum + area)
    }
}

/// Patch sampler settings.
#[derive(Clone, Copy, Debug)]
pub struct PatchSamplerOptions<'a> {
    /// Cell pitch, metres.
    pub spacing: f64,
    /// Lift along the patch normal, metres.
    pub normal_offset: f64,
    /// Refusal ceiling, counted before anything is emitted.
    pub max_samples: usize,
    /// Coplanarity tolerances.
    pub patch: SurfacePatchOptions,
    /// Per group, 0 = context.
    pub sample_group: Option<&'a [u8]>,
    /// Per face, 0 = not a receiver; also splits patches.
    pub sample_face: Option<&'a [u8]>,
}

impl Default for PatchSamplerOptions<'_> {
    fn default() -> Self {
        Self {
            spacing: 1.0,
            normal_offset: 0.01,
            max_samples: 4_000_000,
            patch: SurfacePatchOptions::default(),
            sample_group: None,
            sample_face: None,
        }
    }
}

fn analysed(patch: &SurfacePatch, options: &PatchSamplerOptions<'_>) -> bool {
    let by_group = options.sample_group.is_none_or(|mask| {
        usize::try_from(patch.group)
            .ok()
            .and_then(|g| mask.get(g))
            .is_none_or(|flag| *flag != 0)
    });
    let by_face = options.sample_face.is_none_or(|mask| {
        patch
            .triangles
            .first()
            .and_then(|f| usize::try_from(*f).ok())
            .and_then(|f| mask.get(f))
            .is_none_or(|flag| *flag != 0)
    });
    by_group && by_face
}

fn grow(span: &mut PatchSampleSpan, point: &Vec3) {
    for axis in 0..3 {
        if let (Some(lo), Some(hi), Some(v)) = (
            span.bounds_min.get_mut(axis),
            span.bounds_max.get_mut(axis),
            point.get(axis),
        ) {
            *lo = cpp_min(*lo, *v);
            *hi = cpp_max(*hi, *v);
        }
    }
}

/// Build the patch-domain sample set. `element_of` maps a group id to its
/// element GUID (a group past its end keys on the empty string).
#[must_use]
pub fn build_patch_sample_grid(
    vertices: &[f64],
    triangles: &[u32],
    groups: Option<&[u32]>,
    element_of: &[&str],
    options: &PatchSamplerOptions<'_>,
) -> PatchSampleGrid {
    let mut grid = PatchSampleGrid::default();
    let face_count = triangles.len() / 3;
    if vertices.is_empty()
        || face_count == 0
        || options.spacing.partial_cmp(&0.0) != Some(std::cmp::Ordering::Greater)
    {
        return grid;
    }
    let patches = build_surface_patches(
        vertices,
        triangles,
        groups,
        &options.patch,
        options.sample_face,
    );
    if patches.is_empty() {
        return grid;
    }
    // Counted before anything is built: refused, never truncated.
    let mut projected = 0_usize;
    let mut lattices = Vec::with_capacity(patches.len());
    for patch in &patches {
        if !analysed(patch, options) {
            grid.excluded_patches += 1;
            lattices.push(None);
            continue;
        }
        let lattice = build_patch_grid(patch, vertices, triangles, options.spacing);
        projected += lattice.cells.len();
        lattices.push(Some(lattice));
        if projected > options.max_samples {
            grid.sample_limit_exceeded = true;
            return grid;
        }
    }
    grid.patch_of_triangle = vec![NO_PATCH; face_count];
    let mut occurrences: BTreeMap<PatchKey, u32> = BTreeMap::new();
    for (patch, lattice) in patches.iter().zip(&lattices) {
        let element = usize::try_from(patch.group)
            .ok()
            .and_then(|g| element_of.get(g))
            .copied()
            .unwrap_or("");
        let mut key = make_patch_key(element, patch);
        let seen = occurrences.entry(key.clone()).or_insert(0);
        key.occurrence = *seen;
        *seen += 1;
        let Some(lattice) = lattice.as_ref().filter(|l| !l.cells.is_empty()) else {
            continue;
        };
        if lattice.from_centroid {
            grid.centroid_patches += 1;
        }
        let span_index = u32::try_from(grid.spans.len()).unwrap_or(u32::MAX);
        let mut span = PatchSampleSpan {
            key,
            group: patch.group,
            first: grid.areas.len(),
            count: lattice.cells.len(),
            columns: lattice.columns,
            rows: lattice.rows,
            origin: patch.origin,
            u_axis: patch.u_axis,
            v_axis: patch.v_axis,
            normal: patch.normal,
            area: patch.area,
            ..PatchSampleSpan::default()
        };
        grid.emit_cells(&mut span, lattice, options.normal_offset);
        // Grown to the patch's own corners: the cells stop half a cell inside.
        for &face in &patch.triangles {
            let points = corners(triangles, face)
                .into_iter()
                .flatten()
                .filter_map(|index| vertex_at(vertices, index));
            for p in points {
                grow(&mut span, &p);
            }
            if let Some(slot) = usize::try_from(face)
                .ok()
                .and_then(|f| grid.patch_of_triangle.get_mut(f))
            {
                *slot = span_index;
            }
        }
        grid.spans.push(span);
    }
    grid.valid = !grid.spans.is_empty();
    grid
}

#[cfg(test)]
mod tests;
