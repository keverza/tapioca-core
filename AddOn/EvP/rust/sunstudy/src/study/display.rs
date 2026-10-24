//! The store's read paths over one core: hours, step bits, the summary, the
//! two atlas images, the renderer's per-face arrays, the step masks, the
//! footprint and the hover reading. Each reads the session once, so the
//! arrays it returns come from one generation.

use super::{
    AtlasImage, DisplayData, DisplayInfo, PatchDomain, StepMasks, StudyCore, StudyDomain,
    StudyError, scatter_patch_image, triangle_image,
};
use crate::atlas::AtlasTile;
use crate::occlusion::OcclusionAccumulator;
use crate::patch_atlas::PatchAtlas;
use crate::patch_sampler::{NO_PATCH, PatchSampleGrid};
use crate::raster::Vec3;
use crate::reading::{SunStudyReading, read_patch_study_at, read_triangle_study_at};
use crate::sampler::FaceLayout;
use crate::step_atlas::{pack_step_masks, solar_noon_step, step_minutes};
use crate::summary::{SunStudyResultSummary, summarize_sun_hours};

/// A patch-domain study as the per-source-face arrays the display path
/// reads: every triangle a patch claimed gets that patch's lattice and
/// rectangle; an unclaimed triangle gets an unplaced tile. A one-sample patch
/// is a 1x1 tile on its sample's texel. `None` when the grid and atlas are
/// not one study (an unplaced span, a mapping of the wrong length).
#[must_use]
pub fn patch_face_arrays(
    grid: &PatchSampleGrid,
    atlas: &PatchAtlas,
) -> Option<(Vec<AtlasTile>, Vec<FaceLayout>)> {
    if !grid.valid || grid.patch_of_triangle.is_empty() {
        return None;
    }
    let mut span_tiles = Vec::with_capacity(grid.spans.len());
    let mut span_layouts = Vec::with_capacity(grid.spans.len());
    for span in &grid.spans {
        let allocation = atlas.find(&span.key).filter(|a| a.placed())?;
        let (Some(column), Some(row)) = (
            grid.cell_columns.get(span.first),
            grid.cell_rows.get(span.first),
        ) else {
            return None;
        };
        if span.count == 0 {
            return None;
        }
        let mut layout = FaceLayout {
            origin: span.origin,
            u_axis: span.u_axis,
            v_axis: span.v_axis,
            gridded: true,
            ..FaceLayout::default()
        };
        let tile = if span.count == 1 {
            // The surface's only measurement, everywhere on it.
            layout.columns = 1;
            layout.rows = 1;
            AtlasTile {
                x: allocation.x + (*column).min(allocation.width - 1),
                y: allocation.y + (*row).min(allocation.height - 1),
                width: 1,
                height: 1,
            }
        } else {
            layout.columns = allocation.width;
            layout.rows = allocation.height;
            AtlasTile {
                x: allocation.x,
                y: allocation.y,
                width: allocation.width,
                height: allocation.height,
            }
        };
        span_tiles.push(tile);
        span_layouts.push(layout);
    }
    let faces = grid.patch_of_triangle.len();
    let mut tiles = vec![AtlasTile::default(); faces];
    let mut layouts = vec![FaceLayout::default(); faces];
    for ((span, tile), layout) in grid
        .patch_of_triangle
        .iter()
        .zip(&mut tiles)
        .zip(&mut layouts)
    {
        if *span == NO_PATCH {
            continue; // an unplaced tile: ordinary shading
        }
        let index = usize::try_from(*span).ok()?;
        *tile = *span_tiles.get(index)?;
        *layout = *span_layouts.get(index)?;
    }
    Some((tiles, layouts))
}

fn packed_patch(domain: &PatchDomain) -> Result<&PatchDomain, StudyError> {
    if domain.grid.valid && domain.atlas.width() != 0 {
        Ok(domain)
    } else {
        Err(StudyError::NoPatchAtlas)
    }
}

