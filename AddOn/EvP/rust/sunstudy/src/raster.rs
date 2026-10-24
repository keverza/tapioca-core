//! The shadow camera's geometry as pure arithmetic: frustum fit per sun
//! direction, light view-projection, texel size, bias, the RTC origin, the
//! accumulator layout, the ground sample grid and the refinement ladder.
//!
//! Port of `SunStudy/SunStudyRaster.cpp`, itself a transcription of the Python
//! `sunraster.py` oracle. Three things here produce a plausible wrong picture if
//! lost: the negative near plane, the RTC all-or-nothing rule and the
//! conservative bias. Min/max follow C++ `std::min`/`std::max` (first argument
//! wins unless the comparison strictly prefers the second), not `f64::min`, so
//! NaN inputs propagate exactly as in the oracle.

/// World-space vector, metres or unitless direction depending on use.
pub type Vec3 = [f64; 3];
/// Row-major 4x4 matrix.
pub type Mat4 = [f64; 16];

/// Default shadow-map resolution in texels per side.
pub const DEFAULT_SHADOW_MAP_RESOLUTION: u32 = 2048;
/// Largest accumulator texture side.
pub const MAX_TEXTURE_SIZE: u32 = 16384;
/// Floor of the depth bias, metres.
pub const MIN_BIAS_METRES: f64 = 0.005;

/// Orthographic frustum fitted around a box for one sun direction.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct OrthoFrustum {
    /// Left edge along `right_axis`, metres.
    pub left: f64,
    /// Right edge along `right_axis`, metres.
    pub right: f64,
    /// Bottom edge along `up_axis`, metres.
    pub bottom: f64,
    /// Top edge along `up_axis`, metres.
    pub top: f64,
    /// Near plane along `light_axis`, metres. May be negative and must not be
    /// clamped: a directional light has no eye point, so the scene straddles the
    /// origin and clamping would clip every sunward occluder.
    pub near_plane: f64,
    /// Far plane along `light_axis`, metres.
    pub far_plane: f64,
    /// `right - left`, metres.
    pub world_width: f64,
    /// `top - bottom`, metres.
    pub world_height: f64,
    /// `far - near`, metres.
    pub world_depth: f64,
    /// Unit right axis of the light view.
    pub right_axis: Vec3,
    /// Unit up axis of the light view.
    pub up_axis: Vec3,
    /// Unit axis FROM the sun TOWARD the scene: the direction light travels.
    pub light_axis: Vec3,
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

/// Normalise in place; false only for a length `<= 0` (NaN normalises to NaN,
/// as in the oracle).
fn normalise(v: &mut Vec3) -> bool {
    let length = dot(v, v).sqrt();
    if length <= 0.0 {
        return false;
    }
    for c in v.iter_mut() {
        *c /= length;
    }
    true
}

/// C++ `std::min(a, b)`: `b` only when `b < a`.
fn cpp_min(a: f64, b: f64) -> f64 {
    if b < a { b } else { a }
}

/// C++ `std::max(a, b)`: `b` only when `a < b`.
pub(crate) fn cpp_max(a: f64, b: f64) -> f64 {
    if a < b { b } else { a }
}

fn inverted(lo: &Vec3, hi: &Vec3) -> bool {
    hi[0] < lo[0] || hi[1] < lo[1] || hi[2] < lo[2]
}

const IDENTITY: Mat4 = [
    1.0, 0.0, 0.0, 0.0, //
    0.0, 1.0, 0.0, 0.0, //
    0.0, 0.0, 1.0, 0.0, //
    0.0, 0.0, 0.0, 1.0,
];

/// Row-major `a * b`, summing `k = 0..4` from 0.0 in order, as the oracle does.
fn multiply(a: &Mat4, b: &Mat4) -> Mat4 {
    let mut out = [0.0; 16];
    for (index, slot) in out.iter_mut().enumerate() {
        let (row, col) = (index / 4, index % 4);
        let mut sum = 0.0;
        for k in 0..4 {
            let lhs = a.get(row * 4 + k).copied().unwrap_or(0.0);
            let rhs = b.get(k * 4 + col).copied().unwrap_or(0.0);
            sum += lhs * rhs;
        }
        *slot = sum;
    }
    out
}

