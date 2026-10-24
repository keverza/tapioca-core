//! Turning a triangle soup into the points the study measures.
//!
//! Port of `SunStudy/SunStudySampler.cpp`. A grid of cell centres in each
//! face's own plane, not a subdivision: density depends only on the spacing,
//! and every sample sits strictly inside one face. Every non-degenerate face
//! emits at least one sample (its centroid when smaller than a cell), because
//! dropping small faces would silently delete mullions and railings.

use crate::raster::{Vec3, cpp_max, cpp_min};
use crate::series::count_to_f64;

fn sub(a: &Vec3, b: &Vec3) -> Vec3 {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}

fn dot(a: &Vec3, b: &Vec3) -> f64 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}

fn cross(a: &Vec3, b: &Vec3) -> Vec3 {
    [
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    ]
}

/// Unit normal of `abc` (zero when degenerate) and the triangle's area.
#[must_use]
pub fn triangle_normal(a: &Vec3, b: &Vec3, c: &Vec3) -> (Vec3, f64) {
    let n = cross(&sub(b, a), &sub(c, a));
    let length = dot(&n, &n).sqrt();
    let area = 0.5 * length;
    if length.partial_cmp(&0.0) != Some(std::cmp::Ordering::Greater) {
        return ([0.0; 3], area);
    }
    ([n[0] / length, n[1] / length, n[2] / length], area)
}

fn mix(mut x: u32) -> u32 {
    x ^= x >> 16;
    x = x.wrapping_mul(0x7feb_352d);
    x ^= x >> 15;
    x = x.wrapping_mul(0x846c_a68b);
    x ^= x >> 16;
    x
}

fn unit(h: u32) -> f64 {
    f64::from(h) * (1.0 / 4_294_967_296.0)
}

/// The deterministic per-cell jitter in `[-0.5, 0.5]`: a pure function of the
/// indices, so the same cell jitters the same way on any thread.
#[must_use]
pub fn cell_jitter(face: u32, u: u32, v: u32) -> [f64; 2] {
    let seed = mix(face.wrapping_mul(0x9e37_79b9) ^ mix(u.wrapping_mul(0x85eb_ca6b) ^ mix(v)));
    [unit(seed) - 0.5, unit(mix(seed ^ 0xc2b2_ae35)) - 0.5]
}

/// Where one face's grid sits, and in what frame: the affine map a renderer
/// uses to find the cell an arbitrary point falls in.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct FaceLayout {
    /// The face's first corner.
    pub origin: Vec3,
    /// Unit, along the first edge.
    pub u_axis: Vec3,
    /// Unit, in-plane, perpendicular to `u_axis`.
    pub v_axis: Vec3,
    /// First cell along u. 64-bit here; the C++ `long` is 32-bit on MSVC and
    /// the two agree for every in-range face.
    pub u_start: i64,
    /// First cell along v.
    pub v_start: i64,
    /// Cells spanned along u (not the sample count).
    pub columns: u32,
    /// Cells spanned along v.
    pub rows: u32,
    /// False for a skipped face or one that emitted only its centroid.
    pub gridded: bool,
}

/// The measured points, flat and parallel.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SampleGrid {
    /// xyz, already lifted off the surface.
    pub positions: Vec<f64>,
    /// xyz unit face normals.
    pub normals: Vec<f64>,
    /// m^2 carried by each sample; sums to the sampled mesh area.
    pub areas: Vec<f64>,
    /// Source triangle per sample.
    pub faces: Vec<u32>,
    /// The caller's group id per sample, verbatim.
    pub groups: Vec<u32>,
    /// One per source face when layouts were requested.
    pub layouts: Vec<FaceLayout>,
    /// Cell column of each sample within its face tile (with layouts).
    pub cell_columns: Vec<u32>,
    /// Cell row of each sample within its face tile (with layouts).
    pub cell_rows: Vec<u32>,
    /// Faces skipped as degenerate or with an out-of-range index.
    pub degenerate_faces: usize,
    /// Faces skipped by the group or face filters.
    pub excluded_faces: usize,
    /// Faces that emitted a centroid instead of cells.
    pub undersized_faces: usize,
    /// False when refused (no input, bad spacing, or over `max_samples`).
    pub valid: bool,
}

impl SampleGrid {
    /// Samples emitted.
    #[must_use]
    pub fn count(&self) -> usize {
        self.faces.len()
    }
}

/// Sampler settings.
#[derive(Clone, Copy, Debug)]
pub struct SamplerOptions<'a> {
    /// Exact cell pitch, metres.
    pub spacing: f64,
    /// Lift along the normal, metres.
    pub normal_offset: f64,
    /// Jitter as a fraction of a cell, clamped to 0..=1.
    pub jitter: f64,
    /// Added to the face index for the jitter seed.
    pub jitter_face_offset: u32,
    /// Refusal ceiling.
    pub max_samples: usize,
    /// Fill layouts and cell coordinates.
    pub want_layouts: bool,
    /// Per group, 0 = skip; missing groups are sampled. Ignored without
    /// `groups`.
    pub sample_group: Option<&'a [u8]>,
    /// Per face, 0 = skip; faces past its end are sampled.
    pub sample_face: Option<&'a [u8]>,
}

