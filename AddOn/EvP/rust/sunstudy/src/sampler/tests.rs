//! Mirrors `tests/cpp/test_sunstudysampler.cpp`.

use super::*;
use std::collections::BTreeMap;

struct Quad {
    vertices: Vec<f64>,
    triangles: Vec<u32>,
}

fn quad(s: f64, vertical: bool) -> Quad {
    let vertices = if vertical {
        vec![0.0, 0.0, 0.0, s, 0.0, 0.0, s, 0.0, s, 0.0, 0.0, s]
    } else {
        vec![0.0, 0.0, 0.0, s, 0.0, 0.0, s, s, 0.0, 0.0, s, 0.0]
    };
    Quad {
        vertices,
        triangles: vec![0, 1, 2, 0, 2, 3],
    }
}

fn spaced(spacing: f64) -> SamplerOptions<'static> {
    SamplerOptions {
        spacing,
        ..SamplerOptions::default()
    }
}

fn build(q: &Quad, options: &SamplerOptions<'_>, groups: Option<&[u32]>) -> SampleGrid {
    build_sample_grid(&q.vertices, &q.triangles, groups, options)
}

fn total(areas: &[f64]) -> f64 {
    areas.iter().sum()
}

#[test]
fn empty_input_is_not_valid() {
    assert!(!build_sample_grid(&[], &[], None, &SamplerOptions::default()).valid);
}

#[test]
fn non_positive_and_nan_spacing_is_refused() {
    for spacing in [0.0, -1.0, f64::NAN] {
        assert!(!build(&quad(10.0, false), &spaced(spacing), None).valid);
    }
}

#[test]
fn parallel_arrays_stay_in_step() {
    let grid = build(&quad(10.0, false), &spaced(1.0), None);
    assert!(grid.valid && grid.count() > 0);
    assert_eq!(grid.positions.len(), grid.count() * 3);
    assert_eq!(grid.normals.len(), grid.count() * 3);
    assert_eq!(grid.areas.len(), grid.count());
    assert_eq!(grid.groups.len(), grid.count());
}

#[test]
fn sample_areas_sum_to_the_mesh_area() {
    let grid = build(&quad(10.0, false), &spaced(0.7), None);
    assert!((total(&grid.areas) - 100.0).abs() < 1e-9);
}

#[test]
fn sample_count_tracks_the_requested_spacing() {
    let coarse = build(&quad(10.0, false), &spaced(2.0), None).count();
    let fine = build(&quad(10.0, false), &spaced(1.0), None).count();
    assert!((count_to_f64(fine) / count_to_f64(coarse) - 4.0).abs() < 0.6);
}

#[test]
fn vertical_faces_are_sampled_as_densely_as_horizontal_ones() {
    let flat = build(&quad(10.0, false), &spaced(1.0), None).count();
    let wall = build(&quad(10.0, true), &spaced(1.0), None).count();
    assert_eq!(flat, wall);
    assert!(wall > 50);
}

#[test]
fn faces_below_the_grid_still_emit_one_sample() {
    let grid = build(&quad(0.05, false), &spaced(1.0), None);
    assert!(grid.valid);
    assert_eq!((grid.count(), grid.undersized_faces), (2, 2));
    assert!((total(&grid.areas) - 0.05 * 0.05).abs() < 1e-12);
}

#[test]
fn every_face_is_represented() {
    let grid = build(&quad(10.0, false), &spaced(1.0), None);
    let mut per_face: BTreeMap<u32, usize> = BTreeMap::new();
    for face in &grid.faces {
        *per_face.entry(*face).or_default() += 1;
    }
    assert_eq!(per_face.len(), 2);
}

#[test]
fn degenerate_faces_are_counted_and_skipped() {
    let grid = build_sample_grid(
        &[0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 2.0, 0.0, 0.0],
        &[0, 1, 2],
        None,
        &spaced(0.5),
    );
    assert!(grid.valid);
    assert_eq!((grid.count(), grid.degenerate_faces), (0, 1));
}

#[test]
fn out_of_range_indices_are_rejected_not_dereferenced() {
    let grid = build_sample_grid(
        &[0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0],
        &[0, 1, 99],
        None,
        &spaced(0.5),
    );
    assert_eq!((grid.count(), grid.degenerate_faces), (0, 1));
}

#[test]
fn normals_are_unit_and_face_the_source_winding() {
    let grid = build(&quad(4.0, false), &spaced(1.0), None);
    assert!(grid.count() > 0);
    for n in grid.normals.as_chunks::<3>().0 {
        assert!(((n[0] * n[0] + n[1] * n[1] + n[2] * n[2]).sqrt() - 1.0).abs() < 1e-12);
        assert!((n[2] - 1.0).abs() < 1e-12);
    }
}

#[test]
fn samples_are_lifted_along_their_normal() {
    let options = SamplerOptions {
        normal_offset: 0.25,
        ..spaced(1.0)
    };
    let grid = build(&quad(4.0, false), &options, None);
    assert!(grid.count() > 0);
    assert!(
        grid.positions
            .as_chunks::<3>()
            .0
            .iter()
            .all(|p| (p[2] - 0.25).abs() < 1e-12)
    );
}

