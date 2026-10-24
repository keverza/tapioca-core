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
    };
    let r = run_viewpoint(&MESHES, &[], &options, &w);
    let Ok(r) = r else {
        unreachable!("refused");
    };
    assert_eq!(w.calls.get(), 0);
    assert_eq!(r.perimeter.len(), 1440);
    assert_eq!(r.collision_count, 0);
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
    };
    let Ok(r) = run_viewpoint(&MESHES, &["WALL"], &options, &w) else {
        unreachable!("refused");
    };
    assert_eq!(w.calls.get(), 1);
    assert!(r.collision_count > 0);
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
