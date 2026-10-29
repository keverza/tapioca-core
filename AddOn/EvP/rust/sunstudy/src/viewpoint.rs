//! An isovist: how far one sees from a point in every direction.
//!
//! Port of `SunStudy/ViewpointStudy.cpp`. Three ray sets ([`ViewpointMode`]):
//! all round in the XY plane at the point's height, a sector of that plane, or
//! a band of elevations of the sphere. With Context elements rays stop at the
//! first Context hit, otherwise at the radius. A plan perimeter is
//! counter-clockwise without a closing duplicate, and the area is the fan's
//! shoelace sum about the point, so concave isovists keep their shape.
//!
//! The sphere's seen surfaces are not computed here: the host reconstructs
//! them from these ray ends over its own triangles.

use crate::raster::{cpp_max, cpp_min};
use crate::rays::{FirstHitRay, RayError, RayHit, RayQueries};
use crate::roles::canonical_guid;
use crate::series::count_to_f64;
use std::collections::BTreeSet;

/// Which rays are cast (`ViewpointMode`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum ViewpointMode {
    /// All round, in the XY plane.
    #[default]
    Plan = 0,
    /// A horizontal sector about a bearing; its perimeter is open.
    FieldOfView = 1,
    /// Rows of elevations across a band, each all round.
    Sphere = 2,
}

/// Study settings.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ViewpointOptions {
    /// The eye, world metres.
    pub point: [f64; 3],
    /// Reach, metres.
    pub radius: f64,
    /// Rays around the circle (32..=1440); a field of view takes them pro rata.
    pub rays: usize,
    /// The ray set.
    pub mode: ViewpointMode,
    /// Field of view: its centre, degrees counterclockwise from +X.
    pub bearing_degrees: f64,
    /// Field of view: the full horizontal angle, degrees (1..=359).
    pub field_of_view_degrees: f64,
    /// Sphere: the lowest elevation seen, degrees (-90 straight down).
    pub min_elevation_degrees: f64,
    /// Sphere: the highest elevation seen, degrees (90 the zenith).
    pub max_elevation_degrees: f64,
    /// Sphere: azimuth steps all round (8..=720).
    pub sphere_columns: usize,
    /// Sphere: elevation steps across the band, both ends included (2..=361).
    pub sphere_rows: usize,
}

impl Default for ViewpointOptions {
    fn default() -> Self {
        Self {
            point: [0.0, 0.0, 1.6],
            radius: 20.0,
            rays: 720,
            mode: ViewpointMode::Plan,
            bearing_degrees: 0.0,
            field_of_view_degrees: 90.0,
            min_elevation_degrees: -90.0,
            max_elevation_degrees: 90.0,
            sphere_columns: 96,
            sphere_rows: 33,
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
    /// Plan and field of view: enclosed area, m^2.
    pub area: f64,
    /// Sphere: the volume seen, m^3.
    pub volume: f64,
    /// The share of rays (sphere: of solid angle) that reach the radius.
    pub open_fraction: f64,
}

/// Why a viewpoint was refused; the texts are the C++ ones.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ViewpointError {
    /// Non-finite point, non-positive radius or a ray count outside 32..=1440.
    BadOptions,
    /// A field of view without a finite bearing and a 1..=359 degree angle.
    BadFieldOfView,
    /// A sphere band or grid out of range.
    BadSphere,
    /// A mode byte that is none of [`ViewpointMode`] (the FFI's refusal).
    BadMode,
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
            Self::BadFieldOfView => {
                "viewpoint field of view needs a finite bearing and a 1..359 degree angle"
            }
            Self::BadSphere => {
                "viewpoint sphere needs elevations -90 <= min < max <= 90, 8..720 columns and 2..361 rows"
            }
            Self::BadMode => "viewpoint mode must be plan, field of view or sphere",
            Self::MissingContext => "viewpoint Context contains objects absent from the capture",
            Self::NoIndex => "viewpoint could not acquire the shared snapshot query index",
            Self::Cancelled => "viewpoint calculation cancelled",
        }
    }
}

/// C++ `kRadians`: degrees to radians, as one constant factor.
const RADIANS: f64 = std::f64::consts::PI / 180.0;

fn validate(o: &ViewpointOptions) -> Result<(), ViewpointError> {
    if !o.radius.is_finite()
        || o.radius <= 0.0
        || !(32..=1440).contains(&o.rays)
        || !o.point.iter().all(|v| v.is_finite())
    {
        return Err(ViewpointError::BadOptions);
    }
    if o.mode == ViewpointMode::FieldOfView
        && (!o.bearing_degrees.is_finite()
            || !o.field_of_view_degrees.is_finite()
            || o.field_of_view_degrees < 1.0
            || o.field_of_view_degrees > 359.0)
    {
        return Err(ViewpointError::BadFieldOfView);
    }
    if o.mode == ViewpointMode::Sphere
        && (!o.min_elevation_degrees.is_finite()
            || !o.max_elevation_degrees.is_finite()
            || o.min_elevation_degrees < -90.0
            || o.max_elevation_degrees > 90.0
            || o.min_elevation_degrees >= o.max_elevation_degrees
            || !(8..=720).contains(&o.sphere_columns)
            || !(2..=361).contains(&o.sphere_rows))
    {
        return Err(ViewpointError::BadSphere);
    }
    Ok(())
}

