//! Geometric visibility over the sun study's square-cell sampling domain.
//!
//! Port of `SunStudy/VisibilityStudy.cpp`: a policy over the host's scene
//! index and the existing sampler and atlases, not another index. Two
//! origins:
//!
//! - **Surfaces:** each cell of the FROM surfaces aims at a deterministic set
//!   of TO aim points (farthest-point reduced, every TO element represented)
//!   and scores the fraction whose first hit is a TO element.
//! - **Point:** each cell of the TO surfaces inside the view (a cone about a
//!   direction, or a band of elevations of the sphere) is 1 when the segment
//!   from the point is unobstructed, else 0.
//!
//! An explicit Context (even empty) restricts every ray to Context + TO;
//! otherwise rays see the whole model. Refusals carry the C++ texts.

use crate::atlas::{AtlasOptions, AtlasSamples, SunStudyAtlas, build_sun_study_atlas};
use crate::patch_atlas::{PatchAtlas, PatchShape};
use crate::patch_sampler::PatchSampleGrid;
use crate::raster::Vec3;
use crate::rays::{RayError, RayQueries};
use crate::roles::canonical_guid;
use crate::sampler::SampleGrid;
use crate::series::count_to_f64;
use crate::surface_sampling::{
    SamplingDomain, SamplingMesh, SurfaceSamplingOptions, SurfaceSamplingRequest,
    build_surface_sampling,
};
use std::collections::BTreeSet;

const RAY_BATCH: usize = 65_536;

/// Where rays start.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum VisibilityOrigin {
    /// From the FROM surfaces toward the TO elements.
    #[default]
    Surfaces = 0,
    /// From one point toward the TO surfaces.
    Point = 1,
}

/// What a point sees (`VisibilityView`, point mode only).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum VisibilityView {
    /// A cone of `cone_degrees` about `direction`.
    #[default]
    Cone = 0,
    /// Every direction whose elevation lies in the band.
    Sphere = 1,
}

/// Study settings (`VisibilityStudyOptions`).
#[derive(Clone, Debug, PartialEq)]
pub struct VisibilityOptions {
    /// Ray origin mode.
    pub origin: VisibilityOrigin,
    /// Sampling domain.
    pub domain: SamplingDomain,
    /// Restrict rays to Context + TO (Context may be empty).
    pub explicit_context: bool,
    /// Context element GUIDs.
    pub context_elements: Vec<String>,
    /// Cell pitch, metres.
    pub spacing: f64,
    /// Lift off the surface, metres.
    pub normal_offset: f64,
    /// Near distance, metres.
    pub tmin: f64,
    /// Sample ceiling.
    pub max_samples: usize,
    /// Aim points requested (surface mode).
    pub max_aim_points: usize,
    /// Ray ceiling.
    pub max_rays: usize,
    /// Passed to the host index.
    pub max_parallel: usize,
    /// The eye (point mode).
    pub point: Vec3,
    /// View direction (point mode).
    pub direction: Vec3,
    /// Full cone angle, degrees (point mode).
    pub cone_degrees: f64,
    /// Cone or sphere (point mode).
    pub view: VisibilityView,
    /// Sphere: the lowest elevation seen, degrees (-90 straight down).
    pub min_elevation_degrees: f64,
    /// Sphere: the highest elevation seen, degrees (90 the zenith).
    pub max_elevation_degrees: f64,
}

impl Default for VisibilityOptions {
    fn default() -> Self {
        Self {
            origin: VisibilityOrigin::Surfaces,
            domain: SamplingDomain::SurfacePatch,
            explicit_context: false,
            context_elements: Vec::new(),
            spacing: 2.0,
            normal_offset: 0.05,
            tmin: 0.001,
            max_samples: 4_000_000,
            max_aim_points: 32,
            max_rays: 16_000_000,
            max_parallel: 0,
            point: [0.0; 3],
            direction: [1.0, 0.0, 0.0],
            cone_degrees: 90.0,
            view: VisibilityView::Cone,
            min_elevation_degrees: -90.0,
            max_elevation_degrees: 90.0,
        }
    }
}

