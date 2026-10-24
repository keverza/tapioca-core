//! The core's contracts; bit parity with `SunStudyStore` is proven in
//! `tests/cpp/test_sunstudy_rust_parity.cpp`.

use super::*;
use crate::occlusion::{DirectionalQuery, StepOptions, Traversal, TraversalError};
use crate::patch_atlas::AtlasMesh;
use crate::patch_sampler::{PatchSamplerOptions, build_patch_sample_grid};
use crate::reading::SunStudyReading;
use crate::reuse::ReuseMesh;
use crate::sampler::{SamplerOptions, build_sample_grid};
use crate::series::SunStep;
use crate::session::{AdvanceOptions, StudyInputs};
use crate::summary::summarize_sun_hours;

/// Occluded where x > 3 (half the wall), clear elsewhere.
struct Half;

impl Traversal for Half {
    fn occlude_directional(
        &self,
        query: &DirectionalQuery<'_>,
        out: &mut [u8],
    ) -> Result<bool, TraversalError> {
        for (slot, origin) in out.iter_mut().zip(query.origins.as_chunks::<3>().0) {
            *slot = u8::from(origin[0] > 3.0);
        }
        Ok(true)
    }

    fn scene_version(&self) -> u64 {
        1
    }
}

fn day(steps: usize) -> SunSeries {
    let raw: Vec<SunStep> = (0..steps)
        .map(|i| SunStep {
            hour: 8 + i32::try_from(i).unwrap_or(0),
            minute: 0,
            direction: [0.0, 0.0, 1.0],
            altitude_degrees: 10.0 + crate::series::count_to_f64(i),
        })
        .collect();
    SunSeries::from_steps(&raw, 60, 0.0)
}

/// A 6 m x 2 m wall facing +z (12 triangles) and a 4 m x 3 m slab.
struct Scene {
    vertices: Vec<f64>,
    triangles: Vec<u32>,
    groups: Vec<u32>,
}

impl Scene {
    fn new() -> Self {
        let mut vertices = Vec::new();
        let mut triangles = Vec::new();
        let mut groups = Vec::new();
        for i in 0..=6 {
            let x = f64::from(i);
            vertices.extend_from_slice(&[x, 0.0, 0.0, x, 2.0, 0.0]);
        }
        for i in 0..6_u32 {
            let (a, b, c, d) = (i * 2, i * 2 + 1, i * 2 + 2, i * 2 + 3);
            triangles.extend_from_slice(&[a, c, d, a, d, b]);
            groups.extend_from_slice(&[0, 0]);
        }
        let base = u32::try_from(vertices.len() / 3).unwrap_or(0);
        vertices.extend_from_slice(&[
            20.0, 0.0, 0.0, 24.0, 0.0, 0.0, 24.0, 3.0, 0.0, 20.0, 3.0, 0.0,
        ]);
        triangles.extend_from_slice(&[base, base + 1, base + 2, base, base + 2, base + 3]);
        groups.extend_from_slice(&[1, 1]);
        Self {
            vertices,
            triangles,
            groups,
        }
    }

    fn triangle_grid(&self, spacing: f64) -> SampleGrid {
        let options = SamplerOptions {
            spacing,
            want_layouts: true,
            ..SamplerOptions::default()
        };
        build_sample_grid(
            &self.vertices,
            &self.triangles,
            Some(&self.groups),
            &options,
        )
    }

    fn patch_grid(&self, spacing: f64) -> PatchSampleGrid {
        let options = PatchSamplerOptions {
            spacing,
            ..PatchSamplerOptions::default()
        };
        build_patch_sample_grid(
            &self.vertices,
            &self.triangles,
            Some(&self.groups),
            &["wall", "slab"],
            &options,
        )
    }
}

const MESHES: [AtlasMesh<'static>; 1] = [AtlasMesh {
    guid: "11111111-2222-3333-4444-555555555555",
    triangle_count: 14,
}];

fn triangle_core(spacing: f64, previous: Option<&StudyCore>) -> StudyCore {
    let scene = Scene::new();
    let core = StudyCore::triangle(TriangleStudy {
        series: day(4),
        spacing,
        grid: scene.triangle_grid(spacing),
        meshes: &MESHES,
        previous,
        options: AtlasOptions::default(),
    });
    assert!(core.is_ok());
    core.unwrap_or_else(|_| StudyCore::with_domain(day(0), 0.0, ground_domain()))
}

fn patch_core(spacing: f64, previous: Option<&StudyCore>) -> StudyCore {
    let scene = Scene::new();
    let core = StudyCore::patch(PatchStudy {
        series: day(4),
        spacing,
        grid: scene.patch_grid(spacing),
        previous,
        max_dimension: 8192,
    });
    assert!(core.is_ok());
    core.unwrap_or_else(|_| StudyCore::with_domain(day(0), 0.0, ground_domain()))
}