#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "mirrors size_t(std::lround(x)) of a validated 0..=1440 count"
)]
fn rounded(value: f64) -> usize {
    value.round() as usize
}

/// One ray: its unit direction and the share of the sphere it stands for
/// (1 off the sphere).
struct Ray {
    dir: [f64; 3],
    weight: f64,
}

/// The rays in C++ order (row-major from the lowest elevation), with the
/// columns per row. A field of view keeps the plan's angular density and both
/// edges of its angle.
fn ray_set(o: &ViewpointOptions) -> (Vec<Ray>, usize) {
    let sphere = o.mode == ViewpointMode::Sphere;
    let sector = o.mode == ViewpointMode::FieldOfView;
    let columns = if sphere {
        o.sphere_columns
    } else if sector {
        (rounded(count_to_f64(o.rays) * o.field_of_view_degrees / 360.0) + 1).max(2)
    } else {
        o.rays
    };
    let rows = if sphere { o.sphere_rows } else { 1 };
    let (low_end, high_end) = (o.min_elevation_degrees, o.max_elevation_degrees);
    let steps = count_to_f64(rows.saturating_sub(1));
    let mut rays = Vec::with_capacity(rows * columns);
    for row in 0..rows {
        // The row's elevation, and the band of the sphere it stands for (its
        // solid angle per unit azimuth is the difference of sines across it).
        let (elevation, band) = if sphere {
            let elevation = low_end + (high_end - low_end) * count_to_f64(row) / steps;
            let half = 0.5 * (high_end - low_end) / steps;
            let low = cpp_max(low_end, elevation - half);
            let high = cpp_min(high_end, elevation + half);
            (elevation, (high * RADIANS).sin() - (low * RADIANS).sin())
        } else {
            (0.0, 1.0)
        };
        let up = elevation * RADIANS;
        for column in 0..columns {
            let angle = if sector {
                (o.bearing_degrees - 0.5 * o.field_of_view_degrees
                    + o.field_of_view_degrees * count_to_f64(column) / count_to_f64(columns - 1))
                    * RADIANS
            } else {
                std::f64::consts::TAU * count_to_f64(column) / count_to_f64(columns)
            };
            rays.push(Ray {
                dir: [up.cos() * angle.cos(), up.cos() * angle.sin(), up.sin()],
                weight: band,
            });
        }
    }
    (rays, columns)
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
    validate(options)?;
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
    let (rays, columns) = ray_set(options);
    let mut hits = vec![RayHit::default(); rays.len()];
    if !context.is_empty() {
        let casts: Vec<FirstHitRay> = rays
            .iter()
            .map(|ray| FirstHitRay {
                origin: options.point,
                dir: ray.dir,
                length: options.radius,
            })
            .collect();
        queries
            .first_hits(&casts, Some(&mask), &mut hits, 1)
            .map_err(|e| match e {
                RayError::Cancelled => ViewpointError::Cancelled,
                RayError::Failed => ViewpointError::NoIndex,
            })?;
    } else if queries.cancelled() {
        return Err(ViewpointError::Cancelled);
    }
    let sphere = options.mode == ViewpointMode::Sphere;
    let mut result = ViewpointResult::default();
    let [px, py, pz] = options.point;
    let (mut open_weight, mut total_weight) = (0.0, 0.0);
    for (ray, hit) in rays.iter().zip(&hits) {
        let mut distance = options.radius;
        let mut blocker = None;
        if hit.hit {
            distance = cpp_max(0.0, cpp_min(distance, hit.t));
            result.collision_count += 1;
            blocker = Some(hit.mesh);
        }
        result.ray_mesh.push(blocker);
        let dir = ray.dir;
        result.perimeter.push([
            px + distance * dir[0],
            py + distance * dir[1],
            pz + distance * dir[2],
        ]);
        // Each ray stands for weight x (2 pi / columns) steradians of the sphere.
        total_weight += ray.weight;
        open_weight += if blocker.is_none() { ray.weight } else { 0.0 };
        if sphere {
            result.volume += ray.weight
                * (std::f64::consts::TAU / count_to_f64(columns))
                * distance
                * distance
                * distance
                / 3.0;
        }
    }
    if !sphere {
        result.area = fan_area(&result.perimeter, options);
    }
    result.open_fraction = if total_weight > 0.0 {
        open_weight / total_weight
    } else {
        0.0
    };
    Ok(result)
}

/// The shoelace sum about the point; a field of view stays open, its two
/// edges meeting at the point and adding no area.
fn fan_area(perimeter: &[[f64; 3]], options: &ViewpointOptions) -> f64 {
    let [px, py, _] = options.point;
    let n = perimeter.len();
    let mut area = 0.0;
    for (i, a) in perimeter.iter().enumerate() {
        if options.mode == ViewpointMode::FieldOfView && i + 1 == n {
            break;
        }
        let Some(b) = perimeter.get((i + 1) % n) else {
            continue;
        };
        area += ((a[0] - px) * (b[1] - py) - (a[1] - py) * (b[0] - px)) * 0.5;
    }
    area
}

#[cfg(test)]
mod tests;