#[test]
fn group_ids_are_passed_through_per_face() {
    let groups = [7, 42];
    let grid = build(&quad(4.0, false), &spaced(1.0), Some(&groups));
    for (group, face) in grid.groups.iter().zip(&grid.faces) {
        assert_eq!(Some(group), groups.get(usize::try_from(*face).unwrap_or(9)));
    }
}

#[test]
fn a_context_group_is_skipped_and_the_face_indexing_is_kept() {
    let groups = [0, 1];
    let analysed = [0, 1];
    let options = SamplerOptions {
        want_layouts: true,
        sample_group: Some(&analysed),
        ..spaced(1.0)
    };
    let grid = build(&quad(4.0, false), &options, Some(&groups));
    assert!(grid.count() > 0);
    assert_eq!(grid.excluded_faces, 1);
    assert!(grid.faces.iter().all(|f| *f == 1));
    assert_eq!(grid.layouts.len(), 2);
    assert!(!grid.layouts.first().is_some_and(|l| l.gridded));
    assert!(grid.layouts.get(1).is_some_and(|l| l.gridded));
    let everything = build(
        &quad(4.0, false),
        &SamplerOptions {
            want_layouts: true,
            ..spaced(1.0)
        },
        Some(&groups),
    );
    assert!(everything.count() > grid.count());
    assert_eq!(everything.excluded_faces, 0);
}

#[test]
fn a_face_filter_skips_faces_and_a_short_one_samples_the_rest() {
    let filter = [0];
    let options = SamplerOptions {
        sample_face: Some(&filter),
        ..spaced(1.0)
    };
    let grid = build(&quad(4.0, false), &options, None);
    assert_eq!(grid.excluded_faces, 1);
    assert!(grid.faces.iter().all(|f| *f == 1));
}

#[test]
fn a_short_group_list_is_refused() {
    assert!(!build(&quad(4.0, false), &spaced(1.0), Some(&[0])).valid);
}

#[test]
fn jitter_is_deterministic_and_moves_the_samples() {
    let jittered = SamplerOptions {
        jitter: 0.8,
        ..spaced(1.0)
    };
    let first = build(&quad(10.0, false), &jittered, None);
    let second = build(&quad(10.0, false), &jittered, None);
    assert_eq!(first.positions, second.positions);
    assert_ne!(
        build(&quad(10.0, false), &spaced(1.0), None).positions,
        first.positions
    );
}

#[test]
fn jittered_samples_stay_inside_their_face() {
    let options = SamplerOptions {
        jitter: 1.0,
        normal_offset: 0.0,
        ..spaced(1.0)
    };
    let grid = build(&quad(10.0, false), &options, None);
    assert!(grid.count() > 0);
    for p in grid.positions.as_chunks::<3>().0 {
        assert!((-1e-9..=10.0 + 1e-9).contains(&p[0]));
        assert!((-1e-9..=10.0 + 1e-9).contains(&p[1]));
    }
}

#[test]
fn the_budget_refuses_rather_than_truncates() {
    let options = SamplerOptions {
        max_samples: 100,
        ..spaced(0.5)
    };
    assert!(!build(&quad(100.0, false), &options, None).valid);
}

#[test]
fn triangle_normal_reports_area_and_a_degenerate_face_is_zero_not_nan() {
    let (n, area) = triangle_normal(&[0.0; 3], &[3.0, 0.0, 0.0], &[0.0, 4.0, 0.0]);
    assert!((area - 6.0).abs() < 1e-12 && (n[2] - 1.0).abs() < 1e-12);
    let (n, area) = triangle_normal(&[0.0; 3], &[1.0, 0.0, 0.0], &[2.0, 0.0, 0.0]);
    assert!(area.abs() < f64::EPSILON);
    assert_eq!(n, [0.0; 3]);
}

#[test]
fn cell_jitter_stays_within_one_cell() {
    for f in 0..8 {
        for u in 0..8 {
            let out = cell_jitter(f, u, u * 3 + 1);
            assert!(out.iter().all(|o| (-0.5..=0.5).contains(o)));
        }
    }
}

#[test]
fn opposite_faces_of_a_slab_keep_separate_samples() {
    let vertices = [
        0.0, 0.0, 0.0, 4.0, 0.0, 0.0, 4.0, 4.0, 0.0, 0.0, 4.0, 0.0, // up
        0.0, 0.0, 0.0, 0.0, 4.0, 0.0, 4.0, 4.0, 0.0, 4.0, 0.0, 0.0, // down
    ];
    let triangles = [0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7];
    let grid = build_sample_grid(&vertices, &triangles, None, &spaced(1.0));
    assert!(grid.valid);
    let up = grid
        .normals
        .as_chunks::<3>()
        .0
        .iter()
        .filter(|n| n[2] > 0.0)
        .count();
    assert!(up > 0);
    assert_eq!(up * 2, grid.count());
}
