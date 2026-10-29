//! The two scoring passes: rays from a point toward the TO cells, and rays
//! from the FROM cells toward the TO aim points.

use super::aims::{reduce_aim_points, target_candidates};
use super::{
    CANCELLED, RAY_BATCH, VisibilityError, VisibilityRequest, VisibilityView, above,
    distance_squared, normalize, point3, ray_error,
};
use crate::raster::{cpp_max, cpp_min};
use crate::rays::{FirstHitRay, OcclusionRay, RayHit, RayQueries};
use crate::series::count_to_f64;
use std::collections::BTreeSet;

pub(super) struct Cells<'a> {
    pub(super) positions: &'a [f64],
    pub(super) normals: &'a [f64],
    pub(super) count: usize,
}

type Scored = (Vec<f64>, usize, usize);

pub(super) fn from_point(
    r: &VisibilityRequest<'_>,
    cells: &Cells<'_>,
    query: Option<&[u8]>,
) -> Result<Scored, VisibilityError> {
    let o = r.options;
    let sphere = o.view == VisibilityView::Sphere;
    let mut view = o.direction;
    if !sphere && !normalize(&mut view) {
        return Err(VisibilityError(
            "visibility point needs a non-zero view direction",
        ));
    }
    let cone = cpp_max(1.0, cpp_min(179.0, o.cone_degrees));
    let cos_half = (cone * 0.5 * std::f64::consts::PI / 180.0).cos();
    // The sphere's band as sines of elevation: a unit direction's z IS that sine.
    let sin_low = (o.min_elevation_degrees * std::f64::consts::PI / 180.0).sin();
    let sin_high = (o.max_elevation_degrees * std::f64::consts::PI / 180.0).sin();
    let seen = |dir: &[f64; 3]| {
        if sphere {
            dir[2] >= sin_low - 1.0e-12 && dir[2] <= sin_high + 1.0e-12
        } else {
            dir[0] * view[0] + dir[1] * view[1] + dir[2] * view[2] >= cos_half
        }
    };
    let mut values = vec![0.0; cells.count];
    let mut ray_count = 0;
    for first in (0..cells.count).step_by(RAY_BATCH) {
        if r.queries.cancelled() {
            return Err(CANCELLED);
        }
        let mut rays = Vec::new();
        let mut samples = Vec::new();
        for sample in first..cells.count.min(first + RAY_BATCH) {
            let (p, n) = (
                point3(cells.positions, sample),
                point3(cells.normals, sample),
            );
            let target = [
                p[0] - n[0] * o.normal_offset,
                p[1] - n[1] * o.normal_offset,
                p[2] - n[2] * o.normal_offset,
            ];
            let mut dir = [
                target[0] - o.point[0],
                target[1] - o.point[1],
                target[2] - o.point[2],
            ];
            let distance = distance_squared(&target, &o.point).sqrt();
            if !above(distance, o.tmin) || !normalize(&mut dir) || !seen(&dir) {
                continue;
            }
            rays.push(OcclusionRay {
                origin: o.point,
                dir,
                tmin: o.tmin,
                tmax: cpp_max(o.tmin, distance - cpp_max(o.tmin, 1.0e-6)),
            });
            samples.push(sample);
        }
        let blocked = blocked(r.queries, &rays, query, o.max_parallel)?;
        if r.queries.cancelled() {
            return Err(CANCELLED);
        }
        ray_count += rays.len();
        for (sample, b) in samples.iter().zip(&blocked) {
            if let Some(v) = values.get_mut(*sample) {
                *v = if *b == 0 { 1.0 } else { 0.0 };
            }
        }
    }
    Ok((values, cells.count, ray_count))
}

/// Any-hit over the whole model, or masked first hits for explicit Context.
fn blocked(
    queries: &dyn RayQueries,
    rays: &[OcclusionRay],
    query: Option<&[u8]>,
    max_parallel: usize,
) -> Result<Vec<u8>, VisibilityError> {
    let mut out = vec![1_u8; rays.len()];
    if rays.is_empty() {
        return Ok(out);
    }
    let Some(mask) = query else {
        queries
            .occlude_rays(rays, &mut out, max_parallel)
            .map_err(ray_error)?;
        return Ok(out);
    };
    let first: Vec<FirstHitRay> = rays.iter().map(first_hit).collect();
    let mut hits = vec![RayHit::default(); rays.len()];
    queries
        .first_hits(&first, Some(mask), &mut hits, max_parallel)
        .map_err(ray_error)?;
    for ((slot, ray), hit) in out.iter_mut().zip(rays).zip(&hits) {
        *slot = u8::from(ray.tmax > ray.tmin && hit.hit);
    }
    Ok(out)
}

