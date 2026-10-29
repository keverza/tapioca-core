//! Mirrors the pure cases of `tests/cpp/test_viewpointstudy.cpp`; parity with
//! the C++ study over a real BVH is in `test_sunstudy_rust_views.cpp`.

use super::*;
use crate::rays::OcclusionRay;
use std::cell::Cell;

/// A wall at x = 5 belonging to mesh 1, hittable only through the mask.
struct Wall {
    cancel: bool,
    calls: Cell<usize>,
}

impl RayQueries for Wall {
    fn prepare(&self) -> Result<(), RayError> {
        Ok(())
    }

    fn first_hits(
        &self,
        rays: &[FirstHitRay],
        mask: Option<&[u8]>,
        out: &mut [RayHit],
        _max_parallel: usize,
    ) -> Result<(), RayError> {
        self.calls.set(self.calls.get() + 1);
        if self.cancel {
            return Err(RayError::Cancelled);
        }
        let hittable = mask.is_none_or(|m| m.get(1).is_some_and(|f| *f != 0));
        for (ray, hit) in rays.iter().zip(out.iter_mut()) {
            let t = (5.0 - ray.origin[0]) / ray.dir[0];
            *hit = RayHit {
                hit: hittable && ray.dir[0] > 1e-9 && t <= ray.length,
                t,
                mesh: 1,
            };
        }
        Ok(())
    }

    fn occlude_rays(&self, _: &[OcclusionRay], _: &mut [u8], _: usize) -> Result<(), RayError> {
        Err(RayError::Failed)
    }

    fn cancelled(&self) -> bool {
        self.cancel
    }
}

fn wall() -> Wall {
    Wall {
        cancel: false,
        calls: Cell::new(0),
    }
}

const MESHES: [&str; 2] = ["floor", "wall"];

#[test]
fn an_open_viewpoint_is_the_full_disc_without_querying() {
    let w = wall();
    let options = ViewpointOptions {
        point: [0.0, 0.0, 1.6],
        radius: 10.0,
        rays: 1440,
        ..ViewpointOptions::default()
    };
    let r = run_viewpoint(&MESHES, &[], &options, &w);
    let Ok(r) = r else {
        unreachable!("refused");
    };
    assert_eq!(w.calls.get(), 0);
    assert_eq!(r.perimeter.len(), 1440);
    assert_eq!(r.collision_count, 0);
    assert!(
        r.ray_mesh.iter().all(Option::is_none),
        "every ray reaches the radius"
    );
    assert!((r.area - std::f64::consts::PI * 100.0).abs() < 0.1);
    assert!(
        r.perimeter
            .iter()
            .all(|p| (p[2] - 1.6).abs() < f64::EPSILON)
    );
}

#[test]
fn context_clips_the_isovist_at_the_first_hit() {
    let w = wall();
    let options = ViewpointOptions {
        point: [0.0, 0.0, 1.0],
        radius: 10.0,
        rays: 360,
        ..ViewpointOptions::default()
    };
    let Ok(r) = run_viewpoint(&MESHES, &["WALL"], &options, &w) else {
        unreachable!("refused");
    };
    assert_eq!(w.calls.get(), 1);
    assert!(r.collision_count > 0);
    assert_eq!(r.ray_mesh.len(), r.perimeter.len());
    assert_eq!(
        r.ray_mesh.iter().filter(|m| m.is_some()).count(),
        r.collision_count,
        "one blocker per collision"
    );
    for (mesh, p) in r.ray_mesh.iter().zip(&r.perimeter) {
        match mesh {
            Some(m) => assert!(
                *m == 1 && (p[0] - 5.0).abs() < 1e-9,
                "stopped rays end on the wall"
            ),
            None => assert!(p[0] < 5.0 + 1e-9, "open rays end on the radius"),
        }
    }
    assert!(r.perimeter.iter().all(|p| p[0] <= 5.0 + 1e-9));
    assert!(r.area < std::f64::consts::PI * 100.0);
}

#[test]
fn refusals_carry_the_cpp_messages() {
    let w = wall();
    for options in [
        ViewpointOptions {
            rays: 31,
            ..ViewpointOptions::default()
        },
        ViewpointOptions {
            radius: 0.0,
            ..ViewpointOptions::default()
        },
        ViewpointOptions {
            point: [f64::NAN, 0.0, 0.0],
            ..ViewpointOptions::default()
        },
    ] {
        assert_eq!(
            run_viewpoint(&MESHES, &[], &options, &w),
            Err(ViewpointError::BadOptions)
        );
    }
    let defaults = ViewpointOptions::default();
    assert_eq!(
        run_viewpoint(&MESHES, &["absent"], &defaults, &w),
        Err(ViewpointError::MissingContext)
    );
    let cancelled = Wall {
        cancel: true,
        calls: Cell::new(0),
    };
    let err = run_viewpoint(&MESHES, &["wall"], &defaults, &cancelled);
    assert_eq!(err, Err(ViewpointError::Cancelled));
    assert_eq!(
        ViewpointError::Cancelled.message(),
        "viewpoint calculation cancelled"
    );
}