/// Sample-major, LSB-first lit bits: `step_count` bits per sample.
fn packed_bits(accumulator: &OcclusionAccumulator) -> Vec<u8> {
    let (samples, steps) = (accumulator.sample_count(), accumulator.step_count());
    let mut bits = vec![0_u8; (samples * steps).div_ceil(8)];
    for sample in 0..samples {
        for step in (0..steps).filter(|s| accumulator.lit(sample, *s)) {
            let bit = sample * steps + step;
            if let Some(byte) = bits.get_mut(bit >> 3) {
                *byte |= 1 << (bit & 7);
            }
        }
    }
    bits
}

impl StudyCore {
    /// Sun hours per sample, as they stand.
    ///
    /// # Errors
    /// [`StudyError::Poisoned`].
    pub fn sun_hours(&self) -> Result<Vec<f64>, StudyError> {
        Ok(self.session.sun_hours()?)
    }

    /// One byte per (sample, step), sample-major: 1 when lit.
    ///
    /// # Errors
    /// [`StudyError::Poisoned`].
    pub fn step_bits(&self) -> Result<Vec<u8>, StudyError> {
        Ok(self.session.with_accumulator(|a| {
            let (samples, steps) = (a.sample_count(), a.step_count());
            let mut out = vec![0_u8; samples * steps];
            for (index, slot) in out.iter_mut().enumerate() {
                *slot = u8::from(a.lit(index / steps, index % steps));
            }
            out
        })?)
    }

    /// Count, min/mean/max hours, fully lit and shaded, recomputed only when
    /// the session's generation or resolved steps moved.
    ///
    /// # Errors
    /// [`StudyError::Poisoned`].
    pub fn summary(&self) -> Result<SunStudyResultSummary, StudyError> {
        let progress = self.session.progress()?;
        let mut cache = self.summary.lock().map_err(|_| StudyError::Poisoned)?;
        if let Some(c) = cache.as_ref().filter(|c| {
            c.generation == progress.generation && c.resolved_steps == progress.resolved_steps
        }) {
            return Ok(c.summary);
        }
        let summary = summarize_sun_hours(&self.sun_hours()?, self.series.daylight_hours());
        *cache = Some(super::CachedSummary {
            generation: progress.generation,
            resolved_steps: progress.resolved_steps,
            summary,
        });
        Ok(summary)
    }

    /// The triangle atlas image (hours so far: too low until converged).
    ///
    /// # Errors
    /// [`StudyError::NoTriangleAtlas`] without a valid triangle atlas.
    pub fn atlas_image(&self) -> Result<AtlasImage, StudyError> {
        match &self.domain {
            StudyDomain::Triangle(t) if t.atlas.valid => Ok(triangle_image(t, &self.sun_hours()?)),
            _ => Err(StudyError::NoTriangleAtlas),
        }
    }

    /// The patch atlas image, one rectangle per surface.
    ///
    /// # Errors
    /// [`StudyError::NoPatchAtlas`] unless a packed patch-domain study.
    pub fn patch_atlas_image(&self) -> Result<AtlasImage, StudyError> {
        let StudyDomain::Patch(p) = &self.domain else {
            return Err(StudyError::NoPatchAtlas);
        };
        let p = packed_patch(p)?;
        Ok(AtlasImage {
            width: p.atlas.width(),
            height: p.atlas.height(),
            image: scatter_patch_image(p, &self.sun_hours()?),
        })
    }

    /// Per-face tiles and layouts with the image they address.
    ///
    /// # Errors
    /// [`StudyError::NoPatchAtlas`], [`StudyError::UnplacedPatch`] or
    /// [`StudyError::NoTriangleAtlas`] as the store refuses.
    pub fn display_data(&self) -> Result<DisplayData, StudyError> {
        match &self.domain {
            StudyDomain::Patch(p) => {
                let p = packed_patch(p)?;
                let (tiles, layouts) =
                    patch_face_arrays(&p.grid, &p.atlas).ok_or(StudyError::UnplacedPatch)?;
                let image = AtlasImage {
                    width: p.atlas.width(),
                    height: p.atlas.height(),
                    image: scatter_patch_image(p, &self.sun_hours()?),
                };
                Ok(DisplayData {
                    tiles,
                    layouts,
                    image,
                })
            }
            StudyDomain::Triangle(t) if t.atlas.valid => Ok(DisplayData {
                tiles: t.atlas.tiles.clone(),
                layouts: t.grid.layouts.clone(),
                image: triangle_image(t, &self.sun_hours()?),
            }),
            _ => Err(StudyError::NoTriangleAtlas),
        }
    }

