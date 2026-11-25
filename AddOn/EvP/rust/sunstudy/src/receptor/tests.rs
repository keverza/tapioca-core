//! Planting receptor contract (#278 Stage 1A).

use super::*;
use crate::sampler::{SampleGrid, SamplerOptions, build_sample_grid};

const OFFSET: f64 = 0.01;

type Mesh = (Vec<f64>, Vec<u32>);

/// A 10 m square, tilted by `rise` metres over its 10 m run along X.
fn slab(rise: f64) -> Mesh {
    let vertices = vec![
        0.0, 0.0, 0.0, 10.0, 0.0, rise, 10.0, 10.0, rise, 0.0, 10.0, 0.0,
    ];
    (vertices, vec![0, 1, 2, 0, 2, 3])
}

fn wall() -> Mesh {
    let vertices = vec![0.0, 0.0, 0.0, 10.0, 0.0, 0.0, 10.0, 0.0, 3.0, 0.0, 0.0, 3.0];
    (vertices, vec![0, 1, 2, 0, 2, 3])
}

fn grid(mesh: &Mesh) -> SampleGrid {
    let options = SamplerOptions {
        spacing: 1.0,
        normal_offset: OFFSET,
        want_layouts: true,
        ..SamplerOptions::default()
    };
    let grid = build_sample_grid(&mesh.0, &mesh.1, None, &options);
    assert!(grid.valid && grid.count() > 0);
    grid
}

fn host(grid: &SampleGrid) -> HostSamples<'_> {
    HostSamples {
        positions: &grid.positions,
        normals: &grid.normals,
        areas: &grid.areas,
        normal_offset: OFFSET,
    }
}

fn at(height: f64) -> ReceptorOptions {
    ReceptorOptions {
        height,
        ..ReceptorOptions::default()
    }
}

/// A layer that must build; an empty default fails the count check.
fn layer(g: &SampleGrid, options: &ReceptorOptions) -> ReceptorLayer {
    let built = build_receptor_layer(&host(g), options).unwrap_or_default();
    assert_eq!(built.count(), g.count());
    built
}

fn xyz(values: &[f64]) -> &[[f64; 3]] {
    values.as_chunks::<3>().0
}

fn close(a: f64, b: f64) -> bool {
    (a - b).abs() < 1e-9
}

#[test]
fn flat_host_lifts_straight_up_and_faces_up() {
    let g = grid(&slab(0.0));
    let l = layer(&g, &at(1.5));
    for ((p, s), n) in xyz(&l.positions)
        .iter()
        .zip(xyz(&g.positions))
        .zip(xyz(&l.normals))
    {
        assert!(close(p[0], s[0]) && close(p[1], s[1]));
        assert!(close(p[2], 1.5), "z {}", p[2]);
        assert_eq!(n, &[0.0, 0.0, 1.0]);
    }
    assert!(l.status.iter().all(|s| s.available()));
}

#[test]
fn zero_height_keeps_the_point_lift_epsilon() {
    let l = layer(&grid(&slab(0.0)), &at(0.0));
    assert!(xyz(&l.positions).iter().all(|p| close(p[2], MIN_LIFT)));
    assert!(
        xyz(&l.hosts).iter().all(|h| close(h[2], 0.0)),
        "host point recovers the surface"
    );
}

#[test]
fn sloped_host_lifts_vertically_not_along_the_normal() {
    let l = layer(&grid(&slab(5.0)), &at(3.0));
    for (p, h) in xyz(&l.positions).iter().zip(xyz(&l.hosts)) {
        assert!(close(p[0], h[0]) && close(p[1], h[1]));
        assert!(close(p[2] - h[2], 3.0));
        assert!(close(h[2], h[0] * 0.5), "on the plane z = x / 2");
    }
}

#[test]
fn plan_area_is_the_projected_footprint() {
    let g = grid(&slab(5.0));
    let surface: f64 = g.areas.iter().sum();
    let plan: f64 = layer(&g, &at(0.5)).areas.iter().sum();
    assert!(close(plan, 100.0), "plan {plan}");
    assert!(surface > plan);
    let kept = ReceptorOptions {
        area: ReceptorArea::Surface,
        ..at(0.5)
    };
    assert!(close(layer(&g, &kept).areas.iter().sum(), surface));
}

#[test]
fn two_heights_share_identity_and_plan_position() {
    let g = grid(&slab(2.0));
    let low = layer(&g, &at(0.15));
    let high = layer(&g, &at(3.0));
    assert_eq!(low.hosts, high.hosts);
    assert_eq!(low.areas, high.areas);
    for (a, b) in xyz(&low.positions).iter().zip(xyz(&high.positions)) {
        assert!(close(a[0], b[0]) && close(a[1], b[1]));
        assert!(close(b[2] - a[2], 2.85));
    }
}