/// The study's result.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct VisibilityResult {
    /// Triangle-domain cells.
    pub grid: SampleGrid,
    /// Triangle-domain atlas.
    pub atlas: SunStudyAtlas,
    /// Patch-domain cells.
    pub patch_grid: PatchSampleGrid,
    /// Patch-domain atlas.
    pub patch_atlas: PatchAtlas,
    /// Visibility per cell, 0..=1.
    pub values: Vec<f64>,
    /// Aim points (surface mode) or cells (point mode).
    pub aim_point_count: usize,
    /// Rays cast.
    pub ray_count: usize,
    /// Cells with any visibility.
    pub visible_samples: usize,
    /// Mean of `values` (sample-order sum).
    pub mean_visibility: f64,
    /// Sampled area, m^2 (sample-order sum).
    pub analysed_area: f64,
}

/// A refusal, carrying the C++ message.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct VisibilityError(pub &'static str);

const CANCELLED: VisibilityError = VisibilityError("visibility study cancelled");

/// The study's inputs.
pub struct VisibilityRequest<'a> {
    /// Snapshot meshes.
    pub meshes: &'a [SamplingMesh<'a>],
    /// FROM element GUIDs (surface mode).
    pub from: &'a [&'a str],
    /// TO element GUIDs.
    pub to: &'a [&'a str],
    /// Settings.
    pub options: &'a VisibilityOptions,
    /// The host's scene index.
    pub queries: &'a dyn RayQueries,
}

fn distance_squared(from: &Vec3, to: &Vec3) -> f64 {
    let (dx, dy, dz) = (from[0] - to[0], from[1] - to[1], from[2] - to[2]);
    dx * dx + dy * dy + dz * dz
}

/// `value > bound`, false for NaN: the C++ `!(value > bound)` refusals.
fn above(value: f64, bound: f64) -> bool {
    value > bound
}

fn normalize(v: &mut Vec3) -> bool {
    let length = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt();
    if !above(length, 1.0e-12) || !length.is_finite() {
        return false;
    }
    for c in v.iter_mut() {
        *c /= length;
    }
    true
}

fn canonical_set(guids: impl IntoIterator<Item = impl AsRef<str>>) -> BTreeSet<String> {
    guids
        .into_iter()
        .filter(|g| !g.as_ref().is_empty())
        .map(|g| canonical_guid(g.as_ref()))
        .collect()
}

