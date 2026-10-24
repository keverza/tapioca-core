//! Surface-mode aim points: TO triangle centroids, reduced by deterministic
//! farthest-point selection seeded once per TO element, so a small object
//! cannot vanish because a large neighbour owns more triangles.

use super::{CANCELLED, VisibilityError, distance_squared};
use crate::raster::{Vec3, cpp_min};
use crate::rays::RayQueries;
use crate::surface_sampling::SamplingMesh;
use std::collections::BTreeSet;

/// One aim point and the mesh it belongs to.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(super) struct AimPoint {
    pub(super) p: Vec3,
    pub(super) mesh: usize,
}

/// Every valid TO triangle's centroid, in snapshot order.
pub(super) fn target_candidates(
    meshes: &[SamplingMesh<'_>],
    to_mask: &[u8],
    queries: &dyn RayQueries,
) -> Result<Vec<AimPoint>, VisibilityError> {
    let mut out = Vec::new();
    for (m, mesh) in meshes.iter().enumerate() {
        if to_mask.get(m).is_none_or(|f| *f == 0) {
            continue;
        }
        for (index, t) in mesh.triangles.as_chunks::<3>().0.iter().enumerate() {
            if index % 1024 == 0 && queries.cancelled() {
                return Err(CANCELLED);
            }
            let [a, b, c] = t.map(|i| usize::try_from(i).unwrap_or(usize::MAX));
            let highest = a.max(b).max(c);
            if highest.saturating_mul(3).saturating_add(2) >= mesh.vertices.len() {
                continue;
            }
            let mut point = AimPoint {
                p: [0.0; 3],
                mesh: m,
            };
            for (axis, slot) in point.p.iter_mut().enumerate() {
                let at = |v: usize| mesh.vertices.get(3 * v + axis).copied().unwrap_or(0.0);
                *slot = (at(a) + at(b) + at(c)) / 3.0;
            }
            out.push(point);
        }
    }
    Ok(out)
}

/// Farthest-point reduction to `limit` points; empty when cancelled.
pub(super) fn reduce_aim_points(
    candidates: &[AimPoint],
    limit: usize,
    queries: &dyn RayQueries,
) -> Result<Vec<AimPoint>, VisibilityError> {
    if candidates.len() <= limit {
        return Ok(candidates.to_vec());
    }
    let mut chosen = Vec::new();
    let mut represented = BTreeSet::new();
    for (i, c) in candidates.iter().enumerate() {
        if chosen.len() >= limit {
            break;
        }
        if i % 1024 == 0 && queries.cancelled() {
            return Err(CANCELLED);
        }
        if represented.insert(c.mesh) {
            chosen.push(i);
        }
    }
    let mut nearest = vec![f64::INFINITY; candidates.len()];
    let include = |nearest: &mut [f64], index: usize| -> Result<(), VisibilityError> {
        let Some(from) = candidates.get(index) else {
            return Ok(());
        };
        for (i, (slot, c)) in nearest.iter_mut().zip(candidates).enumerate() {
            if i % 1024 == 0 && queries.cancelled() {
                return Err(CANCELLED);
            }
            *slot = cpp_min(*slot, distance_squared(&c.p, &from.p));
        }
        Ok(())
    };
    for index in chosen.clone() {
        include(&mut nearest, index)?;
        if let Some(slot) = nearest.get_mut(index) {
            *slot = -1.0;
        }
    }
    while chosen.len() < limit {
        let mut best = 0;
        for (i, value) in nearest.iter().enumerate().skip(1) {
            if nearest.get(best).is_some_and(|b| value > b) {
                best = i;
            }
        }
        chosen.push(best);
        if let Some(slot) = nearest.get_mut(best) {
            *slot = -1.0;
        }
        include(&mut nearest, best)?;
    }
    Ok(chosen
        .iter()
        .filter_map(|i| candidates.get(*i).copied())
        .collect())
}
