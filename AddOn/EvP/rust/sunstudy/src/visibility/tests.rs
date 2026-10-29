//! Refusal order and scoring against a fake index; parity with the C++ study
//! over a real BVH is in `tests/cpp/test_sunstudy_rust_views.cpp`.

use super::*;
use crate::rays::{FirstHitRay, OcclusionRay, RayHit};

/// Everything along +x hits the TO wall (mesh 1); nothing else is hit.
struct Fake {
    fail: bool,
}

impl RayQueries for Fake {
    fn prepare(&self) -> Result<(), RayError> {
        if self.fail {
            Err(RayError::Failed)
        } else {
            Ok(())
        }
    }

    fn first_hits(
        &self,
        rays: &[FirstHitRay],
        _mask: Option<&[u8]>,
        out: &mut [RayHit],
        _max_parallel: usize,
    ) -> Result<(), RayError> {
        for (ray, hit) in rays.iter().zip(out.iter_mut()) {
            *hit = RayHit {
                hit: ray.dir[0] > 0.0,
                t: 1.0,
                mesh: 1,
            };
        }
        Ok(())
    }

    fn occlude_rays(
        &self,
        rays: &[OcclusionRay],
        out: &mut [u8],
        _max_parallel: usize,
    ) -> Result<(), RayError> {
        for (ray, o) in rays.iter().zip(out.iter_mut()) {
            *o = u8::from(ray.origin[2] > 100.0);
        }
        Ok(())
    }

    fn cancelled(&self) -> bool {
        false
    }
}

const FLOOR: [f64; 12] = [0.0, 0.0, 0.0, 4.0, 0.0, 0.0, 4.0, 4.0, 0.0, 0.0, 4.0, 0.0];
const WALL: [f64; 12] = [6.0, 0.0, 0.0, 6.0, 4.0, 0.0, 6.0, 4.0, 3.0, 6.0, 0.0, 3.0];
const QUAD: [u32; 6] = [0, 1, 2, 0, 2, 3];

fn meshes() -> [SamplingMesh<'static>; 2] {
    [
        SamplingMesh {
            guid: "floor",
            vertices: &FLOOR,
            triangles: &QUAD,
        },
        SamplingMesh {
            guid: "wall",
            vertices: &WALL,
            triangles: &QUAD,
        },
    ]
}

fn run(
    from: &[&str],
    to: &[&str],
    options: &VisibilityOptions,
    fake: &Fake,
) -> Result<VisibilityResult, VisibilityError> {
    let m = meshes();
    run_visibility(&VisibilityRequest {
        meshes: &m,
        from,
        to,
        options,
        queries: fake,
    })
}

#[test]
fn surfaces_score_the_fraction_of_aim_points_seen() {
    let options = VisibilityOptions {
        spacing: 1.0,
        ..VisibilityOptions::default()
    };
    let Ok(r) = run(&["floor"], &["wall"], &options, &Fake { fail: false }) else {
        unreachable!("refused");
    };
    assert_eq!(r.aim_point_count, 2);
    assert_eq!(r.values.len(), r.patch_grid.count());
    assert!(r.values.iter().all(|v| (*v - 1.0).abs() < f64::EPSILON));
    assert_eq!(r.visible_samples, r.values.len());
    assert!((r.analysed_area - 16.0).abs() < 1e-9);
    assert!(r.patch_atlas.width() > 0);
}

#[test]
fn a_point_sees_what_the_cone_contains() {
    let options = VisibilityOptions {
        origin: VisibilityOrigin::Point,
        domain: SamplingDomain::TriangleLegacy,
        spacing: 1.0,
        point: [0.0, 2.0, 1.5],
        direction: [1.0, 0.0, 0.0],
        cone_degrees: 60.0,
        ..VisibilityOptions::default()
    };
    let Ok(r) = run(&[], &["wall"], &options, &Fake { fail: false }) else {
        unreachable!("refused");
    };
    assert!(r.atlas.valid);
    assert!(r.ray_count > 0 && r.ray_count <= r.values.len());
    assert_eq!(r.visible_samples, r.ray_count);
}

#[test]
fn a_sphere_sees_its_band_of_elevations_whatever_the_direction() {
    let facing_away = VisibilityOptions {
        origin: VisibilityOrigin::Point,
        domain: SamplingDomain::TriangleLegacy,
        spacing: 1.0,
        point: [0.0, 2.0, 1.5],
        direction: [-1.0, 0.0, 0.0],
        view: VisibilityView::Sphere,
        ..VisibilityOptions::default()
    };
    let Ok(all) = run(&[], &["wall"], &facing_away, &Fake { fail: false }) else {
        unreachable!("refused");
    };
    assert_eq!(
        all.ray_count,
        all.values.len(),
        "every cell is in the full sphere"
    );
    // The wall stands 6 m away and 3 m tall about a 1.5 m eye: within +-15 degrees.
    let sky = VisibilityOptions {
        min_elevation_degrees: 30.0,
        ..facing_away.clone()
    };
    let Ok(none) = run(&[], &["wall"], &sky, &Fake { fail: false }) else {
        unreachable!("refused");
    };
    assert_eq!(none.ray_count, 0);
    let inverted = VisibilityOptions {
        min_elevation_degrees: 10.0,
        max_elevation_degrees: -10.0,
        ..facing_away
    };
    assert_eq!(
        run(&[], &["wall"], &inverted, &Fake { fail: false }).map(|_| ()),
        Err(VisibilityError(
            "visibility sphere needs elevations -90 <= min <= max <= 90 degrees"
        ))
    );
}

#[test]
fn refusals_come_in_the_cpp_order() {
    let fake = Fake { fail: false };
    let base = VisibilityOptions::default();
    let cases: [(&[&str], &[&str], VisibilityOptions, &str); 7] = [
        (
            &["floor"],
            &["wall"],
            VisibilityOptions {
                spacing: 0.0,
                ..base.clone()
            },
            "visibility grid spacing must be positive",
        ),
        (
            &["floor"],
            &["wall"],
            VisibilityOptions {
                max_rays: 0,
                ..base.clone()
            },
            "visibility options contain invalid offsets, limits or origin mode",
        ),
        (
            &["floor"],
            &[],
            base.clone(),
            "visibility study needs at least one TO element",
        ),
        (
            &[],
            &["wall"],
            base.clone(),
            "surface visibility needs at least one FROM element",
        ),
        (
            &["wall"],
            &["WALL"],
            base.clone(),
            "FROM and TO element sets must not overlap",
        ),
        (
            &["floor"],
            &["absent"],
            base.clone(),
            "visibility role elements are absent from the live snapshot",
        ),
        (
            &["floor"],
            &["wall"],
            VisibilityOptions {
                origin: VisibilityOrigin::Point,
                cone_degrees: 0.5,
                ..base.clone()
            },
            "visibility point needs finite coordinates, a non-zero direction and a 1..179 degree cone",
        ),
    ];
    for (from, to, options, message) in cases {
        assert_eq!(
            run(from, to, &options, &fake),
            Err(VisibilityError(message))
        );
    }
    assert_eq!(
        run(&["floor"], &["wall"], &base, &Fake { fail: true }),
        Err(VisibilityError(
            "visibility study could not acquire the snapshot query index"
        ))
    );
}