/// Fit an orthographic frustum around an AABB for a sun direction pointing TO
/// the sun. `margin` pads every edge in metres. `None` for a zero-length sun or
/// an inverted box: a degenerate timestep is skipped, not fatal.
#[must_use]
pub fn fit_ortho_frustum(
    aabb_min: &Vec3,
    aabb_max: &Vec3,
    sun_dir: &Vec3,
    margin: f64,
) -> Option<OrthoFrustum> {
    let mut to_sun = *sun_dir;
    if !normalise(&mut to_sun) || inverted(aabb_min, aabb_max) {
        return None;
    }
    let light = [-to_sun[0], -to_sun[1], -to_sun[2]];
    // The up candidate flips near the poles: with the sun overhead cross(up, L)
    // collapses and would normalise to NaN. Noon at low latitude is ordinary.
    let mut up_candidate = [0.0, 0.0, 1.0];
    if dot(&light, &up_candidate).abs() > 0.999 {
        up_candidate = [0.0, 1.0, 0.0];
    }
    let mut right = cross(&up_candidate, &light);
    if !normalise(&mut right) {
        return None;
    }
    let up = cross(&light, &right);

    let (lo, hi) = (aabb_min, aabb_max);
    let corners: [Vec3; 8] = [
        [lo[0], lo[1], lo[2]],
        [hi[0], lo[1], lo[2]],
        [lo[0], hi[1], lo[2]],
        [lo[0], lo[1], hi[2]],
        [hi[0], hi[1], lo[2]],
        [hi[0], lo[1], hi[2]],
        [lo[0], hi[1], hi[2]],
        [hi[0], hi[1], hi[2]],
    ];
    let project = |c: &Vec3| [dot(c, &right), dot(c, &up), dot(c, &light)];
    let first = project(&corners[0]);
    let (mut min, mut max) = (first, first);
    for corner in corners.iter().skip(1) {
        let p = project(corner);
        for axis in 0..3 {
            if let (Some(lo), Some(hi), Some(v)) =
                (min.get_mut(axis), max.get_mut(axis), p.get(axis))
            {
                *lo = cpp_min(*lo, *v);
                *hi = cpp_max(*hi, *v);
            }
        }
    }
    let near_plane = min[2] - margin;
    let far_plane = max[2] + margin;
    Some(OrthoFrustum {
        left: min[0] - margin,
        right: max[0] + margin,
        bottom: min[1] - margin,
        top: max[1] + margin,
        near_plane,
        far_plane,
        world_width: (max[0] - min[0]) + 2.0 * margin,
        world_height: (max[1] - min[1]) + 2.0 * margin,
        world_depth: far_plane - near_plane,
        right_axis: right,
        up_axis: up,
        light_axis: light,
    })
}

/// Orthographic projection; identity for a zero-span frustum.
#[must_use]
pub fn ortho_projection(frustum: &OrthoFrustum) -> Mat4 {
    let mut proj = IDENTITY;
    let (left, right) = (frustum.left, frustum.right);
    let (bottom, top) = (frustum.bottom, frustum.top);
    let (near, far) = (frustum.near_plane, frustum.far_plane);
    #[expect(
        clippy::float_cmp,
        reason = "exact zero-span guard copied from the C++ oracle (r == l etc.)"
    )]
    let degenerate = right == left || top == bottom || far == near;
    if degenerate {
        return proj;
    }
    proj[0] = 2.0 / (right - left);
    proj[5] = 2.0 / (top - bottom);
    proj[10] = -2.0 / (far - near);
    proj[3] = -(right + left) / (right - left);
    proj[7] = -(top + bottom) / (top - bottom);
    proj[11] = -(far + near) / (far - near);
    proj
}

/// Relative-to-centre origin, rounded to whole metres (half away from zero) so
/// the same project always shifts by the same vector. Apply it to everything or
/// to nothing: geometry, samples, camera and the light view-projection.
#[must_use]
#[expect(
    clippy::manual_midpoint,
    reason = "parity with the C++ (lo + hi) * 0.5, including its overflow to infinity"
)]
pub fn rtc_origin(aabb_min: &Vec3, aabb_max: &Vec3) -> Vec3 {
    [
        ((aabb_min[0] + aabb_max[0]) * 0.5).round(),
        ((aabb_min[1] + aabb_max[1]) * 0.5).round(),
        ((aabb_min[2] + aabb_max[2]) * 0.5).round(),
    ]
}

/// Shadow-camera view-projection. With `origin`, the matrix consumes positions
/// with `origin` already subtracted and equals the unshifted matrix on
/// unshifted points.
#[must_use]
pub fn light_view_projection(frustum: &OrthoFrustum, origin: Option<&Vec3>) -> Mat4 {
    let mut view = IDENTITY;
    let (r, u, l) = (frustum.right_axis, frustum.up_axis, frustum.light_axis);
    view[0] = r[0];
    view[1] = r[1];
    view[2] = r[2];
    view[4] = u[0];
    view[5] = u[1];
    view[6] = u[2];
    view[8] = -l[0];
    view[9] = -l[1];
    view[10] = -l[2];
    if let Some(o) = origin {
        let mut shift = IDENTITY;
        shift[3] = o[0];
        shift[7] = o[1];
        shift[11] = o[2];
        view = multiply(&view, &shift);
    }
    multiply(&ortho_projection(frustum), &view)
}