    /// The atlas size and face count a display binds.
    ///
    /// # Errors
    /// [`StudyError::NoTriangleAtlas`] when there is no model-surface atlas.
    pub fn display_info(&self) -> Result<DisplayInfo, StudyError> {
        let info = match &self.domain {
            StudyDomain::Patch(p) => DisplayInfo {
                width: p.atlas.width(),
                height: p.atlas.height(),
                faces: p.grid.patch_of_triangle.len(),
            },
            StudyDomain::Triangle(t) if t.atlas.valid => DisplayInfo {
                width: t.atlas.width,
                height: t.atlas.height,
                faces: t.atlas.tiles.len(),
            },
            _ => return Err(StudyError::NoTriangleAtlas),
        };
        if info.width == 0 || info.height == 0 {
            return Err(StudyError::NoTriangleAtlas);
        }
        Ok(info)
    }

    /// Per-step lit bits in the displayed atlas's shape, the step times and
    /// the solar-noon step.
    ///
    /// # Errors
    /// [`StudyError::NoPatchAtlas`] or [`StudyError::NoTriangleAtlas`].
    pub fn step_masks(&self) -> Result<StepMasks, StudyError> {
        let (bits, samples, steps) = self
            .session
            .with_accumulator(|a| (packed_bits(a), a.sample_count(), a.step_count()))?;
        let (texel_of, size): (Vec<i64>, (u32, u32)) = match &self.domain {
            StudyDomain::Patch(p) => {
                let p = packed_patch(p)?;
                let texels = (0..samples)
                    .map(|s| {
                        p.atlas
                            .texel_of(&p.grid, s)
                            .and_then(|t| i64::try_from(t).ok())
                            .unwrap_or(-1)
                    })
                    .collect();
                (texels, (p.atlas.width(), p.atlas.height()))
            }
            StudyDomain::Triangle(t) if t.atlas.valid => {
                let texels = (0..samples)
                    .map(|s| t.atlas.texels.get(s).map_or(-1, |t| i64::from(*t)))
                    .collect();
                (texels, (t.atlas.width, t.atlas.height))
            }
            _ => return Err(StudyError::NoTriangleAtlas),
        };
        let steps_of = self.series.steps();
        Ok(StepMasks {
            atlas: pack_step_masks(&bits, samples, steps, &texel_of, size),
            minutes: step_minutes(steps_of),
            noon_step: solar_noon_step(steps_of),
        })
    }

    /// Sample count and the analysed surface area, m^2 (sample order sum).
    #[must_use]
    pub fn footprint(&self) -> (usize, f64) {
        let area = match &self.domain {
            StudyDomain::Ground { .. } => 0.0,
            StudyDomain::Triangle(t) => t.grid.areas.iter().fold(0.0, |sum, a| sum + a),
            StudyDomain::Patch(p) => p.grid.total_area(),
        };
        (self.samples().count, area)
    }

    /// The study's value at `point` on source face `face` of the snapshot it
    /// ran on (the caller checks the snapshot id).
    ///
    /// # Errors
    /// [`StudyError::Poisoned`].
    pub fn read_at(&self, face: usize, point: &Vec3) -> Result<SunStudyReading, StudyError> {
        let hours = self.sun_hours()?;
        Ok(match &self.domain {
            StudyDomain::Patch(p) => {
                read_patch_study_at(&p.grid, self.spacing, &hours, face, point)
            }
            StudyDomain::Triangle(t) => {
                read_triangle_study_at(&t.grid, self.spacing, &hours, face, point)
            }
            // C++ reads the (empty) triangle grid of a ground study.
            StudyDomain::Ground { .. } => SunStudyReading::default(),
        })
    }
}
