//! Reuse planning on a floor / wall / far-floor scene, mirroring the shape of
//! `tests/cpp/test_sunstudyreuse.cpp` (the C++ differential is the oracle).

use super::*;
use crate::series::SunStep;

fn floor(x: f64) -> Vec<f64> {
    vec![
        x,
        0.0,
        0.0,
        x + 12.0,
        0.0,
        0.0,
        x + 12.0,
        8.0,
        0.0,
        x,
        8.0,
        0.0,
    ]
}

const QUAD: [u32; 6] = [0, 1, 2, 0, 2, 3];

fn day(steps: usize) -> SunSeries {
    let raw: Vec<SunStep> = (0..steps)
        .map(|i| {
            let angle = 0.5 + crate::series::count_to_f64(i % 4) * 0.1;
            SunStep {
                hour: 8,
                minute: i32::try_from(i).unwrap_or(0),
                direction: [angle.cos(), 0.0, angle.sin()],
                altitude_degrees: angle.to_degrees(),
            }
        })
        .collect();
    SunSeries::from_steps(&raw, 1, 0.0)
}

/// Samples one per floor quad corner region: (mesh, x) pairs on z = 0.01.
fn samples(xs: &[(u32, f64)]) -> (Vec<f64>, Vec<f64>, Vec<u32>) {
    let mut positions = Vec::new();
    let mut normals = Vec::new();
    let mut meshes = Vec::new();
    for &(mesh, x) in xs {
        positions.extend_from_slice(&[x, 4.0, 0.01]);
        normals.extend_from_slice(&[0.0, 0.0, 1.0]);
        meshes.push(mesh);
    }
    (positions, normals, meshes)
}

struct Owned {
    near: Vec<f64>,
    wall: Vec<f64>,
    far: Vec<f64>,
    series: SunSeries,
    positions: Vec<f64>,
    normals: Vec<f64>,
    sample_meshes: Vec<u32>,
}

fn scene(wall_x: f64, steps: usize) -> Owned {
    let (positions, normals, sample_meshes) = samples(&[(0, 2.0), (0, 10.0), (2, 72.0), (2, 80.0)]);
    Owned {
        near: floor(0.0),
        wall: vec![
            wall_x, 0.0, 0.0, wall_x, 8.0, 0.0, wall_x, 8.0, 4.0, wall_x, 0.0, 4.0,
        ],
        far: floor(70.0),
        series: day(steps),
        positions,
        normals,
        sample_meshes,
    }
}

fn meshes(o: &Owned) -> [ReuseMesh<'_>; 3] {
    [
        ReuseMesh {
            guid: "near",
            vertices: &o.near,
            triangles: &QUAD,
        },
        ReuseMesh {
            guid: "wall",
            vertices: &o.wall,
            triangles: &QUAD,
        },
        ReuseMesh {
            guid: "far",
            vertices: &o.far,
            triangles: &QUAD,
        },
    ]
}

fn study<'a>(o: &'a Owned, m: &'a [ReuseMesh<'a>]) -> ReuseStudy<'a> {
    ReuseStudy {
        meshes: m,
        roles: &[0, 0, 0],
        positions: &o.positions,
        normals: &o.normals,
        sample_meshes: &o.sample_meshes,
        series: &o.series,
        grid_spacing: 0.75,
        domain: 0,
    }
}

fn plan(source: &Owned, target: &Owned) -> Option<ReusePlan> {
    let (ms, mt) = (meshes(source), meshes(target));
    plan_reuse(&ReuseRequest {
        source: study(source, &ms),
        target: study(target, &mt),
        source_converged: true,
        source_default_ray_bounds: true,
        is_cancelled: None,
    })
}

#[test]
fn an_identical_study_reuses_every_sample_and_step() {
    let (a, b) = (scene(6.0, 4), scene(6.0, 4));
    let p = plan(&a, &b);
    assert!(
        p.as_ref()
            .is_some_and(|p| p.reused_samples == 4 && p.reused_sample_steps == 16)
    );
    assert!(p.is_some_and(|p| p.map == [0, 1, 2, 3] && p.dirty_steps.iter().all(|w| *w == 0)));
}

