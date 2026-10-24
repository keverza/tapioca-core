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