fn ground_domain() -> StudyDomain {
    StudyDomain::Ground {
        positions: Vec::new(),
        normals: Vec::new(),
    }
}

fn converge(core: &StudyCore) {
    assert_eq!(core.sync(StudyInputs::default()), Ok(()));
    let options = AdvanceOptions {
        max_steps: usize::MAX,
        step: StepOptions::default(),
    };
    let steps = core.series().step_count();
    assert_eq!(core.advance(&Half, &options, None), Ok(steps));
    assert!(core.session().progress().is_ok_and(|p| p.converged));
}

#[test]
fn a_core_is_shareable_between_threads() {
    fn shareable<T: Send + Sync>() {}
    shareable::<StudyCore>();
}

#[test]
fn ground_studies_measure_but_have_no_atlas() {
    let positions = vec![1.0, 0.0, 0.0, 5.0, 0.0, 0.0];
    assert_eq!(
        StudyCore::ground(day(3), 1.0, positions.clone(), vec![0.0; 3]).err(),
        Some(StudyError::BadSamples)
    );
    let core = StudyCore::ground(day(3), 1.0, positions, Vec::new());
    let Ok(core) = core else {
        unreachable!("a valid ground grid was refused");
    };
    assert!(core.samples().normals.is_none());
    converge(&core);
    assert_eq!(core.sun_hours(), Ok(vec![3.0, 0.0]));
    assert_eq!(core.step_bits(), Ok(vec![1, 1, 1, 0, 0, 0]));
    assert_eq!(core.footprint(), (2, 0.0));
    assert_eq!(core.atlas_image().err(), Some(StudyError::NoTriangleAtlas));
    assert_eq!(
        core.patch_atlas_image().err(),
        Some(StudyError::NoPatchAtlas)
    );
    assert_eq!(core.display_info().err(), Some(StudyError::NoTriangleAtlas));
    assert_eq!(core.step_masks().err(), Some(StudyError::NoTriangleAtlas));
    assert_eq!(
        core.read_at(0, &[1.0, 0.0, 0.0]),
        Ok(SunStudyReading::default())
    );
}

#[test]
fn triangle_studies_draw_through_their_stable_atlas() {
    let core = triangle_core(0.7, None);
    let StudyDomain::Triangle(domain) = core.domain() else {
        unreachable!("not a triangle study");
    };
    assert!(domain.atlas.valid);
    converge(&core);
    let hours = core.sun_hours().unwrap_or_default();
    let display = core.display_data();
    let Ok(display) = display else {
        unreachable!("no display data");
    };
    assert_eq!(display.tiles, domain.atlas.tiles);
    assert_eq!(display.layouts, domain.grid.layouts);
    for (sample, texel) in domain.atlas.texels.iter().enumerate() {
        let shown = display
            .image
            .image
            .get(usize::try_from(*texel).unwrap_or(usize::MAX));
        assert_eq!(
            shown.copied(),
            hours.get(sample).map(|h| crate::atlas::f64_to_f32(*h))
        );
    }
    assert_eq!(core.atlas_image().map(|i| i.image), Ok(display.image.image));
    // Each sample reads itself through its own face.
    for (sample, face) in domain.grid.faces.iter().enumerate() {
        let mut point = [0.0; 3];
        if let Some(at) = domain.grid.positions.get(sample * 3..sample * 3 + 3) {
            point.copy_from_slice(at);
        }
        let reading = core.read_at(usize::try_from(*face).unwrap_or(0), &point);
        assert!(reading.is_ok_and(|r| r.measured && r.sample == sample));
    }
    let area: f64 = domain.grid.areas.iter().sum();
    assert!((core.footprint().1 - area).abs() < 1e-12);
}

#[test]
fn step_masks_agree_with_the_step_bits_texel_for_texel() {
    for core in [triangle_core(0.7, None), patch_core(0.7, None)] {
        converge(&core);
        let bits = core.step_bits().unwrap_or_default();
        let Ok(masks) = core.step_masks() else {
            unreachable!("no step masks");
        };
        let steps = core.series().step_count();
        assert_eq!(masks.minutes, vec![480, 540, 600, 660]);
        assert_eq!(masks.noon_step, 3);
        let plane = usize::try_from(masks.atlas.width * masks.atlas.height).unwrap_or(0);
        let samples = core.samples().count;
        let texel_of = |s: usize| match core.domain() {
            StudyDomain::Triangle(t) => t.atlas.texels.get(s).map(|t| *t as usize),
            StudyDomain::Patch(p) => p
                .atlas
                .texel_of(&p.grid, s)
                .and_then(|t| usize::try_from(t).ok()),
            StudyDomain::Ground { .. } => None,
        };
        for sample in 0..samples {
            let Some(texel) = texel_of(sample) else {
                continue;
            };
            for step in 0..steps {
                let word = masks.atlas.masks.get((step >> 5) * plane + texel);
                let lit = word.map(|w| w & (1 << (step & 31)) != 0);
                assert_eq!(lit, bits.get(sample * steps + step).map(|b| *b != 0));
            }
        }
    }
}

