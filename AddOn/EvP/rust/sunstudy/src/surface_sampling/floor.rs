//! A strict lower bound on a patch-domain study's sample count, from triangle
//! areas alone, so a grid far over the ceiling is refused before a single
//! lattice is built (#117). Live, a 0.25 m grid on a model of 584k samples at
//! 1 m took 21-28 s of lattice building to find out it was over 4 M.
//!
//! ⚠️ WHY IT IS A BOUND, NOT A GUESS. A lattice cell is emitted for ANY covered
//! area, and the covered pieces of one patch partition its projected area, so a
//! patch has at least `projected area / spacing^2` cells. Every triangle in a
//! patch lies within the coplanar tolerance of the seed plane, so projection
//! keeps at least `cos(tolerance)` of its area. With an analysis cylinder, only
//! triangles wholly inside `radius - spacing * sqrt(2)` count: a cell's centre
//! lies within the cell, so within `spacing * sqrt(2)` of the triangle it
//! touches, and the cylinder keeps it.
//!
//! ⚠️ THE MARGIN. Coincident, same-facing triangles in one patch would count
//! twice; [`EARLY_REFUSAL_MARGIN`] keeps such a model from being refused early
//! when it would fit. Only clear excess refuses here; the exact count in
//! `build_patch_sample_grid` remains the authority for everything else.

use super::{SamplingDomain, SurfaceSamplingRequest};
use crate::sampler::triangle_normal;
use crate::series::count_to_f64;
use crate::surface_patch::{SurfacePatchOptions, vertex_at};

/// The share of the bound that must exceed the ceiling to refuse early.
pub(super) const EARLY_REFUSAL_MARGIN: f64 = 0.9;

/// The fewest samples a patch-domain sampling of `request` can produce.
pub(super) fn patch_sample_floor(request: &SurfaceSamplingRequest<'_>) -> f64 {
    let options = &request.options;
    let [cx, cy, radius] = options.clip;
    let clipped = radius > 0.0;
    let inner = radius - options.spacing * std::f64::consts::SQRT_2;
    if clipped && inner <= 0.0 {
        return 0.0;
    }
    let inside = |v: &[f64; 3]| (v[0] - cx).powi(2) + (v[1] - cy).powi(2) <= inner * inner;
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
            if clipped && !(inside(&a) && inside(&b) && inside(&c)) {
                continue;
            }
            area += triangle_normal(&a, &b, &c).1;
        }
    }
    let projected = SurfacePatchOptions::default()
        .coplanar_degrees
        .to_radians()
        .cos();
    area * projected / (options.spacing * options.spacing)
}

/// Whether `request` is a patch-domain sampling certain to exceed its ceiling.
pub(super) fn clearly_over_ceiling(request: &SurfaceSamplingRequest<'_>) -> bool {
    request.options.domain == SamplingDomain::SurfacePatch
        && patch_sample_floor(request) * EARLY_REFUSAL_MARGIN
            > count_to_f64(request.options.max_samples)
}