#[test]
fn wall_host_is_steep_not_dark() {
    let g = grid(&wall());
    let l = layer(&g, &at(0.5));
    assert!(l.status.iter().all(|s| *s == ReceptorStatus::SteepHost));
    assert!(close(l.area_split().0, 0.0));
    let surface = ReceptorOptions {
        normal: ReceptorNormal::Surface,
        area: ReceptorArea::Surface,
        ..at(0.5)
    };
    let l = layer(&g, &surface);
    assert!(l.status.iter().all(|s| s.available()));
    assert!(
        xyz(&l.normals).iter().all(|n| close(n[1].abs(), 1.0)),
        "keeps the wall normal"
    );
}

#[test]
fn downward_host_is_steep() {
    // A balcony soffit, wound to face down.
    let mesh = (
        vec![0.0, 0.0, 3.0, 0.0, 2.0, 3.0, 4.0, 2.0, 3.0, 4.0, 0.0, 3.0],
        vec![0, 1, 2, 0, 2, 3],
    );
    let g = grid(&mesh);
    assert!(xyz(&g.normals).iter().all(|n| n[2] < 0.0));
    let l = layer(&g, &at(0.0));
    assert!(l.status.iter().all(|s| *s == ReceptorStatus::SteepHost));
}

#[test]
fn overhead_uses_the_point_lift_arithmetic() {
    // A balcony 2.5 m above the bed: ground and shrub receptors pass, the
    // small-tree receptor does not.
    let ceiling = 2.5 - OVERHEAD_START;
    assert!(!overhead_blocks(ceiling, 0.15));
    assert!(!overhead_blocks(ceiling, 1.5));
    assert!(overhead_blocks(ceiling, 3.0));
    // Clearance: 2.45 m is within 0.10 m of the ceiling.
    assert!(overhead_blocks(ceiling, 2.45));
    assert!(!overhead_blocks(ceiling, 2.35));
    assert!(!overhead_blocks(-1.0, 50.0), "nothing overhead");
    assert!(!overhead_blocks(f64::NAN, 50.0));
    // A ceiling within clearance of the host blocks even a zero layer.
    assert!(overhead_blocks(0.05, 0.0));
}

#[test]
fn mark_overhead_keeps_earlier_reasons_and_counts() {
    let g = grid(&slab(0.0));
    let mut l = layer(&g, &at(3.0));
    let n = l.count();
    if let Some(first) = l.status.first_mut() {
        *first = ReceptorStatus::SteepHost;
    }
    let mut ceilings = vec![-1.0; n];
    ceilings.iter_mut().take(n / 2).for_each(|c| *c = 2.5);
    assert_eq!(mark_overhead(&mut l, &ceilings, 3.0), Ok(n / 2 - 1));
    assert_eq!(l.status.first(), Some(&ReceptorStatus::SteepHost));
    assert_eq!(l.status.last(), Some(&ReceptorStatus::Available));
    let (available, other) = l.area_split();
    assert!(close(available + other, 100.0), "split conserves area");
    let short = ceilings.get(1..).unwrap_or_default();
    assert_eq!(
        mark_overhead(&mut l, short, 3.0),
        Err(ReceptorRefusal::Shape)
    );
}

#[test]
fn bad_input_is_refused_not_partial() {
    let g = grid(&slab(0.0));
    for height in [-0.1, MAX_HEIGHT + 1.0, f64::NAN, f64::INFINITY] {
        assert_eq!(
            build_receptor_layer(&host(&g), &at(height)),
            Err(ReceptorRefusal::Options)
        );
    }
    let tilt = ReceptorOptions {
        min_host_up: 0.0,
        ..at(1.0)
    };
    assert_eq!(
        build_receptor_layer(&host(&g), &tilt),
        Err(ReceptorRefusal::Options)
    );
    let short = HostSamples {
        areas: g.areas.get(1..).unwrap_or_default(),
        ..host(&g)
    };
    assert_eq!(
        build_receptor_layer(&short, &at(1.0)),
        Err(ReceptorRefusal::Shape)
    );
}

#[test]
fn non_finite_host_is_invalid_not_traced() {
    let mut g = grid(&slab(0.0));
    if let Some(x) = g.positions.first_mut() {
        *x = f64::NAN;
    }
    let l = layer(&g, &at(1.0));
    assert_eq!(l.status.first(), Some(&ReceptorStatus::InvalidHost));
    assert!(l.status.iter().skip(1).all(|s| s.available()));
}