/// `m * (p, 1)` with the perspective divide when `w` is neither 0 nor 1.
#[must_use]
pub fn transform_point(matrix: &Mat4, point: &Vec3) -> Vec3 {
    let row = |r: usize| {
        let at = |c: usize| matrix.get(r * 4 + c).copied().unwrap_or(0.0);
        at(0) * point[0] + at(1) * point[1] + at(2) * point[2] + at(3)
    };
    let (x, y, z, w) = (row(0), row(1), row(2), row(3));
    #[expect(
        clippy::float_cmp,
        reason = "exact affine-w test copied from the C++ oracle"
    )]
    let affine = w == 0.0 || w == 1.0;
    if affine {
        [x, y, z]
    } else {
        [x / w, y / w, z / w]
    }
}

/// Metres per shadow-map texel, using the larger frustum side; 0 at zero
/// resolution.
#[must_use]
pub fn texel_world_size(frustum: &OrthoFrustum, resolution: u32) -> f64 {
    if resolution == 0 {
        return 0.0;
    }
    cpp_max(frustum.world_width, frustum.world_height) / f64::from(resolution)
}

/// `max(2 x texel, floor)`: acne reads as a real result, so err large.
#[must_use]
pub fn derive_bias(texel_world_size: f64, min_bias: f64) -> f64 {
    cpp_max(2.0 * texel_world_size, min_bias)
}

/// [`derive_bias`] of [`texel_world_size`].
#[must_use]
pub fn derive_bias_from_frustum(frustum: &OrthoFrustum, resolution: u32, min_bias: f64) -> f64 {
    derive_bias(texel_world_size(frustum, resolution), min_bias)
}

/// World-metre bias in depth-buffer units, clamped to 0.5; 0 for a
/// non-positive span.
#[must_use]
pub fn depth_bias_ndc(bias_metres: f64, near_plane: f64, far_plane: f64) -> f64 {
    let span = far_plane - near_plane;
    if span <= 0.0 {
        return 0.0;
    }
    cpp_min(0.5, bias_metres / span)
}

/// The largest bias across a study's frustums, so a sample cannot change
/// classification with the hour for a reason that is not the sun. An empty
/// sequence returns the floor, never zero.
#[must_use]
pub fn pick_bias(frustums: &[OrthoFrustum], resolution: u32, min_bias: f64) -> f64 {
    frustums.iter().fold(min_bias, |bias, frustum| {
        cpp_max(
            bias,
            derive_bias_from_frustum(frustum, resolution, min_bias),
        )
    })
}

/// Accumulator texture size in texels.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AccumLayout {
    /// Texels per row.
    pub width: u32,
    /// Rows.
    pub height: u32,
}

/// Roughly square accumulator, capped at `max_width` per side. `None` when the
/// samples exceed `max_width^2` or `max_width` is 0: refused, never truncated.
/// Zero samples give a 1x1 layout.
#[must_use]
pub fn compute_accum_layout(sample_count: usize, max_width: u32) -> Option<AccumLayout> {
    if max_width == 0 {
        return None;
    }
    if sample_count == 0 {
        return Some(AccumLayout {
            width: 1,
            height: 1,
        });
    }
    let root = crate::series::count_to_f64(sample_count).sqrt().ceil();
    let width = truncate_to_u32(cpp_min(f64::from(max_width), cpp_max(1.0, root)));
    let height = sample_count.div_ceil(usize::try_from(width).ok()?);
    let height = u32::try_from(height).ok().filter(|h| *h <= max_width)?;
    Some(AccumLayout { width, height })
}

#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "callers clamp to 1..=u32 range first; mirrors static_cast<uint32_t>(double)"
)]
fn truncate_to_u32(value: f64) -> u32 {
    value as u32
}

/// Linear sample index to `(column, row)`; `(0, 0)` for a zero width. Values
/// wrap to 32 bits as `static_cast<uint32_t>` does.
#[must_use]
pub fn sample_index_to_texel(sample_index: usize, accum_width: u32) -> (u32, u32) {
    let Ok(width) = usize::try_from(accum_width) else {
        return (0, 0);
    };
    if width == 0 {
        return (0, 0);
    }
    (
        wrap_to_u32(sample_index % width),
        wrap_to_u32(sample_index / width),
    )
}

#[expect(
    clippy::cast_possible_truncation,
    reason = "deliberate 32-bit wrap, matching static_cast<uint32_t>(size_t)"
)]
fn wrap_to_u32(value: usize) -> u32 {
    value as u32
}

mod ground;
pub use ground::{
    GroundGrid, GroundGridRequest, auto_ground_pad, grid_ladder, make_ground_sample_grid,
};

#[cfg(test)]
mod tests;
