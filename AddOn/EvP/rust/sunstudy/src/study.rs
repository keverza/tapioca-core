//! The portable half of one live study.
//!
//! Split-record ownership (issue #3, option A): the C++ `StudyRecord` keeps
//! the snapshot, traversal, occluders, display cache, selection binding,
//! timings, the store's id map, revisions and locks, and holds one of these.
//! Everything a study computes or reads back without the SDK lives here: the
//! day, the samples (borrowed from the owning grid, not copied twice), the
//! session, the atlas the result is drawn through and the sample-to-mesh map
//! the next study's reuse reads.
//!
//! Construction and the preparation calls (`set_sample_meshes`, `reuse_from`)
//! take `&mut self` and happen before the study is shared; afterwards every
//! call takes `&self` and the core is safe to share between threads (the
//! session serialises itself). The read paths port `SunStudyStore`'s readers;
//! the store's "computing" refusal while a slice runs stays in C++, which
//! owns the advancing flag.

use crate::atlas::{AtlasOptions, AtlasSamples, AtlasTile, SunStudyAtlas, scatter_to_atlas};
use crate::occlusion::SampleSet;
use crate::patch_atlas::{AtlasMesh, PatchAtlas, PatchShape, build_stable_triangle_atlas};
use crate::patch_sampler::PatchSampleGrid;
use crate::reuse::ReuseMesh;
use crate::sampler::{FaceLayout, SampleGrid};
use crate::series::SunSeries;
use crate::session::{Poisoned, SunStudySession};
use crate::step_atlas::StepMaskAtlas;
use crate::summary::SunStudyResultSummary;
use std::sync::Mutex;

/// Why a study could not be built or read.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StudyError {
    /// Positions are not xyz triples or normals are not parallel to them.
    BadSamples,
    /// The patch atlas could not be packed within the maximum dimension.
    AtlasRefused,
    /// No valid triangle atlas (a ground or patch study, or a refused pack).
    NoTriangleAtlas,
    /// Not a patch-domain study with a packed patch atlas.
    NoPatchAtlas,
    /// A patch the atlas did not place: grid and atlas are not one study.
    UnplacedPatch,
    /// A panic inside the session left it unusable.
    Poisoned,
}

impl From<Poisoned> for StudyError {
    fn from(_: Poisoned) -> Self {
        Self::Poisoned
    }
}

/// A triangle-domain study's sampling and its stable atlas.
#[derive(Clone, Debug, Default)]
pub struct TriangleDomain {
    /// The samples, with layouts.
    pub grid: SampleGrid,
    /// The packed atlas.
    pub atlas: SunStudyAtlas,
    /// The allocations keyed by (element, local triangle) that keep tiles
    /// still across reruns.
    pub allocations: PatchAtlas,
}

/// A patch-domain study's sampling and its atlas.
#[derive(Clone, Debug, Default)]
pub struct PatchDomain {
    /// The samples, in patch order.
    pub grid: PatchSampleGrid,
    /// One rectangle per patch, stable across reruns.
    pub atlas: PatchAtlas,
}

/// Which samples a study measures.
#[derive(Clone, Debug)]
pub enum StudyDomain {
    /// A ground grid: no model-surface atlas.
    Ground {
        /// xyz positions.
        positions: Vec<f64>,
        /// xyz normals (may be empty: always facing the sun).
        normals: Vec<f64>,
    },
    /// Per-triangle lattices.
    Triangle(TriangleDomain),
    /// Per-surface lattices.
    Patch(PatchDomain),
}

/// A triangle-domain study's inputs.
pub struct TriangleStudy<'a> {
    /// The day.
    pub series: SunSeries,
    /// Grid spacing, metres.
    pub spacing: f64,
    /// The samples (built with layouts).
    pub grid: SampleGrid,
    /// The snapshot's meshes, for stable tile identities.
    pub meshes: &'a [AtlasMesh<'a>],
    /// The study being rerun; its allocations are kept only when it is a
    /// triangle study at the same spacing.
    pub previous: Option<&'a StudyCore>,
    /// Packing settings.
    pub options: AtlasOptions,
}

/// A patch-domain study's inputs.
pub struct PatchStudy<'a> {
    /// The day.
    pub series: SunSeries,
    /// Grid spacing, metres.
    pub spacing: f64,
    /// The samples.
    pub grid: PatchSampleGrid,
    /// The study being rerun; its atlas is refitted only when it is a patch
    /// study at the same spacing that fits within `max_dimension`.
    pub previous: Option<&'a StudyCore>,
    /// Hard ceiling per axis, texels.
    pub max_dimension: u32,
}

