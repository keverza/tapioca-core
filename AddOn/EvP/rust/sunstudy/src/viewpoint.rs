//! A horizontal isovist: how far one sees from a point in every direction.
//!
//! Port of `SunStudy/ViewpointStudy.cpp`. Rays fan out in the XY plane at the
//! point's height; with Context elements they stop at the first Context hit,
//! otherwise at the radius. The perimeter is counter-clockwise without a
//! closing duplicate, and the area is the fan's shoelace sum about the point,
//! so concave isovists keep their shape.

use crate::raster::{cpp_max, cpp_min};
use crate::rays::{FirstHitRay, RayError, RayHit, RayQueries};
use crate::roles::canonical_guid;
use crate::series::count_to_f64;
use std::collections::BTreeSet;

/// Study settings.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ViewpointOptions {
    /// The eye, world metres.
    pub point: [f64; 3],
    /// Horizontal reach, metres.
    pub radius: f64,
    /// Rays around the circle (32..=1440).
    pub rays: usize,
}

impl Default for ViewpointOptions {
    fn default() -> Self {
        Self {
            point: [0.0, 0.0, 1.6],
            radius: 20.0,
            rays: 720,
        }
    }
}

/// The isovist.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ViewpointResult {
    /// Counter-clockwise perimeter.
    pub perimeter: Vec<[f64; 3]>,
    /// Per ray, aligned with `perimeter`: the snapshot mesh whose Context
    /// element stopped it, `None` when it reached the radius.
    pub ray_mesh: Vec<Option<usize>>,
    /// Rays that stopped at Context.
    pub collision_count: usize,
    /// Enclosed area, m^2.
    pub area: f64,
}

/// Why a viewpoint was refused; the texts are the C++ ones.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ViewpointError {
    /// Non-finite point, non-positive radius or a ray count outside 32..=1440.
    BadOptions,
    /// A Context GUID is absent from the snapshot.
    MissingContext,
    /// The host index could not be acquired.
    NoIndex,
    /// Cancelled.
    Cancelled,
}

impl ViewpointError {
    /// The C++ error text.
    #[must_use]
    pub fn message(self) -> &'static str {
        match self {
            Self::BadOptions => {
                "viewpoint needs a snapshot, finite point, positive radius and 32..1440 rays"
            }
            Self::MissingContext => "viewpoint Context contains objects absent from the capture",
            Self::NoIndex => "viewpoint could not acquire the shared snapshot query index",
            Self::Cancelled => "viewpoint calculation cancelled",
        }
    }
}

/// Run the study. `meshes` are the snapshot's element GUIDs in mesh order.
///
/// # Errors
/// [`ViewpointError`] as described on its variants.
pub fn run_viewpoint(
    meshes: &[&str],
    context: &[&str],
    options: &ViewpointOptions,
    queries: &dyn RayQueries,
) -> Result<ViewpointResult, ViewpointError> {
    if !options.radius.is_finite()
        || options.radius <= 0.0
        || !(32..=1440).contains(&options.rays)
        || !options.point.iter().all(|v| v.is_finite())
    {
        return Err(ViewpointError::BadOptions);
    }
    let selected: BTreeSet<String> = context.iter().map(|g| canonical_guid(g)).collect();
    let mut missing = selected.clone();
    let mask: Vec<u8> = meshes
        .iter()
        .map(|g| {
            let guid = canonical_guid(g);
            let hit = u8::from(selected.contains(&guid));
            missing.remove(&guid);
            hit
        })
        .collect();
    if !missing.is_empty() {
        return Err(ViewpointError::MissingContext);
    }
    if !context.is_empty() {
        queries.prepare().map_err(|_| ViewpointError::NoIndex)?;
    }
    let count = count_to_f64(options.rays);
    let directions: Vec<[f64; 3]> = (0..options.rays)
        .map(|ray| {
            let angle = std::f64::consts::TAU * count_to_f64(ray) / count;
            [angle.cos(), angle.sin(), 0.0]
        })
        .collect();
    let mut hits = vec![RayHit::default(); options.rays];
    if !context.is_empty() {
        let rays: Vec<FirstHitRay> = directions
            .iter()
            .map(|dir| FirstHitRay {
                origin: options.point,
                dir: *dir,
                length: options.radius,
            })
            .collect();
        queries
            .first_hits(&rays, Some(&mask), &mut hits, 1)
            .map_err(|e| match e {
                RayError::Cancelled => ViewpointError::Cancelled,
                RayError::Failed => ViewpointError::NoIndex,
            })?;
    } else if queries.cancelled() {
        return Err(ViewpointError::Cancelled);
    }
    let mut result = ViewpointResult::default();
    let [px, py, pz] = options.point;
    for (dir, hit) in directions.iter().zip(&hits) {
        let mut distance = options.radius;
        let mut blocker = None;
        if hit.hit {
            distance = cpp_max(0.0, cpp_min(distance, hit.t));
            result.collision_count += 1;
            blocker = Some(hit.mesh);
        }
        result.ray_mesh.push(blocker);
        result
            .perimeter
            .push([px + distance * dir[0], py + distance * dir[1], pz]);
    }
    let n = result.perimeter.len();
    for (i, a) in result.perimeter.iter().enumerate() {
        let Some(b) = result.perimeter.get((i + 1) % n) else {
            continue;
        };
        result.area += ((a[0] - px) * (b[1] - py) - (a[1] - py) * (b[0] - px)) * 0.5;
    }
    Ok(result)
}

#[cfg(test)]
mod tests;
