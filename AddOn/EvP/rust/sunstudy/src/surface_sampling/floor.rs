//! A strict lower bound on a patch-domain study's sample count, from triangle
//! areas alone, so a grid far over the ceiling is refused before a single
//! lattice is built (#117). Live, a 0.25 m grid on a model of 584k samples at
//! 1 m took 21-28 s of lattice building to find out it was over 4 M.
//!
//! ⚠️ WHY IT IS A BOUND, NOT A GUESS. A lattice cell is emitted for ANY covered
//! area, and the covered pieces of one patch partition its projected area, so a
//! patch has at least `projected area / spacing^2` cells. Every triangle in a
//! patch lies within the coplanar tolerance of the seed plane, so projection
//! keeps at least `cos(tolerance)` of its area.
//!
//! ⚠️ WITH AN ANALYSIS CYLINDER, the part of each triangle inside a polygon
//! inscribed in `radius - spacing * sqrt(2)` counts: a cell's centre lies within
//! the cell, so within `spacing * sqrt(2)` of the covered piece it holds, and
//! the cylinder keeps it. Live, counting only triangles WHOLLY inside left the
//! slabs that cross the circle out, and a clipped refusal still took 31 s.
//!
//! ⚠️ THE MARGIN. Coincident, same-facing triangles in one patch would count
//! twice; [`EARLY_REFUSAL_MARGIN`] keeps such a model from being refused early
//! when it would fit. Only clear excess refuses here; the exact count in
//! `build_patch_sample_grid` remains the authority for everything else.

use super::{SamplingDomain, SurfaceSamplingRequest};
use crate::raster::Vec3;
use crate::sampler::triangle_normal;
use crate::series::count_to_f64;
use crate::surface_patch::{SurfacePatchOptions, vertex_at};

/// The share of the bound that must exceed the ceiling to refuse early.
pub(super) const EARLY_REFUSAL_MARGIN: f64 = 0.9;

/// Sides of the polygon inscribed in the clip circle (97 % of its area).
const CLIP_SIDES: u32 = 32;

/// The part of `polygon` where `(p.xy - centre) . normal <= limit`.
fn clip_half_plane(polygon: &[Vec3], centre: [f64; 2], normal: [f64; 2], limit: f64) -> Vec<Vec3> {
    let side = |p: &Vec3| (p[0] - centre[0]) * normal[0] + (p[1] - centre[1]) * normal[1] - limit;
    let mut out = Vec::with_capacity(polygon.len() + 1);
    for (i, a) in polygon.iter().enumerate() {
        let Some(b) = polygon.get((i + 1) % polygon.len()) else {
            continue;
        };
        let (da, db) = (side(a), side(b));
        if da <= 0.0 {
            out.push(*a);
        }
        if (da <= 0.0) != (db <= 0.0) {
            let t = da / (da - db);
            out.push([
                a[0] + (b[0] - a[0]) * t,
                a[1] + (b[1] - a[1]) * t,
                a[2] + (b[2] - a[2]) * t,
            ]);
        }
    }
    out
}

/// Area of a planar polygon in space, by its fan.
fn polygon_area(polygon: &[Vec3]) -> f64 {
    let Some(first) = polygon.first() else {
        return 0.0;
    };
    polygon
        .windows(2)
        .skip(1)
        .map(|pair| match pair {
            [b, c] => triangle_normal(first, b, c).1,
            _ => 0.0,
        })
        .sum()
}

/// The triangle's area inside the polygon of [`CLIP_SIDES`] sides inscribed in
/// the circle of radius `inner` about `centre`.
fn area_inside(triangle: [Vec3; 3], centre: [f64; 2], inner: f64) -> f64 {
    let apothem = inner * (std::f64::consts::PI / f64::from(CLIP_SIDES)).cos();
    let distance = |p: &Vec3| (p[0] - centre[0]).hypot(p[1] - centre[1]);
    // Wholly within the polygon's own inscribed circle: nothing to clip.
    if triangle.iter().all(|p| distance(p) <= apothem) {
        let [a, b, c] = triangle;
        return triangle_normal(&a, &b, &c).1;
    }
    let mut polygon = triangle.to_vec();
    for k in 0..CLIP_SIDES {
        let angle = 2.0 * std::f64::consts::PI * f64::from(k) / f64::from(CLIP_SIDES);
        polygon = clip_half_plane(&polygon, centre, [angle.cos(), angle.sin()], apothem);
        if polygon.len() < 3 {
            return 0.0;
        }
    }
    polygon_area(&polygon)
}

/// The least surface area (m2) a patch-domain sampling of `request` covers
/// with samples, projection loss included.
pub(super) fn analysed_area_floor(request: &SurfaceSamplingRequest<'_>) -> f64 {
    let options = &request.options;
    let [cx, cy, radius] = options.clip;
    let clipped = radius > 0.0;
    let inner = radius - options.spacing * std::f64::consts::SQRT_2;
    if clipped && inner <= 0.0 {
        return 0.0;
    }
    let mut area = 0.0;
    for (index, mesh) in request.meshes.iter().enumerate() {
        if request.sample_mask.get(index).is_none_or(|flag| *flag == 0) {
            continue;
        }
        // An empty entry measures every face, as the sampler reads it.
        let faces = request
            .face_masks
            .get(index)
            .filter(|mask| !mask.is_empty());
        for (face, corners) in mesh.triangles.as_chunks::<3>().0.iter().enumerate() {
            if faces.is_some_and(|mask| mask.get(face).is_none_or(|flag| *flag == 0)) {
                continue;
            }
            let [Some(a), Some(b), Some(c)] = corners.map(|i| vertex_at(mesh.vertices, i)) else {
                continue;
            };
            area += if clipped {
                area_inside([a, b, c], [cx, cy], inner)
            } else {
                triangle_normal(&a, &b, &c).1
            };
        }
    }
    let projected = SurfacePatchOptions::default()
        .coplanar_degrees
        .to_radians()
        .cos();
    area * projected
}

/// The fewest samples a patch-domain sampling of `request` can produce.
pub(super) fn patch_sample_floor(request: &SurfaceSamplingRequest<'_>) -> f64 {
    let spacing = request.options.spacing;
    analysed_area_floor(request) / (spacing * spacing)
}

/// Whether `request` is a patch-domain sampling certain to exceed its ceiling.
pub(super) fn clearly_over_ceiling(request: &SurfaceSamplingRequest<'_>) -> bool {
    request.options.domain == SamplingDomain::SurfacePatch
        && patch_sample_floor(request) * EARLY_REFUSAL_MARGIN
            > count_to_f64(request.options.max_samples)
}