/// What reuse needs of the C++-owned half, for both studies.
pub struct ReuseContext<'a> {
    /// The source study's snapshot meshes.
    pub source_meshes: &'a [ReuseMesh<'a>],
    /// The source study's role byte per mesh.
    pub source_roles: &'a [u8],
    /// The target study's snapshot meshes.
    pub target_meshes: &'a [ReuseMesh<'a>],
    /// The target study's role byte per mesh.
    pub target_roles: &'a [u8],
    /// Whether the source traced with the default ray bounds (store-owned).
    pub source_default_ray_bounds: bool,
    /// Polled between meshes and samples.
    pub is_cancelled: Option<&'a dyn Fn() -> bool>,
}

/// What a reuse seeded.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ReuseOutcome {
    /// Samples with no dirty step at all.
    pub reused_samples: usize,
    /// Sample x steps kept.
    pub reused_sample_steps: usize,
}

/// An atlas image: `width * height` floats, -1 where no sample landed.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct AtlasImage {
    /// Texels across.
    pub width: u32,
    /// Texels down.
    pub height: u32,
    /// Hours per texel, row-major.
    pub image: Vec<f32>,
}

/// Everything a renderer needs, from one generation.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct DisplayData {
    /// One tile per source face.
    pub tiles: Vec<AtlasTile>,
    /// One layout per source face.
    pub layouts: Vec<FaceLayout>,
    /// The hours image.
    pub image: AtlasImage,
}

/// The shadow views' inputs.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct StepMasks {
    /// Per-step lit bits in the displayed atlas's shape.
    pub atlas: StepMaskAtlas,
    /// Each step's time of day, minutes.
    pub minutes: Vec<u16>,
    /// The step at which the sun stands highest.
    pub noon_step: u32,
}

/// The atlas a display would bind, without the image.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DisplayInfo {
    /// Texels across.
    pub width: u32,
    /// Texels down.
    pub height: u32,
    /// Source faces drawn.
    pub faces: usize,
}

#[derive(Clone, Copy, Debug)]
struct CachedSummary {
    generation: u64,
    resolved_steps: usize,
    summary: SunStudyResultSummary,
}

/// The portable state of one live study.
#[derive(Debug)]
pub struct StudyCore {
    series: SunSeries,
    spacing: f64,
    domain: StudyDomain,
    sample_meshes: Vec<u32>,
    reuse: ReuseOutcome,
    session: SunStudySession,
    summary: Mutex<Option<CachedSummary>>,
}

/// Mirrors the C++ `source.gridSpacing == target.gridSpacing`.
#[expect(
    clippy::float_cmp,
    reason = "exact equality is the C++ reuse contract for the grid spacing"
)]
fn same_spacing(a: f64, b: f64) -> bool {
    a == b
}

impl StudyCore {
    fn with_domain(series: SunSeries, spacing: f64, domain: StudyDomain) -> Self {
        Self {
            series,
            spacing,
            domain,
            sample_meshes: Vec::new(),
            reuse: ReuseOutcome::default(),
            session: SunStudySession::default(),
            summary: Mutex::new(None),
        }
    }

    /// A ground-grid study.
    ///
    /// # Errors
    /// [`StudyError::BadSamples`] when positions are not triples or normals
    /// are neither empty nor parallel.
    pub fn ground(
        series: SunSeries,
        spacing: f64,
        positions: Vec<f64>,
        normals: Vec<f64>,
    ) -> Result<Self, StudyError> {
        if !positions.len().is_multiple_of(3)
            || (!normals.is_empty() && normals.len() != positions.len())
        {
            return Err(StudyError::BadSamples);
        }
        Ok(Self::with_domain(
            series,
            spacing,
            StudyDomain::Ground { positions, normals },
        ))
    }

    /// A triangle-domain study with its stable atlas. A refused pack is kept
    /// (as C++ keeps it); its readers report [`StudyError::NoTriangleAtlas`].
    ///
    /// # Errors
    /// [`StudyError::BadSamples`] for a grid whose arrays disagree.
    pub fn triangle(study: TriangleStudy<'_>) -> Result<Self, StudyError> {
        let grid = study.grid;
        if !grid.positions.len().is_multiple_of(3) || grid.normals.len() != grid.positions.len() {
            return Err(StudyError::BadSamples);
        }
        let previous = study.previous.and_then(|p| match &p.domain {
            StudyDomain::Triangle(t) if same_spacing(p.spacing, study.spacing) => {
                Some(&t.allocations)
            }
            _ => None,
        });
        let mut allocations = PatchAtlas::default();
        let samples = AtlasSamples {
            layouts: &grid.layouts,
            faces: &grid.faces,
            cell_columns: &grid.cell_columns,
            cell_rows: &grid.cell_rows,
        };
        let atlas = build_stable_triangle_atlas(
            &samples,
            study.meshes,
            &mut allocations,
            previous,
            &study.options,
        );
        let domain = StudyDomain::Triangle(TriangleDomain {
            grid,
            atlas,
            allocations,
        });
        Ok(Self::with_domain(study.series, study.spacing, domain))
    }

