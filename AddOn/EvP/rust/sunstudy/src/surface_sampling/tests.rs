//! Mirrors the contracts of `tests/cpp/test_sunstudysurfacesampling.cpp`.

use super::*;

/// A closed box mesh: 8 vertices, 12 outward triangles.
fn cube(x: f64, size: f64) -> (Vec<f64>, Vec<u32>) {
    let s = size;
    let vertices = vec![
        x,
        0.0,
        0.0,
        x + s,
        0.0,
        0.0,
        x + s,
        s,
        0.0,
        x,
        s,
        0.0,
        x,
        0.0,
        s,
        x + s,
        0.0,
        s,
        x + s,
        s,
        s,
        x,
        s,
        s,
    ];
    let triangles = vec![
        0, 3, 2, 0, 2, 1, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6,
        3, 0, 4, 3, 4, 7,
    ];
    (vertices, triangles)
}

struct Scene {
    parts: Vec<(String, Vec<f64>, Vec<u32>)>,
}

impl Scene {
    fn new() -> Self {
        let (a, ta) = cube(0.0, 4.0);
        let (b, tb) = cube(6.0, 2.0);
        let (c, tc) = cube(70.0, 3.0);
        Self {
            parts: vec![
                ("near".into(), a, ta),
                ("wall".into(), b, tb),
                ("far".into(), c, tc),
            ],
        }
    }

    fn meshes(&self) -> Vec<SamplingMesh<'_>> {
        self.parts
            .iter()
            .map(|(guid, vertices, triangles)| SamplingMesh {
                guid,
                vertices,
                triangles,
            })
            .collect()
    }
}

fn options(domain: SamplingDomain) -> SurfaceSamplingOptions {
    SurfaceSamplingOptions {
        domain,
        spacing: 0.75,
        ..SurfaceSamplingOptions::default()
    }
}

fn sample(
    meshes: &[SamplingMesh<'_>],
    options: SurfaceSamplingOptions,
    previous: Option<PreviousSampling<'_>>,
) -> SurfaceSamplingResult {
    build_surface_sampling(&SurfaceSamplingRequest {
        meshes,
        sample_mask: &[1, 1, 1],
        options,
        previous,
        face_masks: &[],
        is_cancelled: None,
    })
}

fn previous<'a>(
    meshes: &'a [SamplingMesh<'a>],
    result: &'a SurfaceSamplingResult,
    domain: SamplingDomain,
) -> PreviousSampling<'a> {
    PreviousSampling {
        meshes,
        domain,
        layout: &result.layout,
        sample_grid: &result.triangles,
        patch_grid: &result.patches,
    }
}

#[test]
fn a_cold_build_samples_every_mesh_and_partitions_the_grid() {
    for domain in [SamplingDomain::TriangleLegacy, SamplingDomain::SurfacePatch] {
        let scene = Scene::new();
        let meshes = scene.meshes();
        let result = sample(&meshes, options(domain), None);
        assert!(result.valid && result.layout.valid);
        assert_eq!((result.rebuilt_meshes, result.reused_meshes), (3, 0));
        let total: usize = result.layout.meshes.iter().map(|m| m.sample_count).sum();
        assert_eq!(total, result.triangles.count() + result.patches.count());
        assert_eq!(
            result
                .layout
                .meshes
                .iter()
                .map(|m| m.first_face)
                .collect::<Vec<_>>(),
            [0, 12, 24]
        );
    }
}

#[test]
fn an_unchanged_snapshot_reuses_every_mesh_exactly() {
    for domain in [SamplingDomain::TriangleLegacy, SamplingDomain::SurfacePatch] {
        let scene = Scene::new();
        let meshes = scene.meshes();
        let first = sample(&meshes, options(domain), None);
        let second = sample(
            &meshes,
            options(domain),
            Some(previous(&meshes, &first, domain)),
        );
        assert_eq!((second.reused_meshes, second.rebuilt_meshes), (3, 0));
        assert_eq!(second.triangles, first.triangles);
        assert_eq!(second.patches, first.patches);
        assert_eq!(second.layout, first.layout);
    }
}

