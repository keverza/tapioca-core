//! Pure cases of the seen-surface pass. Bit parity with the C++ pass over a
//! real BVH is `SunStudyRustViewsParity.ViewpointMatchesEveryBitInEveryMode`.

use super::*;
use crate::rays::{FirstHitRay, OcclusionRay, RayHit};
use std::cell::Cell;

/// A wall at x = 5 (mesh 1). First hits honour the mask; occlusion ignores it.
struct Wall {
    cancel: bool,
    first_hits: Cell<usize>,
    occlusions: Cell<usize>,
}

fn wall() -> Wall {
    Wall {
        cancel: false,
        first_hits: Cell::new(0),
        occlusions: Cell::new(0),
    }
}

fn blocks(origin: &[f64; 3], dir: &[f64; 3], length: f64) -> bool {
    let t = (5.0 - origin[0]) / dir[0];
    dir[0] > 1e-9 && t <= length
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
        _: usize,
    ) -> Result<(), RayError> {
        self.first_hits.set(self.first_hits.get() + 1);
        if self.cancel {
            return Err(RayError::Cancelled);
        }
        let hittable = mask.is_none_or(|m| m.get(1).is_some_and(|f| *f != 0));
        for (ray, hit) in rays.iter().zip(out.iter_mut()) {
            *hit = RayHit {
                hit: hittable && blocks(&ray.origin, &ray.dir, ray.length),
                t: 0.0,
                mesh: 1,
            };
        }
        Ok(())
    }

    fn occlude_rays(
        &self,
        rays: &[OcclusionRay],
        out: &mut [u8],
        _: usize,
    ) -> Result<(), RayError> {
        self.occlusions.set(self.occlusions.get() + 1);
        for (ray, blocked) in rays.iter().zip(out.iter_mut()) {
            *blocked = u8::from(blocks(&ray.origin, &ray.dir, ray.tmax));
        }
        Ok(())
    }

    fn cancelled(&self) -> bool {
        self.cancel
    }
}

/// Mesh 0: a 20 m square floor at z = 0 centred on the origin. Mesh 1: a
/// sliver standing in for the wall, far away so that it is never near.
const FLOOR_VERTICES: [f64; 12] = [
    -10.0, -10.0, 0.0, 10.0, -10.0, 0.0, 10.0, 10.0, 0.0, -10.0, 10.0, 0.0,
];
const FLOOR_TRIANGLES: [u32; 6] = [0, 1, 2, 0, 2, 3];
const WALL_VERTICES: [f64; 9] = [500.0, 0.0, 0.0, 500.0, 1.0, 0.0, 500.0, 0.0, 1.0];
const WALL_TRIANGLES: [u32; 3] = [0, 1, 2];

fn meshes() -> [SamplingMesh<'static>; 2] {
    [
        SamplingMesh {
            guid: "floor",
            vertices: &FLOOR_VERTICES,
            triangles: &FLOOR_TRIANGLES,
        },
        SamplingMesh {
            guid: "wall",
            vertices: &WALL_VERTICES,
            triangles: &WALL_TRIANGLES,
        },
    ]
}