fn mask_of(meshes: &[SamplingMesh<'_>], selected: &BTreeSet<String>) -> (Vec<u8>, usize) {
    let mask: Vec<u8> = meshes
        .iter()
        .map(|m| u8::from(selected.contains(&canonical_guid(m.guid))))
        .collect();
    let matches = mask.iter().filter(|m| **m != 0).count();
    (mask, matches)
}

fn ray_error(e: RayError) -> VisibilityError {
    match e {
        RayError::Cancelled => CANCELLED,
        RayError::Failed => {
            VisibilityError("visibility study could not acquire the snapshot query index")
        }
    }
}

fn point3(values: &[f64], index: usize) -> Vec3 {
    let mut out = [0.0; 3];
    if let Some(at) = values.get(index * 3..index * 3 + 3) {
        out.copy_from_slice(at);
    }
    out
}

/// FROM mask, TO mask, query mask (explicit Context) and TO match count.
type RoleMasks = (Vec<u8>, Vec<u8>, Option<Vec<u8>>, usize);

/// The role checks, in C++ order. Returns the FROM, TO and query masks and
/// the TO match count.
fn roles(r: &VisibilityRequest<'_>) -> Result<RoleMasks, VisibilityError> {
    let o = r.options;
    let (from, to) = (canonical_set(r.from), canonical_set(r.to));
    let context = canonical_set(&o.context_elements);
    if to.is_empty() {
        return Err(VisibilityError(
            "visibility study needs at least one TO element",
        ));
    }
    if o.origin == VisibilityOrigin::Surfaces && from.is_empty() {
        return Err(VisibilityError(
            "surface visibility needs at least one FROM element",
        ));
    }
    if from.iter().any(|g| to.contains(g)) {
        return Err(VisibilityError("FROM and TO element sets must not overlap"));
    }
    if o.explicit_context && context.iter().any(|g| from.contains(g) || to.contains(g)) {
        return Err(VisibilityError(
            "Analysis, Context and Focus element sets must not overlap",
        ));
    }
    let (from_mask, from_matches) = mask_of(r.meshes, &from);
    let (to_mask, to_matches) = mask_of(r.meshes, &to);
    let present: BTreeSet<String> = r.meshes.iter().map(|m| canonical_guid(m.guid)).collect();
    let missing = |role: &BTreeSet<String>| role.iter().any(|g| !present.contains(g));
    if to_matches == 0
        || missing(&to)
        || (o.origin == VisibilityOrigin::Surfaces && (from_matches == 0 || missing(&from)))
        || (o.explicit_context && missing(&context))
    {
        return Err(VisibilityError(
            "visibility role elements are absent from the live snapshot",
        ));
    }
    let query = o.explicit_context.then(|| {
        let (context_mask, _) = mask_of(r.meshes, &context);
        context_mask
            .iter()
            .zip(&to_mask)
            .map(|(c, t)| u8::from(*c != 0 || *t != 0))
            .collect()
    });
    Ok((from_mask, to_mask, query, to_matches))
}

fn validate(o: &VisibilityOptions) -> Result<(), VisibilityError> {
    if !above(o.spacing, 0.0) || !o.spacing.is_finite() {
        return Err(VisibilityError("visibility grid spacing must be positive"));
    }
    if !o.normal_offset.is_finite()
        || o.normal_offset < 0.0
        || !o.tmin.is_finite()
        || o.tmin < 0.0
        || o.max_aim_points == 0
        || o.max_samples == 0
        || o.max_rays == 0
    {
        return Err(VisibilityError(
            "visibility options contain invalid offsets, limits or origin mode",
        ));
    }
    if o.origin == VisibilityOrigin::Point {
        const BAD_POINT: VisibilityError = VisibilityError(
            "visibility point needs finite coordinates, a non-zero direction and a 1..179 degree cone",
        );
        if !o.point.iter().all(|v| v.is_finite()) {
            return Err(BAD_POINT);
        }
        let mut view = o.direction;
        match o.view {
            VisibilityView::Sphere => {
                if !o.min_elevation_degrees.is_finite()
                    || !o.max_elevation_degrees.is_finite()
                    || o.min_elevation_degrees < -90.0
                    || o.max_elevation_degrees > 90.0
                    || o.min_elevation_degrees > o.max_elevation_degrees
                {
                    return Err(VisibilityError(
                        "visibility sphere needs elevations -90 <= min <= max <= 90 degrees",
                    ));
                }
            }
            VisibilityView::Cone => {
                if !normalize(&mut view)
                    || !o.cone_degrees.is_finite()
                    || o.cone_degrees < 1.0
                    || o.cone_degrees > 179.0
                {
                    return Err(BAD_POINT);
                }
            }
        }
    }
    Ok(())
}

/// Sample the display surfaces and pack their atlas.
fn sample(
    r: &VisibilityRequest<'_>,
    display_mask: &[u8],
    result: &mut VisibilityResult,
) -> Result<(), VisibilityError> {
    let o = r.options;
    let poll = || r.queries.cancelled();
    let sampling = build_surface_sampling(&SurfaceSamplingRequest {
        meshes: r.meshes,
        sample_mask: display_mask,
        options: SurfaceSamplingOptions {
            domain: o.domain,
            spacing: o.spacing,
            normal_offset: o.normal_offset,
            jitter: 0.0,
            max_samples: o.max_samples,
        },
        previous: None,
        face_masks: &[],
        is_cancelled: Some(&poll),
    });
    if r.queries.cancelled() {
        return Err(CANCELLED);
    }
    let patch = o.domain == SamplingDomain::SurfacePatch;
    let empty = if patch {
        !sampling.patches.valid || sampling.patches.count() == 0
    } else {
        !sampling.triangles.valid || sampling.triangles.count() == 0
    };
    if !sampling.valid || empty {
        return Err(VisibilityError(
            "visibility square-cell sampling was refused or produced no cells",
        ));
    }
    result.grid = sampling.triangles;
    result.patch_grid = sampling.patches;
    let count = if patch {
        result.patch_grid.count()
    } else {
        result.grid.count()
    };
    if count > o.max_rays {
        return Err(VisibilityError(
            "visibility ray budget cannot represent every square cell at least once",
        ));
    }
    let (width, height) = if patch {
        let shapes: Vec<PatchShape> = result
            .patch_grid
            .spans
            .iter()
            .map(|s| PatchShape {
                key: s.key.clone(),
                columns: s.columns,
                rows: s.rows,
            })
            .collect();
        let _ = result.patch_atlas.fit(&shapes, 1, 8192);
        (result.patch_atlas.width(), result.patch_atlas.height())
    } else {
        let g = &result.grid;
        result.atlas = build_sun_study_atlas(
            &AtlasSamples {
                layouts: &g.layouts,
                faces: &g.faces,
                cell_columns: &g.cell_columns,
                cell_rows: &g.cell_rows,
            },
            &AtlasOptions::default(),
        );
        (result.atlas.width, result.atlas.height)
    };
    if width == 0 || height == 0 || (!patch && !result.atlas.valid) {
        return Err(VisibilityError(
            "visibility square-cell atlas could not be packed",
        ));
    }
    Ok(())
}

/// Run the study.
///
/// # Errors
/// [`VisibilityError`] with the C++ refusal text.
pub fn run_visibility(r: &VisibilityRequest<'_>) -> Result<VisibilityResult, VisibilityError> {
    if r.meshes.is_empty() {
        return Err(VisibilityError(
            "visibility study needs a live non-empty snapshot",
        ));
    }
    validate(r.options)?;
    let (from_mask, to_mask, query, to_matches) = roles(r)?;
    let display_mask = if r.options.origin == VisibilityOrigin::Point {
        &to_mask
    } else {
        &from_mask
    };
    let mut result = VisibilityResult::default();
    sample(r, display_mask, &mut result)?;
    let patch = r.options.domain == SamplingDomain::SurfacePatch;
    let (count, areas) = if patch {
        (result.patch_grid.count(), &result.patch_grid.areas)
    } else {
        (result.grid.count(), &result.grid.areas)
    };
    result.analysed_area = areas.iter().fold(0.0, |sum, a| sum + a);
    result.values = vec![0.0; count];
    r.queries.prepare().map_err(ray_error)?;
    let cells = Cells {
        positions: if patch {
            &result.patch_grid.positions
        } else {
            &result.grid.positions
        },
        normals: if patch {
            &result.patch_grid.normals
        } else {
            &result.grid.normals
        },
        count,
    };
    let (values, aims, rays) = if r.options.origin == VisibilityOrigin::Point {
        from_point(r, &cells, query.as_deref())?
    } else {
        from_surfaces(r, &cells, &to_mask, query.as_deref(), to_matches)?
    };
    if r.queries.cancelled() {
        return Err(CANCELLED);
    }
    result.values = values;
    result.aim_point_count = aims;
    result.ray_count = rays;
    for value in &result.values {
        result.mean_visibility += value;
        result.visible_samples += usize::from(*value > 0.0);
    }
    result.mean_visibility /= count_to_f64(result.values.len());
    Ok(result)
}

mod aims;
mod score;
use score::{Cells, from_point, from_surfaces};

#[cfg(test)]
mod tests;
