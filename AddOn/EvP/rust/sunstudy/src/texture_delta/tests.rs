//! Texture delta behaviour (the C++ cases live in
//! `test_sunstudytextureupdates.cpp`).

use super::*;

#[test]
fn nothing_changed_is_no_region() {
    let image = vec![1.0_f32; 16];
    assert_eq!(
        atlas_changed_regions((4, 4, 1), &image, &image, 512),
        Vec::new()
    );
}

#[test]
fn a_rectangle_of_changes_is_one_region() {
    let before = vec![0_u32; 16];
    let mut after = before.clone();
    for y in 1..3 {
        for x in 1..3 {
            if let Some(t) = after.get_mut(y * 4 + x) {
                *t = 9;
            }
        }
    }
    let regions = atlas_changed_regions((4, 4, 1), &before, &after, 512);
    assert_eq!(
        regions,
        [AtlasRegion {
            x: 1,
            y: 1,
            width: 2,
            height: 2,
            layer: 0
        }]
    );
    assert!(atlas_regions_valid(&regions, 4, 4, 1));
}

#[test]
fn runs_merge_only_with_identical_extents_and_layers_are_separate() {
    let before = vec![0.0_f32; 2 * 9];
    let mut after = before.clone();
    for i in [0, 1, 3, 4, 7, 9 + 4] {
        if let Some(t) = after.get_mut(i) {
            *t = 1.0;
        }
    }
    let regions = atlas_changed_regions((3, 3, 2), &before, &after, 512);
    assert_eq!(
        regions,
        [
            AtlasRegion {
                x: 0,
                y: 0,
                width: 2,
                height: 2,
                layer: 0
            },
            AtlasRegion {
                x: 1,
                y: 2,
                width: 1,
                height: 1,
                layer: 0
            },
            AtlasRegion {
                x: 1,
                y: 1,
                width: 1,
                height: 1,
                layer: 1
            },
        ]
    );
}

#[test]
fn byte_comparison_sees_sentinels_and_signed_zero() {
    let before = vec![0.0_f32, -1.0];
    let after = vec![-0.0_f32, -1.0];
    assert_eq!(
        atlas_changed_regions((2, 1, 1), &before, &after, 512).len(),
        1
    );
}

#[test]
fn a_fragmented_layer_collapses_to_one_full_plane() {
    let before = vec![0_u32; 64];
    let after: Vec<u32> = (0..64).map(|i| u32::from(i % 2 == 0)).collect();
    let regions = atlas_changed_regions((8, 8, 1), &before, &after, 3);
    assert_eq!(
        regions,
        [AtlasRegion {
            x: 0,
            y: 0,
            width: 8,
            height: 8,
            layer: 0
        }]
    );
}

#[test]
fn inconsistent_shapes_require_a_full_upload() {
    let image = vec![0_u32; 15];
    assert_eq!(
        atlas_changed_regions((4, 4, 1), &image, &image, 512),
        Vec::new()
    );
    assert_eq!(
        atlas_changed_regions((0, 4, 1), &image, &image, 512),
        Vec::new()
    );
    assert!(!atlas_regions_valid(
        &[AtlasRegion {
            x: 3,
            y: 0,
            width: 2,
            height: 1,
            layer: 0
        }],
        4,
        4,
        1
    ));
}