#[test]
fn patch_studies_scatter_per_surface_and_fan_out_per_face() {
    let core = patch_core(0.7, None);
    converge(&core);
    let StudyDomain::Patch(domain) = core.domain() else {
        unreachable!("not a patch study");
    };
    let hours = core.sun_hours().unwrap_or_default();
    let Ok(image) = core.patch_atlas_image() else {
        unreachable!("no patch image");
    };
    assert_eq!(
        image.image.len(),
        usize::try_from(image.width * image.height).unwrap_or(0)
    );
    for (sample, value) in hours.iter().enumerate() {
        let texel = domain.atlas.texel_of(&domain.grid, sample);
        let texel = texel
            .and_then(|t| usize::try_from(t).ok())
            .unwrap_or(usize::MAX);
        assert_eq!(
            image.image.get(texel).copied(),
            Some(crate::atlas::f64_to_f32(*value))
        );
    }
    let Ok(display) = core.display_data() else {
        unreachable!("no display data");
    };
    assert_eq!(display.tiles.len(), domain.grid.patch_of_triangle.len());
    // Every triangle of the wall carries the wall's one record.
    let wall = domain.grid.patch_of_triangle.first().copied();
    for (face, span) in domain.grid.patch_of_triangle.iter().enumerate() {
        if Some(*span) == wall {
            assert_eq!(display.tiles.get(face), display.tiles.first());
            assert_eq!(display.layouts.get(face), display.layouts.first());
        }
    }
    assert_eq!(core.atlas_image().err(), Some(StudyError::NoTriangleAtlas));
    let info = core.display_info();
    assert_eq!(
        info.map(|i| (i.width, i.height, i.faces)),
        Ok((image.width, image.height, 14))
    );
}

#[test]
fn a_one_sample_patch_is_a_one_texel_tile() {
    let mut grid = Scene::new().patch_grid(0.7);
    let mut atlas = PatchAtlas::default();
    let shapes: Vec<PatchShape> = grid
        .spans
        .iter()
        .map(|s| PatchShape {
            key: s.key.clone(),
            columns: s.columns,
            rows: s.rows,
        })
        .collect();
    let _ = atlas.fit(&shapes, 1, 8192);
    if let Some(span) = grid.spans.get_mut(1) {
        span.count = 1;
    }
    let arrays = patch_face_arrays(&grid, &atlas);
    let Some((tiles, layouts)) = arrays else {
        unreachable!("refused");
    };
    let slab = tiles.last().copied().unwrap_or_default();
    assert_eq!((slab.width, slab.height), (1, 1));
    assert_eq!(layouts.last().map(|l| (l.columns, l.rows)), Some((1, 1)));
    // An atlas that does not hold the grid's patches is refused outright.
    assert!(patch_face_arrays(&grid, &PatchAtlas::default()).is_none());
    grid.valid = false;
    assert!(patch_face_arrays(&grid, &atlas).is_none());
}

#[test]
fn a_rerun_at_the_same_spacing_keeps_every_rectangle() {
    let first = patch_core(0.7, None);
    let again = patch_core(0.7, Some(&first));
    let finer = patch_core(0.5, Some(&first));
    let (StudyDomain::Patch(a), StudyDomain::Patch(b), StudyDomain::Patch(c)) =
        (first.domain(), again.domain(), finer.domain())
    else {
        unreachable!("not patch studies");
    };
    let a: Vec<_> = a.atlas.allocations().cloned().collect();
    assert_eq!(a, b.atlas.allocations().cloned().collect::<Vec<_>>());
    // A different spacing starts afresh, exactly as a first run would.
    let fresh = patch_core(0.5, None);
    let StudyDomain::Patch(fresh) = fresh.domain() else {
        unreachable!("not a patch study");
    };
    assert_eq!(c.atlas.cursor(), fresh.atlas.cursor());

    let first = triangle_core(0.7, None);
    let again = triangle_core(0.7, Some(&first));
    let (StudyDomain::Triangle(a), StudyDomain::Triangle(b)) = (first.domain(), again.domain())
    else {
        unreachable!("not triangle studies");
    };
    assert_eq!(a.atlas.tiles, b.atlas.tiles);
    assert_eq!(a.allocations.cursor(), b.allocations.cursor());
}

