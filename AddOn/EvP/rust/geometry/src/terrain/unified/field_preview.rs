//! #175: the unified build's plan previewed from its field (see [`field_preview`]).

use std::sync::atomic::{AtomicU32, Ordering};

use super::{
    Baseline, BuildError, Design, Field, Plan, design, grading_allowed, is_protected, outline,
    plan, region_height_field, validate_plan,
};
use crate::terrain::preview::{self, Preview};

/// The design previewed from its field on a plan grid every `step` metres -- its surface, its
/// contours at `interval` (see [`crate::terrain::contour::trace`]), its cut and fill and its
/// outline shortfalls -- in milliseconds where [`super::build_terrain`] takes seconds, and with
/// no mesh.
///
/// Inside the property the field is what [`super::build_terrain`] grades a vertex to, in its
/// order: the region entries composed over the baseline, then the outline's faces graded over
/// that, then each path band over that (a design has an outline or bands, not both), masks
/// included; outside it, and with no property, the baseline. Fixed controls pin mesh vertices,
/// not a field: the build applies them.
///
/// Where the outline's faces run out before they meet the ground, the reach is widened as the
/// editor's job widens the build's -- four times at a time, at least 20 m -- up to `max_reach`
/// (no widening at or below the asked limit). [`Preview::unreached`] counts the grid nodes still
/// short at the reach used, [`Preview::clipped`] the property-ring samples where the faces end
/// in a step, [`Preview::reach`] the reach used.
///
/// # Errors
/// As [`super::build_terrain`] for the baseline, the plan's regions, the outline's inputs and
/// the path bands' parameters.
pub fn field_preview(
    baseline: &Baseline<'_>,
    plan: &Plan<'_>,
    step: f64,
    interval: f64,
    max_reach: f64,
) -> Result<Preview, BuildError> {
    let base = Field::new(baseline.points, baseline.triangles).map_err(BuildError::Baseline)?;
    let prepared = validate_plan(plan, baseline.property.is_some())?;
    let composed = if prepared.is_empty() {
        None
    } else {
        Some(region_height_field(baseline, &prepared, plan.masks)?)
    };
    let ground_field = composed.as_ref().unwrap_or(&base);
    let bands = plan
        .paths
        .iter()
        .enumerate()
        .map(|(index, path)| {
            Design::new(&path.band, ground_field, path.tolerance).map_err(|_| {
                BuildError::PathParameters {
                    path: u32::try_from(index).unwrap_or(u32::MAX),
                }
            })
        })
        .collect::<Result<Vec<_>, _>>()?;
    let property = baseline.property.map(plan::vertices);
    let inside = |x: f64, y: f64| {
        property.as_ref().is_none_or(|ring| {
            let mut inside = false;
            for (index, a) in ring.iter().enumerate() {
                let Some(b) = ring.get((index + 1) % ring.len()) else {
                    break;
                };
                if (a[1] > y) != (b[1] > y) {
                    let along = (y - a[1]) / (b[1] - a[1]);
                    if (b[0] - a[0]).mul_add(along, a[0]) > x {
                        inside = !inside;
                    }
                }
            }
            inside
        })
    };
    let bounds = base.bounds();
    let mut reach = plan
        .outlines
        .first()
        .map_or(0.0, |planned| planned.grading.limit);
    loop {
        let operator = plan
            .outlines
            .first()
            .map(|planned| {
                outline::Operator::new(
                    baseline.points,
                    baseline.triangles,
                    planned.surface_points,
                    planned.surface_triangles,
                    planned.surface_grades,
                    outline::Grading {
                        limit: reach,
                        ..planned.grading
                    },
                )
            })
            .transpose()
            .map_err(BuildError::Outline)?;
        // The outline's collinear runs merged: the same faces, a fraction of the edges (#175).
        let merged = operator
            .as_ref()
            .map(|operator| operator.merged_edges(outline::PREVIEW_MERGE));
        let faces = operator
            .as_ref()
            .zip(merged.as_deref())
            .map(|(operator, edges)| operator.faces_over(edges));
        // Where the faces cannot reach the design is the ground composed there: those samples
        // skip the faces altogether (the bulk of a large site).
        let near = faces.as_ref().map(outline::Faces::near);
        let short = AtomicU32::new(0);
        let height = |x: f64, y: f64| -> Option<[f64; 2]> {
            let ground = base.height_at(x, y)?.height;
            if !inside(x, y) {
                return Some([ground, ground]);
            }
            let composed = composed.as_ref().map_or(Some(ground), |field| {
                field.height_at(x, y).map(|sample| sample.height)
            })?;
            if is_protected(plan.masks, x, y) || !grading_allowed(plan.masks, x, y) {
                return Some([composed, ground]);
            }
            let mut design = composed;
            if let (Some(faces), Some(near)) = (&faces, &near)
                && near.touches([x, y], [x, y])
            {
                let (graded, unreached) = faces.over(composed, x, y);
                design = graded;
                if unreached {
                    short.fetch_add(1, Ordering::Relaxed);
                }
            }
            for band in &bands {
                if let Ok(Some(sample)) = band.height(x, y)
                    && !matches!(sample.region, design::Region::Base)
                {
                    design = sample.height;
                }
            }
            Some([design, ground])
        };
        let mut previewed = preview::preview(
            [bounds.min[0], bounds.min[1]],
            [bounds.max[0], bounds.max[1]],
            step,
            interval,
            &height,
        );
        let unreached = short.load(Ordering::Relaxed);
        let (Some(faces), Some(operator)) = (&faces, &operator) else {
            return Ok(previewed);
        };
        if unreached == 0 || reach >= max_reach {
            previewed.unreached = unreached;
            previewed.reach = reach;
            if let Some(ring) = &property {
                let indices: Vec<u32> = (0..u32::try_from(ring.len()).unwrap_or(0)).collect();
                let ground = |x, y| operator.base.height_at(x, y).map(|sample| sample.height);
                previewed.clipped = outline::clipped(faces, &ground, &|_, _| true, ring, &indices)
                    .map_err(BuildError::Outline)?;
            }
            return Ok(previewed);
        }
        reach = max_reach.min((reach * 4.0).max(20.0));
    }
}