#[test]
fn a_moved_mesh_rebuilds_only_itself() {
    let scene = Scene::new();
    let meshes = scene.meshes();
    let first = sample(&meshes, options(SamplingDomain::TriangleLegacy), None);
    let mut moved = Scene::new();
    if let Some(part) = moved.parts.get_mut(1) {
        part.1 = cube(6.5, 2.0).0;
    }
    let moved_meshes = moved.meshes();
    let previous = previous(&meshes, &first, SamplingDomain::TriangleLegacy);
    let second = sample(
        &moved_meshes,
        options(SamplingDomain::TriangleLegacy),
        Some(previous),
    );
    assert_eq!((second.reused_meshes, second.rebuilt_meshes), (2, 1));
    assert_eq!(
        second,
        sample(&moved_meshes, options(SamplingDomain::TriangleLegacy), None).with_counts(&second)
    );
}

impl SurfaceSamplingResult {
    /// The same grids with another result's reuse counters, for comparing a
    /// reused build with a cold one.
    fn with_counts(mut self, other: &Self) -> Self {
        self.reused_meshes = other.reused_meshes;
        self.rebuilt_meshes = other.rebuilt_meshes;
        self.reused_samples = other.reused_samples;
        self.generated_samples = other.generated_samples;
        self
    }
}

#[test]
fn a_settings_change_rebuilds_everything_and_the_cap_refuses() {
    let scene = Scene::new();
    let meshes = scene.meshes();
    let first = sample(&meshes, options(SamplingDomain::TriangleLegacy), None);
    let finer = SurfaceSamplingOptions {
        spacing: 0.5,
        ..options(SamplingDomain::TriangleLegacy)
    };
    let second = sample(
        &meshes,
        finer,
        Some(previous(&meshes, &first, SamplingDomain::TriangleLegacy)),
    );
    assert_eq!((second.reused_meshes, second.rebuilt_meshes), (0, 3));
    let capped = SurfaceSamplingOptions {
        max_samples: 10,
        ..options(SamplingDomain::TriangleLegacy)
    };
    assert!(!sample(&meshes, capped, None).valid);
}

#[test]
fn ambiguous_guids_fall_back_to_the_whole_scene() {
    let mut scene = Scene::new();
    if let Some(part) = scene.parts.get_mut(2) {
        part.0 = "{NEAR}".into();
    }
    let meshes = scene.meshes();
    let result = sample(&meshes, options(SamplingDomain::TriangleLegacy), None);
    assert!(result.valid);
    assert!(
        !result.layout.valid,
        "the whole-scene path keeps no per-mesh layout"
    );
    assert_eq!(result.rebuilt_meshes, 3);
}

#[test]
fn cancellation_and_malformed_inputs_publish_no_partial_grid() {
    let scene = Scene::new();
    let meshes = scene.meshes();
    let stop = || true;
    let cancelled = build_surface_sampling(&SurfaceSamplingRequest {
        meshes: &meshes,
        sample_mask: &[1, 1, 1],
        options: options(SamplingDomain::TriangleLegacy),
        previous: None,
        face_masks: &[],
        is_cancelled: Some(&stop),
    });
    assert_eq!(cancelled, SurfaceSamplingResult::default());
    let short_mask = build_surface_sampling(&SurfaceSamplingRequest {
        meshes: &meshes,
        sample_mask: &[1, 1],
        options: options(SamplingDomain::TriangleLegacy),
        previous: None,
        face_masks: &[],
        is_cancelled: None,
    });
    assert_eq!(short_mask, SurfaceSamplingResult::default());
    let bad = SurfaceSamplingOptions {
        spacing: f64::NAN,
        ..options(SamplingDomain::TriangleLegacy)
    };
    assert_eq!(sample(&meshes, bad, None), SurfaceSamplingResult::default());
}

#[test]
fn a_context_mesh_keeps_its_faces_but_no_samples() {
    let scene = Scene::new();
    let meshes = scene.meshes();
    let result = build_surface_sampling(&SurfaceSamplingRequest {
        meshes: &meshes,
        sample_mask: &[1, 0, 1],
        options: options(SamplingDomain::TriangleLegacy),
        previous: None,
        face_masks: &[],
        is_cancelled: None,
    });
    let wall = result.layout.meshes.get(1).cloned().unwrap_or_default();
    assert!(!wall.sampled && wall.sample_count == 0 && wall.excluded == 12);
}
