//! Mirrors the pure cases of `tests/cpp/test_sunstudyreading.cpp`.

use super::*;
use crate::patch_sampler::{PatchSamplerOptions, build_patch_sample_grid};
use crate::sampler::{SamplerOptions, build_sample_grid};

/// A 6 m x 2 m wall as 12 welded triangles (one surface), plus a 4 m x 3 m
/// quad of a second element.
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

    fn faces(&self) -> usize {
        self.triangles.len() / 3
    }

    fn patches(&self, spacing: f64, mask: Option<&[u8]>) -> PatchSampleGrid {
        let options = PatchSamplerOptions {
            spacing,
            normal_offset: 0.0,
            sample_group: mask,
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

/// Distinct hours per sample, so "the right sample" is checkable by value.
fn hours(count: usize) -> Vec<f64> {
    (0..count)
        .map(|i| 0.25 * crate::series::count_to_f64(i))
        .collect()
}

fn point_of(positions: &[f64], sample: usize) -> Vec3 {
    let mut point = [0.0; 3];
    if let Some(at) = positions.get(sample * 3..sample * 3 + 3) {
        point.copy_from_slice(at);
    }
    point
}

#[test]
fn every_patch_sample_reads_itself_from_every_triangle_of_its_surface() {
    let scene = Scene::new();
    let grid = scene.patches(0.7, None);
    assert!(grid.valid);
    let hours = hours(grid.count());
    for sample in 0..grid.count() {
        let point = point_of(&grid.positions, sample);
        for face in 0..scene.faces() {
            if grid.patch_of_triangle.get(face) != grid.span_of.get(sample) {
                continue;
            }
            let reading = read_patch_study_at(&grid, 0.7, &hours, face, &point);
            assert!(reading.measured);
            assert_eq!(reading.sample, sample, "through face {face}");
            assert!(!reading.nearest);
            assert_eq!(
                Some(reading.hours.to_bits()),
                hours.get(sample).map(|h| h.to_bits())
            );
        }
    }
}

#[test]
fn every_triangle_sample_reads_itself() {
    let scene = Scene::new();
    let options = SamplerOptions {
        spacing: 0.7,
        normal_offset: 0.0,
        want_layouts: true,
        ..SamplerOptions::default()
    };
    let grid = build_sample_grid(
        &scene.vertices,
        &scene.triangles,
        Some(&scene.groups),
        &options,
    );
    assert!(grid.valid);
    let hours = hours(grid.count());
    for (sample, face) in grid.faces.iter().enumerate() {
        let point = point_of(&grid.positions, sample);
        let face = usize::try_from(*face).unwrap_or(usize::MAX);
        let reading = read_triangle_study_at(&grid, 0.7, &hours, face, &point);
        assert!(reading.measured);
        assert_eq!(reading.sample, sample);
        assert_eq!(
            Some(reading.hours.to_bits()),
            hours.get(sample).map(|h| h.to_bits())
        );
    }
}

#[test]
fn a_context_surface_has_no_measurement() {
    let scene = Scene::new();
    let analysed = [1, 0];
    let grid = scene.patches(1.0, Some(&analysed));
    let reading = read_patch_study_at(&grid, 1.0, &hours(grid.count()), 12, &[21.0, 1.0, 0.0]);
    assert!(!reading.measured);
}

#[test]
fn a_point_off_every_sampled_cell_falls_back_to_the_nearest_sample() {
    let scene = Scene::new();
    let grid = scene.patches(0.7, None);
    let hours = hours(grid.count());
    // Far outside the wall: the clamped cell exists, so nearest only applies
    // when that corner cell carries no sample; either way it is measured.
    let reading = read_patch_study_at(&grid, 0.7, &hours, 0, &[-50.0, -50.0, 0.0]);
    assert!(reading.measured);
    assert_eq!((reading.column, reading.row), (0, 0));
}

#[test]
fn refusals_read_nothing() {
    let scene = Scene::new();
    let grid = scene.patches(0.7, None);
    let hours = hours(grid.count());
    let p = [1.0, 1.0, 0.0];
    for reading in [
        read_patch_study_at(&grid, 0.0, &hours, 0, &p),
        read_patch_study_at(&grid, f64::NAN, &hours, 0, &p),
        read_patch_study_at(&grid, 0.7, &hours, scene.faces(), &p),
        read_patch_study_at(&grid, 0.7, &[], 0, &p),
        read_triangle_study_at(&SampleGrid::default(), 0.7, &hours, 0, &p),
    ] {
        assert_eq!(reading, SunStudyReading::default());
    }
}

#[test]
fn a_nan_cell_saturates_instead_of_the_undefined_cast() {
    assert_eq!(clamp_cell(f64::NAN, 4.0), 0);
    assert_eq!(clamp_cell(-3.0, 4.0), 0);
    assert_eq!(clamp_cell(9.0, 4.0), 4);
    assert_eq!(clamp_cell(2.0, 4.0), 2);
}