#[test]
fn an_atlas_that_cannot_fit_is_refused() {
    let study = StudyCore::patch(PatchStudy {
        series: day(2),
        spacing: 0.1,
        grid: Scene::new().patch_grid(0.1),
        previous: None,
        max_dimension: 16,
    });
    assert_eq!(study.err(), Some(StudyError::AtlasRefused));
    let mut grid = Scene::new().patch_grid(0.7);
    grid.normals.pop();
    let study = StudyCore::patch(PatchStudy {
        series: day(2),
        spacing: 0.7,
        grid,
        previous: None,
        max_dimension: 8192,
    });
    assert_eq!(study.err(), Some(StudyError::BadSamples));
}

#[test]
fn the_summary_follows_the_session_generation() {
    let core = triangle_core(0.7, None);
    assert_eq!(core.sync(StudyInputs::default()), Ok(()));
    let empty = core.summary();
    assert!(empty.is_ok_and(|s| s.max_hours.abs() < f64::EPSILON));
    converge(&core);
    let full = core.summary().unwrap_or_default();
    let hours = core.sun_hours().unwrap_or_default();
    assert_eq!(full, summarize_sun_hours(&hours, 4.0));
    assert!(full.fully_lit > 0 && full.fully_shaded > 0);
    // A new geometry version resets the session; the summary follows it.
    let reset = StudyInputs {
        geometry_version: 9,
        ..StudyInputs::default()
    };
    assert_eq!(core.sync(reset), Ok(()));
    assert!(
        core.summary()
            .is_ok_and(|s| s.max_hours.abs() < f64::EPSILON)
    );
}

fn reuse_meshes(scene: &Scene) -> Vec<ReuseMesh<'_>> {
    vec![ReuseMesh {
        guid: "11111111-2222-3333-4444-555555555555",
        vertices: &scene.vertices,
        triangles: &scene.triangles,
    }]
}

#[test]
fn an_unchanged_rerun_reuses_every_sample_without_tracing() {
    let scene = Scene::new();
    let meshes = reuse_meshes(&scene);
    let mut source = triangle_core(0.7, None);
    source.set_sample_meshes(&meshes);
    converge(&source);
    let mut target = triangle_core(0.7, Some(&source));
    // A triangle study's groups are the scene's element ids; map them all to
    // the one mesh, as a snapshot of one element would.
    source.sample_meshes.fill(0);
    target.set_sample_meshes(&meshes);
    target.sample_meshes.fill(0);
    assert_eq!(target.sync(StudyInputs::default()), Ok(()));
    let roles = [0_u8];
    let context = ReuseContext {
        source_meshes: &meshes,
        source_roles: &roles,
        target_meshes: &meshes,
        target_roles: &roles,
        source_default_ray_bounds: true,
        is_cancelled: None,
    };
    let outcome = target.reuse_from(&source, &context);
    let samples = target.samples().count;
    assert_eq!(
        outcome,
        Ok(Some(ReuseOutcome {
            reused_samples: samples,
            reused_sample_steps: samples * 4,
        }))
    );
    assert_eq!(
        target.reuse(),
        outcome.unwrap_or_default().unwrap_or_default()
    );
    assert!(target.session().progress().is_ok_and(|p| p.converged));
    assert_eq!(target.sun_hours(), source.sun_hours());

    // Without the default ray bounds nothing is reused.
    let mut other = triangle_core(0.7, Some(&source));
    other.set_sample_meshes(&meshes);
    other.sample_meshes.fill(0);
    assert_eq!(other.sync(StudyInputs::default()), Ok(()));
    let strict = ReuseContext {
        source_default_ray_bounds: false,
        ..context
    };
    assert_eq!(other.reuse_from(&source, &strict), Ok(None));
    assert_eq!(other.reuse(), ReuseOutcome::default());
}

#[test]
fn patch_sample_meshes_follow_the_span_elements() {
    let scene = Scene::new();
    let (wall, slab) = scene.triangles.split_at(36);
    let meshes = [
        ReuseMesh {
            guid: "wall",
            vertices: &scene.vertices,
            triangles: wall,
        },
        ReuseMesh {
            guid: "slab",
            vertices: &scene.vertices,
            triangles: slab,
        },
    ];
    let mut core = patch_core(0.7, None);
    core.set_sample_meshes(&meshes);
    let StudyDomain::Patch(p) = core.domain() else {
        unreachable!("not a patch study");
    };
    for (sample, span) in p.grid.span_of.iter().enumerate() {
        assert_eq!(core.sample_meshes().get(sample), Some(span));
    }
    // A missing element clears the map; an ambiguous one leaves it.
    let before = core.sample_meshes().to_vec();
    let twice = [meshes[0], meshes[0]];
    core.set_sample_meshes(&twice);
    assert_eq!(core.sample_meshes(), before.as_slice());
    core.set_sample_meshes(meshes.split_at(1).0);
    assert_eq!(core.sample_meshes(), []);
}