    /// A patch-domain study with its atlas fitted (gutter 1).
    ///
    /// # Errors
    /// [`StudyError::BadSamples`] for a grid whose arrays disagree;
    /// [`StudyError::AtlasRefused`] when the atlas cannot be packed.
    pub fn patch(study: PatchStudy<'_>) -> Result<Self, StudyError> {
        let grid = study.grid;
        if !grid.positions.len().is_multiple_of(3) || grid.normals.len() != grid.positions.len() {
            return Err(StudyError::BadSamples);
        }
        let mut atlas = match study.previous.map(|p| (p, &p.domain)) {
            Some((p, StudyDomain::Patch(previous)))
                if same_spacing(p.spacing, study.spacing)
                    && previous.atlas.width() <= study.max_dimension =>
            {
                previous.atlas.clone()
            }
            _ => PatchAtlas::default(),
        };
        let shapes: Vec<PatchShape> = grid
            .spans
            .iter()
            .map(|span| PatchShape {
                key: span.key.clone(),
                columns: span.columns,
                rows: span.rows,
            })
            .collect();
        let _ = atlas.fit(&shapes, 1, study.max_dimension);
        if atlas.width() == 0 {
            return Err(StudyError::AtlasRefused);
        }
        let domain = StudyDomain::Patch(PatchDomain { grid, atlas });
        Ok(Self::with_domain(study.series, study.spacing, domain))
    }

    /// The study's samples, borrowed from whichever grid owns them.
    #[must_use]
    pub fn samples(&self) -> SampleSet<'_> {
        let (positions, normals) = match &self.domain {
            StudyDomain::Ground { positions, normals } => (positions, normals),
            StudyDomain::Triangle(t) => (&t.grid.positions, &t.grid.normals),
            StudyDomain::Patch(p) => (&p.grid.positions, &p.grid.normals),
        };
        SampleSet {
            positions,
            normals: (!normals.is_empty()).then_some(normals.as_slice()),
            count: positions.len() / 3,
        }
    }

    /// The study's domain.
    #[must_use]
    pub fn domain(&self) -> &StudyDomain {
        &self.domain
    }

    /// The day.
    #[must_use]
    pub fn series(&self) -> &SunSeries {
        &self.series
    }

    /// Grid spacing, metres.
    #[must_use]
    pub fn spacing(&self) -> f64 {
        self.spacing
    }

    /// Mesh index per sample (`u32::MAX` unclaimed); empty when unknown.
    #[must_use]
    pub fn sample_meshes(&self) -> &[u32] {
        &self.sample_meshes
    }

    /// What the last reuse seeded.
    #[must_use]
    pub fn reuse(&self) -> ReuseOutcome {
        self.reuse
    }

    /// The session (advance, progress and the accumulator).
    #[must_use]
    pub fn session(&self) -> &SunStudySession {
        &self.session
    }
}

mod display;
mod prepare;

#[cfg(test)]
mod tests;

pub use display::patch_face_arrays;

/// The patch atlas image as the hours stand: patch by patch through the
/// atlas's own scatter, so a one-surface update cannot disagree with it.
fn scatter_patch_image(domain: &PatchDomain, hours: &[f64]) -> Vec<f32> {
    let mut image = vec![-1.0_f32; domain.atlas.texel_count()];
    for (index, span) in domain.grid.spans.iter().enumerate() {
        let Some(values) = hours.get(span.first..span.first + span.count) else {
            continue; // the study has not produced these yet
        };
        let _ = domain
            .atlas
            .scatter_patch(&domain.grid, index, values, &mut image);
    }
    image
}

fn triangle_image(domain: &TriangleDomain, hours: &[f64]) -> AtlasImage {
    AtlasImage {
        width: domain.atlas.width,
        height: domain.atlas.height,
        image: scatter_to_atlas(&domain.atlas, hours, -1.0),
    }
}