#[test]
fn a_field_of_view_is_an_open_sector_at_the_plan_density() {
    let w = wall();
    let options = ViewpointOptions {
        point: [0.0, 0.0, 1.0],
        radius: 10.0,
        rays: 360,
        mode: ViewpointMode::FieldOfView,
        bearing_degrees: 90.0,
        field_of_view_degrees: 90.0,
        ..ViewpointOptions::default()
    };
    let Ok(r) = run_viewpoint(&MESHES, &[], &options, &w) else {
        unreachable!("refused");
    };
    assert_eq!(r.perimeter.len(), 91, "both edges of the angle");
    let edge = 10.0 * (45.0_f64).to_radians().cos();
    let (Some(first), Some(last)) = (r.perimeter.first(), r.perimeter.last()) else {
        unreachable!("no rays");
    };
    assert!((first[0] - edge).abs() < 1e-9, "from bearing - 45 degrees");
    assert!((last[0] + edge).abs() < 1e-9, "to bearing + 45 degrees");
    assert!(
        (r.area - std::f64::consts::PI * 25.0).abs() < 0.05,
        "a quarter disc, not closed back across it"
    );
    assert!((r.open_fraction - 1.0).abs() < f64::EPSILON);
}

#[test]
fn a_sphere_weighs_its_rows_by_solid_angle() {
    let w = wall();
    let options = ViewpointOptions {
        point: [0.0, 0.0, 0.0],
        radius: 10.0,
        mode: ViewpointMode::Sphere,
        sphere_columns: 64,
        sphere_rows: 17,
        ..ViewpointOptions::default()
    };
    let Ok(open) = run_viewpoint(&MESHES, &[], &options, &w) else {
        unreachable!("refused");
    };
    assert_eq!(open.perimeter.len(), 64 * 17);
    assert!(
        open.perimeter.first().is_some_and(|p| p[2] < -9.99),
        "row-major from the lowest elevation"
    );
    let ball = 4.0 / 3.0 * std::f64::consts::PI * 1000.0;
    assert!(
        (open.volume - ball).abs() < 1e-6 * ball,
        "the bands add up to the whole sphere"
    );
    assert!((open.open_fraction - 1.0).abs() < 1e-12);
    assert!(open.area.abs() < f64::EPSILON, "a sphere has no plan area");
    let Ok(walled) = run_viewpoint(&MESHES, &["wall"], &options, &w) else {
        unreachable!("refused");
    };
    assert!(walled.collision_count > 0);
    assert!(walled.volume < open.volume);
    assert!(walled.open_fraction > 0.4 && walled.open_fraction < 1.0);
}

#[test]
fn mode_refusals_carry_the_cpp_messages() {
    let w = wall();
    let narrow = ViewpointOptions {
        mode: ViewpointMode::FieldOfView,
        field_of_view_degrees: 0.5,
        ..ViewpointOptions::default()
    };
    assert_eq!(
        run_viewpoint(&MESHES, &[], &narrow, &w),
        Err(ViewpointError::BadFieldOfView)
    );
    for sphere in [
        ViewpointOptions {
            min_elevation_degrees: 10.0,
            max_elevation_degrees: 10.0,
            ..ViewpointOptions::default()
        },
        ViewpointOptions {
            sphere_columns: 7,
            ..ViewpointOptions::default()
        },
        ViewpointOptions {
            sphere_rows: 362,
            ..ViewpointOptions::default()
        },
    ] {
        let options = ViewpointOptions {
            mode: ViewpointMode::Sphere,
            ..sphere
        };
        assert_eq!(
            run_viewpoint(&MESHES, &[], &options, &w),
            Err(ViewpointError::BadSphere)
        );
    }
    let plan = ViewpointOptions {
        sphere_rows: 0,
        field_of_view_degrees: 0.0,
        ..ViewpointOptions::default()
    };
    assert!(
        run_viewpoint(&MESHES, &[], &plan, &w).is_ok(),
        "a plan ignores the other modes' settings"
    );
}