/// The C++ first-hit form of a bounded ray: the origin moved along the ray by
/// `tmin`, the length `tmax - tmin`.
fn first_hit(ray: &OcclusionRay) -> FirstHitRay {
    FirstHitRay {
        origin: [
            ray.origin[0] + ray.dir[0] * ray.tmin,
            ray.origin[1] + ray.dir[1] * ray.tmin,
            ray.origin[2] + ray.dir[2] * ray.tmin,
        ],
        dir: ray.dir,
        length: ray.tmax - ray.tmin,
    }
}

pub(super) fn from_surfaces(
    r: &VisibilityRequest<'_>,
    cells: &Cells<'_>,
    to_mask: &[u8],
    query: Option<&[u8]>,
    to_matches: usize,
) -> Result<Scored, VisibilityError> {
    let o = r.options;
    let candidates = target_candidates(r.meshes, to_mask, r.queries)?;
    if candidates.is_empty() {
        return Err(VisibilityError(
            "TO elements contain no valid target triangles",
        ));
    }
    let represented: BTreeSet<usize> = candidates.iter().map(|c| c.mesh).collect();
    if represented.len() != to_matches {
        return Err(VisibilityError(
            "a TO element contains no valid target triangles",
        ));
    }
    let budget = o.max_rays / cells.count;
    let limit = o.max_aim_points.max(to_matches).min(budget);
    if limit < to_matches {
        return Err(VisibilityError(
            "visibility ray budget cannot represent every TO element at least once",
        ));
    }
    let aims = reduce_aim_points(&candidates, limit, r.queries)?;
    if aims.is_empty() || r.queries.cancelled() {
        return Err(CANCELLED);
    }
    let mut values = vec![0.0; cells.count];
    let mut ray_count = 0;
    let step = (RAY_BATCH / aims.len()).max(1);
    for first in (0..cells.count).step_by(step) {
        if r.queries.cancelled() {
            return Err(CANCELLED);
        }
        let mut rays = Vec::new();
        let mut samples = Vec::new();
        for sample in first..cells.count.min(first + step) {
            let (origin, normal) = (
                point3(cells.positions, sample),
                point3(cells.normals, sample),
            );
            for aim in &aims {
                let mut dir = [
                    aim.p[0] - origin[0],
                    aim.p[1] - origin[1],
                    aim.p[2] - origin[2],
                ];
                let distance = distance_squared(&aim.p, &origin).sqrt();
                if !above(distance, o.tmin)
                    || !normalize(&mut dir)
                    || dir[0] * normal[0] + dir[1] * normal[1] + dir[2] * normal[2] <= 0.0
                {
                    continue;
                }
                rays.push(OcclusionRay {
                    origin,
                    dir,
                    tmin: o.tmin,
                    tmax: distance + cpp_max(o.tmin, 1.0e-6),
                });
                samples.push(sample);
            }
        }
        if !rays.is_empty() {
            let first_rays: Vec<FirstHitRay> = rays.iter().map(first_hit).collect();
            let mut hits = vec![RayHit::default(); rays.len()];
            r.queries
                .first_hits(&first_rays, query, &mut hits, o.max_parallel)
                .map_err(ray_error)?;
            for (sample, hit) in samples.iter().zip(&hits) {
                let visible = hit.hit && to_mask.get(hit.mesh).is_some_and(|m| *m != 0);
                if let (true, Some(v)) = (visible, values.get_mut(*sample)) {
                    *v += 1.0;
                }
            }
        }
        if r.queries.cancelled() {
            return Err(CANCELLED);
        }
        ray_count += rays.len();
    }
    let n = count_to_f64(aims.len());
    for value in &mut values {
        *value /= n;
    }
    Ok((values, aims.len(), ray_count))
}