impl Default for SamplerOptions<'_> {
    fn default() -> Self {
        Self {
            spacing: 1.0,
            normal_offset: 0.01,
            jitter: 0.0,
            jitter_face_offset: 0,
            max_samples: 4_000_000,
            want_layouts: false,
            sample_group: None,
            sample_face: None,
        }
    }
}

/// Whether `groups`/`sample_group` select this face (C++ `SamplesFace`).
#[must_use]
pub fn samples_face(sample_group: Option<&[u8]>, groups: Option<&[u32]>, face: usize) -> bool {
    let (Some(mask), Some(groups)) = (sample_group, groups) else {
        return true;
    };
    let Some(group) = groups.get(face).and_then(|g| usize::try_from(*g).ok()) else {
        return true;
    };
    mask.get(group).is_none_or(|flag| *flag != 0)
}

#[expect(
    clippy::cast_possible_truncation,
    reason = "saturating cast of a floored cell coordinate; mirrors static_cast<long> for in-range values"
)]
fn cell(value: f64) -> i64 {
    value.floor() as i64
}

#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "deliberate 32-bit wrap, matching static_cast<uint32_t> of a long"
)]
fn wrap32(value: i64) -> u32 {
    value as u32
}

#[expect(
    clippy::cast_precision_loss,
    reason = "mirrors static_cast<double>(long) for cell indices"
)]
fn cell_f64(value: i64) -> f64 {
    value as f64
}

fn vertex(vertices: &[f64], index: u32) -> Option<Vec3> {
    let start = usize::try_from(index).ok()?.checked_mul(3)?;
    match vertices.get(start..start.checked_add(3)?)? {
        [x, y, z] => Some([*x, *y, *z]),
        _ => None,
    }
}

/// One face's planar frame and its cell bounds.
struct Frame {
    a: Vec3,
    u: Vec3,
    v: Vec3,
    normal: Vec3,
    area: f64,
    corners: [(f64, f64); 3],
    inverse_d: f64,
    u_range: (i64, i64),
    v_range: (i64, i64),
}

fn frame(a: Vec3, b: Vec3, c: Vec3, spacing: f64) -> Option<Frame> {
    let (normal, area) = triangle_normal(&a, &b, &c);
    if area.partial_cmp(&0.0) != Some(std::cmp::Ordering::Greater) {
        return None;
    }
    // A basis in the face's own plane: gridding in world XY would collapse on
    // a vertical wall, and facades are most of what a sun study is for.
    let edge = sub(&b, &a);
    let edge_length = dot(&edge, &edge).sqrt();
    if edge_length.partial_cmp(&0.0) != Some(std::cmp::Ordering::Greater) {
        return None;
    }
    let u_axis = [
        edge[0] / edge_length,
        edge[1] / edge_length,
        edge[2] / edge_length,
    ];
    let v_axis = cross(&normal, &u_axis);
    let (u0, v0) = (0.0, 0.0);
    let (u1, v1) = (dot(&sub(&b, &a), &u_axis), dot(&sub(&b, &a), &v_axis));
    let (u2, v2) = (dot(&sub(&c, &a), &u_axis), dot(&sub(&c, &a), &v_axis));
    let u_lo = cpp_min(u0, cpp_min(u1, u2));
    let u_hi = cpp_max(u0, cpp_max(u1, u2));
    let v_lo = cpp_min(v0, cpp_min(v1, v2));
    let v_hi = cpp_max(v0, cpp_max(v1, v2));
    let denominator = (v1 - v2) * (u0 - u2) + (u2 - u1) * (v0 - v2);
    let inverse_d = if denominator.abs() > 0.0 {
        1.0 / denominator
    } else {
        0.0
    };
    Some(Frame {
        a,
        u: u_axis,
        v: v_axis,
        normal,
        area,
        corners: [(u0, v0), (u1, v1), (u2, v2)],
        inverse_d,
        u_range: (cell(u_lo / spacing), cell(u_hi / spacing)),
        v_range: (cell(v_lo / spacing), cell(v_hi / spacing)),
    })
}

/// Unlifted positions and cell coordinates of one face's interior cells.
struct Emitted {
    positions: Vec<f64>,
    cells: Vec<(u32, u32)>,
}