fn run(
    meshes: &[SamplingMesh<'_>],
    context: &[&str],
    options: &ViewpointOptions,
    cell: f64,
    queries: &dyn RayQueries,
) -> SeenSurfaces {
    let Ok(seen) = seen_surfaces(meshes, context, options, cell, queries) else {
        unreachable!("refused");
    };
    seen
}

fn sphere(radius: f64) -> ViewpointOptions {
    ViewpointOptions {
        point: [0.0, 0.0, 1.0],
        radius,
        mode: ViewpointMode::Sphere,
        ..ViewpointOptions::default()
    }
}

fn floor_area() -> f64 {
    400.0
}

#[test]
fn a_floor_wholly_in_reach_is_kept_whole() {
    let seen = run(&meshes(), &[], &sphere(100.0), 0.0, &wall());
    assert_eq!(seen.triangles.len(), 6, "both floor triangles, uncut");
    assert_eq!(
        seen.triangles.get(..3),
        Some(&[[-10.0, -10.0, 0.0], [10.0, -10.0, 0.0], [10.0, 10.0, 0.0]][..])
    );
    assert!((seen.area - floor_area()).abs() < 1e-9);
    assert!(
        (seen.cell_metres - 1.0).abs() < 1e-12,
        "radius / 80 capped at 1 m"
    );
}

#[test]
fn the_radius_cuts_the_floor_at_its_crossing() {
    let radius = 6.0;
    let seen = run(&meshes(), &[], &sphere(radius), 0.25, &wall());
    // The seen floor is the disc of radius sqrt(r^2 - 1) under the eye.
    let disc = std::f64::consts::PI * (radius * radius - 1.0);
    assert!(
        (seen.area - disc).abs() / disc < 0.02,
        "area {} vs disc {disc}",
        seen.area
    );
    let eye = [0.0, 0.0, 1.0];
    for corner in &seen.triangles {
        assert!(distance(&eye, corner) <= radius + 2.0 * CROSSING_METRES);
    }
    assert!(
        (seen.cell_metres - 0.25).abs() < 1e-12,
        "a requested cell is kept when it fits"
    );
}

#[test]
fn the_elevation_band_hides_what_lies_below_it() {
    let mut options = sphere(100.0);
    options.min_elevation_degrees = 0.0;
    let seen = run(&meshes(), &[], &options, 0.0, &wall());
    assert_eq!(seen.triangles.len(), 0);
    assert!(seen.area.abs() < f64::EPSILON);
}

/// x > 5 is behind the wall, give or take the seen slack (a ray stopping
/// within it of its target still reaches it).
fn behind_the_wall(seen: &SeenSurfaces) {
    let slack = SEEN_SLACK_METRES + 2.0 * CROSSING_METRES;
    let kept = floor_area() * 0.75;
    assert!(
        seen.area >= kept && seen.area <= kept + 20.0 * slack,
        "area {}",
        seen.area
    );
    for corner in &seen.triangles {
        assert!(corner[0] <= 5.0 + slack);
    }
}

#[test]
fn context_shadows_the_floor_behind_it_through_the_mask() {
    let queries = wall();
    let seen = run(&meshes(), &["wall"], &sphere(100.0), 0.5, &queries);
    assert!(
        queries.first_hits.get() > 1,
        "corners, then bisection rounds"
    );
    assert_eq!(queries.occlusions.get(), 0);
    behind_the_wall(&seen);
}

#[test]
fn every_mesh_in_context_asks_for_plain_occlusion() {
    let queries = wall();
    let seen = run(&meshes(), &["floor", "wall"], &sphere(100.0), 0.5, &queries);
    assert_eq!(queries.first_hits.get(), 0);
    assert!(queries.occlusions.get() > 1);
    behind_the_wall(&seen);
}

#[test]
fn off_the_sphere_nothing_is_built_and_no_ray_is_cast() {
    let queries = wall();
    let mut options = sphere(100.0);
    options.mode = ViewpointMode::Plan;
    let seen = run(&meshes(), &["wall"], &options, 0.0, &queries);
    assert_eq!(seen, SeenSurfaces::default());
    assert_eq!(queries.first_hits.get(), 0);
}

#[test]
fn refusals_and_cancellation_are_the_studys() {
    let mut bad = sphere(100.0);
    bad.radius = -1.0;
    assert_eq!(
        seen_surfaces(&meshes(), &[], &bad, 0.0, &wall()),
        Err(ViewpointError::BadOptions)
    );
    assert_eq!(
        seen_surfaces(&meshes(), &["absent"], &sphere(100.0), 0.0, &wall()),
        Err(ViewpointError::MissingContext)
    );
    let cancelled = Wall {
        cancel: true,
        ..wall()
    };
    assert_eq!(
        seen_surfaces(&meshes(), &["wall"], &sphere(100.0), 0.0, &cancelled),
        Err(ViewpointError::Cancelled)
    );
}

/// 400 one-metre right triangles over the floor.
fn tiles() -> (Vec<f64>, Vec<u32>) {
    let mut vertices = Vec::new();
    let mut triangles = Vec::new();
    for k in 0..400_u32 {
        let x = f64::from(k % 20) - 10.0;
        let y = f64::from(k / 20) - 10.0;
        let base = k * 3;
        vertices.extend_from_slice(&[x, y, 0.0, x + 1.0, y, 0.0, x, y + 1.0, 0.0]);
        triangles.extend_from_slice(&[base, base + 1, base + 2]);
    }
    (vertices, triangles)
}

#[test]
fn a_fine_cell_coarsens_until_the_run_fits_its_budget() {
    let (vertices, triangles) = tiles();
    let tiles = [SamplingMesh {
        guid: "tiles",
        vertices: &vertices,
        triangles: &triangles,
    }];
    let seen = run(&tiles, &[], &sphere(100.0), 0.01, &wall());
    assert!(seen.cell_metres > 0.01);
    let near = near_triangles(&tiles, &sphere(100.0));
    let cells: usize = near
        .iter()
        .map(|t| splits(t, seen.cell_metres).pow(2))
        .sum();
    assert!(cells <= MAX_SURFACE_CELLS);
}

#[test]
fn batches_do_not_change_the_answer() {
    // Many small triangles, so that the run spans several batches.
    let (vertices, triangles) = tiles();
    let tiles = [SamplingMesh {
        guid: "tiles",
        vertices: &vertices,
        triangles: &triangles,
    }];
    let options = sphere(8.0);
    let whole = run(&tiles, &[], &options, 0.02, &wall());
    let near = near_triangles(&tiles, &options);
    assert!(
        batch_end(&near, 0, whole.cell_metres) < near.len(),
        "the run is batched"
    );
    // Each triangle on its own gives the same pieces in the same order.
    let mut one_by_one = SeenSurfaces {
        cell_metres: whole.cell_metres,
        ..SeenSurfaces::default()
    };
    let queries = wall();
    let sight = Sight {
        eye: options.point,
        radius: options.radius,
        sin_low: (options.min_elevation_degrees * RADIANS).sin(),
        sin_high: (options.max_elevation_degrees * RADIANS).sin(),
        mask: &[0],
        everything: false,
        traced: false,
        queries: &queries,
    };
    for t in &near {
        let Ok(()) = seen_batch(
            &sight,
            std::slice::from_ref(t),
            whole.cell_metres,
            &mut one_by_one,
        ) else {
            unreachable!("refused");
        };
    }
    assert_eq!(whole, one_by_one);
}
