//! Patch sampler behaviour; mirrors the patch case of
//! `tests/cpp/test_sunstudyreceivers.cpp` and the key/refusal contracts.

use super::*;

fn strip() -> (Vec<f64>, Vec<u32>) {
    (
        vec![
            0.0, 0.0, 0.0, 2.0, 0.0, 0.0, 4.0, 0.0, 0.0, 0.0, 2.0, 0.0, 2.0, 2.0, 0.0, 4.0, 2.0,
            0.0,
        ],
        vec![0, 1, 4, 0, 4, 3, 1, 2, 5, 1, 5, 4],
    )
}

fn spaced(spacing: f64) -> PatchSamplerOptions<'static> {
    PatchSamplerOptions {
        spacing,
        ..PatchSamplerOptions::default()
    }
}

#[test]
fn coplanar_connected_glass_and_opaque_faces_remain_separate() {
    let (v, t) = strip();
    let all = build_patch_sample_grid(&v, &t, None, &["pane-frame"], &spaced(0.5));
    assert_eq!(all.spans.len(), 1);
    let mask = [1, 1, 0, 0];
    let options = PatchSamplerOptions {
        sample_face: Some(&mask),
        ..spaced(0.5)
    };
    let glass = build_patch_sample_grid(&v, &t, None, &["pane-frame"], &options);
    assert!(glass.valid);
    assert_eq!(glass.spans.len(), 1);
    assert!(
        glass
            .spans
            .first()
            .is_some_and(|s| (s.area - 4.0).abs() < 1e-12)
    );
    assert_eq!(glass.count(), 16);
    assert_eq!(glass.excluded_patches, 1);
    assert_eq!(glass.patch_of_triangle, [0, 0, NO_PATCH, NO_PATCH]);
    assert!(
        glass
            .positions
            .as_chunks::<3>()
            .0
            .iter()
            .all(|p| p[0] < 2.0)
    );
}

#[test]
fn areas_sum_to_the_surface_and_spans_are_contiguous() {
    let (v, t) = strip();
    let grid = build_patch_sample_grid(&v, &t, None, &["e"], &spaced(0.3));
    assert!((grid.total_area() - 8.0).abs() < 1e-9);
    let mut cursor = 0;
    for (index, span) in grid.spans.iter().enumerate() {
        assert_eq!(span.first, cursor);
        let owned = grid.span_of.get(span.first..span.first + span.count);
        assert!(owned.is_some_and(|o| o.iter().all(|s| usize::try_from(*s) == Ok(index))));
        cursor += span.count;
    }
    assert_eq!(cursor, grid.count());
}

#[test]
fn bounds_reach_the_surface_corners_not_just_the_cells() {
    let (v, t) = strip();
    let grid = build_patch_sample_grid(&v, &t, None, &["e"], &spaced(1.0));
    let span = grid.spans.first().cloned().unwrap_or_default();
    assert_eq!(span.bounds_min, [0.0, 0.0, 0.0]);
    assert_eq!(span.bounds_max, [4.0, 2.0, 0.0]);
}

#[test]
fn the_budget_refuses_rather_than_truncates() {
    let (v, t) = strip();
    let options = PatchSamplerOptions {
        max_samples: 3,
        ..spaced(0.5)
    };
    let grid = build_patch_sample_grid(&v, &t, None, &["e"], &options);
    assert!(grid.sample_limit_exceeded && !grid.valid);
    assert!(grid.areas.is_empty() && grid.patch_of_triangle.is_empty());
}

#[test]
fn a_context_group_gets_no_span() {
    let (v, t) = strip();
    let groups = [0, 0, 0, 0];
    let context = [0];
    let options = PatchSamplerOptions {
        sample_group: Some(&context),
        ..spaced(1.0)
    };
    let grid = build_patch_sample_grid(&v, &t, Some(&groups), &["e"], &options);
    assert!(!grid.valid);
    assert_eq!(grid.excluded_patches, 1);
    assert_eq!(grid.patch_of_triangle, [NO_PATCH; 4]);
}

#[test]
fn keys_follow_the_guid_plane_and_occurrence() {
    let vertices = [
        0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 1.0, 0.0, // a
        0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 1.0, 0.0, // identical copy, unshared indices
    ];
    let triangles = [0, 1, 2, 3, 4, 5];
    let grid = build_patch_sample_grid(&vertices, &triangles, None, &["G"], &spaced(1.0));
    let keys: Vec<_> = grid.spans.iter().map(|s| s.key.clone()).collect();
    assert_eq!(keys.len(), 2);
    assert_eq!(keys.first().map(|k| k.occurrence), Some(0));
    assert_eq!(keys.get(1).map(|k| k.occurrence), Some(1));
    assert_eq!(keys.first().map(|k| k.plane), keys.get(1).map(|k| k.plane));
    assert!(keys.iter().all(|k| k.element == "G"));
}