fn rasterise(f: &Frame, face_seed: u32, options: &SamplerOptions<'_>) -> Emitted {
    let jitter_scale = cpp_max(0.0, cpp_min(1.0, options.jitter));
    let [(u0, v0), (u1, v1), (u2, v2)] = f.corners;
    let mut out = Emitted {
        positions: Vec::new(),
        cells: Vec::new(),
    };
    for vi in f.v_range.0..=f.v_range.1 {
        for ui in f.u_range.0..=f.u_range.1 {
            let offset = if jitter_scale > 0.0 {
                cell_jitter(face_seed, wrap32(ui), wrap32(vi))
            } else {
                [0.0, 0.0]
            };
            let su = (cell_f64(ui) + 0.5 + offset[0] * jitter_scale) * options.spacing;
            let sv = (cell_f64(vi) + 0.5 + offset[1] * jitter_scale) * options.spacing;
            let l0 = ((v1 - v2) * (su - u2) + (u2 - u1) * (sv - v2)) * f.inverse_d;
            let l1 = ((v2 - v0) * (su - u2) + (u0 - u2) * (sv - v2)) * f.inverse_d;
            let l2 = 1.0 - l0 - l1;
            if l0 < 0.0 || l1 < 0.0 || l2 < 0.0 {
                continue;
            }
            out.positions.extend_from_slice(&[
                f.a[0] + su * f.u[0] + sv * f.v[0],
                f.a[1] + su * f.u[1] + sv * f.v[1],
                f.a[2] + su * f.u[2] + sv * f.v[2],
            ]);
            out.cells
                .push((wrap32(ui - f.u_range.0), wrap32(vi - f.v_range.0)));
        }
    }
    out
}

fn layout_of(f: &Frame, gridded: bool) -> FaceLayout {
    FaceLayout {
        origin: f.a,
        u_axis: f.u,
        v_axis: f.v,
        u_start: f.u_range.0,
        v_start: f.v_range.0,
        columns: wrap32(f.u_range.1 - f.u_range.0 + 1),
        rows: wrap32(f.v_range.1 - f.v_range.0 + 1),
        gridded,
    }
}

/// Grid every selected face of a flat xyz/index soup. `groups` holds one id
/// per face. Refused (`valid == false`) for empty input, non-positive spacing,
/// a `groups` list shorter than the faces (undefined behaviour in the C++
/// original), or more than `max_samples` samples.
#[must_use]
pub fn build_sample_grid(
    vertices: &[f64],
    triangles: &[u32],
    groups: Option<&[u32]>,
    options: &SamplerOptions<'_>,
) -> SampleGrid {
    let mut grid = SampleGrid::default();
    let face_count = triangles.len() / 3;
    if vertices.is_empty() || face_count == 0 || groups.is_some_and(|g| g.len() < face_count) {
        return grid;
    }
    if options.spacing.partial_cmp(&0.0) != Some(std::cmp::Ordering::Greater) {
        return grid;
    }
    let mut flat_positions = Vec::new();
    let mut flat_normals = Vec::new();
    if options.want_layouts {
        grid.layouts = vec![FaceLayout::default(); face_count];
    }
    for (face, corners) in triangles.as_chunks::<3>().0.iter().enumerate() {
        let face_filtered = options
            .sample_face
            .and_then(|mask| mask.get(face))
            .is_some_and(|flag| *flag == 0);
        if !samples_face(options.sample_group, groups, face) || face_filtered {
            grid.excluded_faces += 1;
            continue;
        }
        let [i0, i1, i2] = *corners;
        let (Some(a), Some(b), Some(c)) = (
            vertex(vertices, i0),
            vertex(vertices, i1),
            vertex(vertices, i2),
        ) else {
            grid.degenerate_faces += 1;
            continue;
        };
        let Some(f) = frame(a, b, c, options.spacing) else {
            grid.degenerate_faces += 1;
            continue;
        };
        let face32 = wrap32(i64::try_from(face).unwrap_or(i64::MAX));
        let mut emitted = rasterise(&f, face32.wrapping_add(options.jitter_face_offset), options);
        let gridded = !emitted.cells.is_empty();
        if !gridded {
            // A face below the grid is still measured: its centroid, on cell
            // (0, 0) of a layout marked not gridded.
            grid.undersized_faces += 1;
            emitted.positions = vec![
                (a[0] + b[0] + c[0]) / 3.0,
                (a[1] + b[1] + c[1]) / 3.0,
                (a[2] + b[2] + c[2]) / 3.0,
            ];
            emitted.cells.push((0, 0));
        }
        let count = emitted.cells.len();
        for _ in 0..count {
            flat_normals.extend_from_slice(&f.normal);
        }
        flat_positions.extend_from_slice(&emitted.positions);
        if options.want_layouts {
            grid.cell_columns.extend(emitted.cells.iter().map(|c| c.0));
            grid.cell_rows.extend(emitted.cells.iter().map(|c| c.1));
        }
        if grid.faces.len() + count > options.max_samples {
            return grid;
        }
        if let Some(layout) = grid.layouts.get_mut(face) {
            *layout = layout_of(&f, gridded);
        }
        // The exact face area split between its own samples keeps the weights
        // summing to the true mesh area.
        let weight = f.area / count_to_f64(count);
        let group = groups.and_then(|g| g.get(face).copied()).unwrap_or(0);
        grid.areas.extend(std::iter::repeat_n(weight, count));
        grid.faces.extend(std::iter::repeat_n(face32, count));
        grid.groups.extend(std::iter::repeat_n(group, count));
    }
    grid.positions = flat_positions
        .iter()
        .zip(&flat_normals)
        .map(|(p, n)| p + n * options.normal_offset)
        .collect();
    grid.normals = flat_normals;
    grid.valid = true;
    grid
}

#[cfg(test)]
mod tests;