#[test]
fn a_moved_wall_dirties_only_samples_its_shadow_can_reach() {
    let (a, b) = (scene(6.0, 4), scene(7.0, 4));
    let p = plan(&a, &b);
    let Some(p) = p else {
        unreachable!("a local edit keeps the far floor");
    };
    assert_eq!(p.map, [0, 1, 2, 3], "unchanged meshes keep identity");
    // The far floor (x >= 70) is outside the sunward rays of both walls.
    assert_eq!(p.dirty_steps.get(2..), Some(&[0_u64, 0][..]));
    assert!(p.reused_samples >= 2);
}

#[test]
fn incompatible_or_ambiguous_studies_plan_nothing() {
    let a = scene(6.0, 4);
    let mut other_day = scene(6.0, 5);
    assert_eq!(plan(&a, &other_day), None, "a different day");
    other_day = scene(6.0, 4);
    other_day.sample_meshes.pop();
    assert_eq!(plan(&a, &other_day), None, "sample map length");
    let b = scene(6.0, 4);
    let (ms, mut mt) = (meshes(&a), meshes(&b));
    mt[2].guid = "{NEAR}"; // canonical duplicate of "near"
    let request = ReuseRequest {
        source: study(&a, &ms),
        target: study(&b, &mt),
        source_converged: true,
        source_default_ray_bounds: true,
        is_cancelled: None,
    };
    assert_eq!(plan_reuse(&request), None, "ambiguous identity");
    let (ms, mt) = (meshes(&a), meshes(&b));
    let stop = || true;
    let cancelled = ReuseRequest {
        source: study(&a, &ms),
        target: study(&b, &mt),
        source_converged: true,
        source_default_ray_bounds: true,
        is_cancelled: Some(&stop),
    };
    assert_eq!(plan_reuse(&cancelled), None, "cancelled");
}

#[test]
fn the_envelope_contains_the_box_and_falls_away_from_the_sun() {
    let mut b = ChangedBounds::default();
    b.add(&[0.0, 0.0, 0.0]);
    b.add(&[1.0, 1.0, 1.0]);
    let e = shadow_influence_envelope(&b, &[[0.0, 0.0, 1.0]], 10.0);
    assert!(e.valid && e.min[2] <= -10.0 + 1e-12 && e.max[2] >= 1.0);
    assert!(e.intersects(&[0.5, 0.5, -5.0], &[0.5, 0.5, -5.0]));
    assert!(!e.intersects(&[0.5, 0.5, 5.0], &[0.5, 0.5, 5.0]));
    assert_eq!(
        shadow_influence_envelope(&ChangedBounds::default(), &[[0.0, 0.0, 1.0]], 1.0),
        ChangedBounds::default()
    );
}

#[test]
fn patch_sample_meshes_follow_canonical_guids() {
    let o = scene(6.0, 1);
    let m = meshes(&o);
    let spans = [
        SpanOwner {
            element: "{FAR}",
            first: 0,
            count: 2,
        },
        SpanOwner {
            element: "near",
            first: 3,
            count: 1,
        },
    ];
    assert_eq!(
        patch_sample_meshes(&m, 4, &spans),
        Ok(vec![2, 2, u32::MAX, 0])
    );
    let missing = [SpanOwner {
        element: "gone",
        first: 0,
        count: 1,
    }];
    assert_eq!(
        patch_sample_meshes(&m, 4, &missing),
        Err(SampleMeshError::BadSpan)
    );
    let overrun = [SpanOwner {
        element: "near",
        first: 3,
        count: 2,
    }];
    assert_eq!(
        patch_sample_meshes(&m, 4, &overrun),
        Err(SampleMeshError::BadSpan)
    );
}