#[test]
fn a_flipped_normal_is_a_different_key() {
    let vertices = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 1.0, 0.0];
    let up = build_patch_sample_grid(&vertices, &[0, 1, 2], None, &["G"], &spaced(1.0));
    let down = build_patch_sample_grid(&vertices, &[0, 2, 1], None, &["G"], &spaced(1.0));
    assert_ne!(
        up.spans.first().map(|s| s.key.plane),
        down.spans.first().map(|s| s.key.plane)
    );
}

/// A square quad `[x0, x1] x [y0, y1]` at z = 0 as two triangles, appended.
fn quad(v: &mut Vec<f64>, t: &mut Vec<u32>, [x0, y0, x1, y1]: [f64; 4]) {
    let base = u32::try_from(v.len() / 3).unwrap_or(u32::MAX);
    v.extend_from_slice(&[x0, y0, 0.0, x1, y0, 0.0, x1, y1, 0.0, x0, y1, 0.0]);
    t.extend_from_slice(&[base, base + 1, base + 2, base, base + 2, base + 3]);
}

/// Mirrors `SunStudyPatchAtlas.AnalysisCylinderClipsCellsNotPatches` (#152):
/// a 20 m slab under a 5 m cylinder keeps its patch but only the cells
/// inside the circle; a quad wholly outside keeps no span at all.
#[test]
fn the_analysis_cylinder_clips_cells_not_patches() {
    let (mut v, mut t) = (Vec::new(), Vec::new());
    quad(&mut v, &mut t, [0.0, 0.0, 20.0, 20.0]);
    quad(&mut v, &mut t, [40.0, 40.0, 44.0, 44.0]);
    let groups = [0, 0, 1, 1];
    let options = PatchSamplerOptions {
        normal_offset: 0.0,
        clip: [10.0, 10.0, 5.0],
        ..spaced(1.0)
    };
    let grid = build_patch_sample_grid(&v, &t, Some(&groups), &["slab", "far"], &options);
    assert!(grid.valid);
    assert_eq!(grid.spans.len(), 1);
    assert_eq!(
        grid.spans.first().map(|s| s.key.element.as_str()),
        Some("slab")
    );
    let cells = grid.count();
    assert!(cells > 70 && cells < 90, "pi * 25 = 78.5, got {cells}");
    for p in grid.positions.as_chunks::<3>().0 {
        assert!((p[0] - 10.0).hypot(p[1] - 10.0) <= 5.0);
    }
    assert_eq!(
        grid.patch_of_triangle.get(2),
        Some(&NO_PATCH),
        "the far quad kept a patch"
    );
    let unclipped = build_patch_sample_grid(&v, &t, Some(&groups), &["slab", "far"], &spaced(1.0));
    assert_eq!(unclipped.spans.len(), 2);
    assert!(unclipped.count() > 400);
}

/// #117: skipping cells the cylinder is certain to drop changes no kept cell,
/// on a flat strip and on a tilted roof, for circles inside, across and away.
#[test]
fn skipping_far_cells_keeps_exactly_the_clipped_lattice() {
    let (strip_v, strip_t) = strip();
    let roof_v = vec![0.0, 0.0, 0.0, 6.0, 0.0, 0.0, 6.0, 3.0, 2.0, 0.0, 3.0, 2.0];
    let roof_t = vec![0, 1, 2, 0, 2, 3];
    let clips = [
        [2.0, 1.0, 0.7],
        [0.0, 0.0, 1.3],
        [4.2, 2.2, 2.0],
        [3.0, 1.5, 50.0],
        [90.0, 90.0, 1.0],
    ];
    for (vertices, triangles) in [(&strip_v, &strip_t), (&roof_v, &roof_t)] {
        let patches = build_surface_patches(
            vertices,
            triangles,
            None,
            &SurfacePatchOptions::default(),
            None,
        );
        for patch in &patches {
            for spacing in [0.13, 0.5, 1.1] {
                for clip in clips {
                    let mut whole =
                        crate::surface_patch::build_patch_grid(patch, vertices, triangles, spacing);
                    clip_cells(&mut whole, clip);
                    let mut fast =
                        build_patch_grid_within(patch, vertices, triangles, spacing, clip);
                    clip_cells(&mut fast, clip);
                    assert_eq!(fast.cells, whole.cells, "spacing {spacing} clip {clip:?}");
                    assert_eq!((fast.columns, fast.rows), (whole.columns, whole.rows));
                }
            }
        }
    }
}
