//! Mirrors the `PatchAtlas` cases of `tests/cpp/test_sunstudypatchatlas.cpp`
//! and the stable triangle atlas contract.

use super::*;
use crate::sampler::{SampleGrid, SamplerOptions, build_sample_grid};

fn shape(element: &str, plane: u64, columns: u32, rows: u32) -> PatchShape {
    PatchShape {
        key: PatchKey {
            element: element.to_owned(),
            plane,
            occurrence: 0,
        },
        columns,
        rows,
    }
}

fn site() -> Vec<PatchShape> {
    (0..10)
        .map(|i| {
            shape(
                if i < 5 { "A" } else { "B" },
                i,
                3 + u32::try_from(i % 3).unwrap_or(0),
                2 + u32::try_from(i % 4).unwrap_or(0),
            )
        })
        .collect()
}

#[test]
fn a_first_fit_sizes_the_atlas_and_places_everything() {
    let mut atlas = PatchAtlas::default();
    let update = atlas.fit(&site(), 1, 8192);
    assert!(update.resized);
    assert_eq!(update.added, 10);
    assert!(atlas.allocations().all(PatchAtlasAllocation::placed));
    assert!(atlas.width().is_power_of_two());
}

#[test]
fn refitting_an_unchanged_grid_moves_nothing() {
    let mut atlas = PatchAtlas::default();
    let _ = atlas.fit(&site(), 1, 8192);
    let before = atlas.clone();
    let update = atlas.fit(&site(), 1, 8192);
    assert_eq!(update.retained, 10);
    assert_eq!(
        (update.moved, update.added, update.removed, update.resized),
        (0, 0, 0, false)
    );
    assert_eq!(atlas, before);
}

#[test]
fn a_changed_element_leaves_every_other_tile_where_it_was() {
    let mut atlas = PatchAtlas::default();
    let _ = atlas.fit(&site(), 1, 8192);
    let before = atlas.clone();
    let mut changed = site();
    if let Some(first) = changed.first_mut() {
        first.columns += 1;
    }
    let update = atlas.fit(&changed, 1, 8192);
    assert_eq!((update.moved, update.retained), (1, 9));
    for shape in changed.iter().skip(1) {
        assert_eq!(atlas.find(&shape.key), before.find(&shape.key));
    }
    let moved = changed
        .first()
        .and_then(|s| atlas.find(&s.key))
        .cloned()
        .unwrap_or_default();
    let old = changed
        .first()
        .and_then(|s| before.find(&s.key))
        .cloned()
        .unwrap_or_default();
    assert_ne!(moved.generation, old.generation);
}

#[test]
fn removed_keys_are_retired_without_reclaiming_space() {
    let mut atlas = PatchAtlas::default();
    let _ = atlas.fit(&site(), 1, 8192);
    let cursor = atlas.cursor();
    let fewer: Vec<_> = site().into_iter().skip(3).collect();
    let update = atlas.fit(&fewer, 1, 8192);
    assert_eq!(update.removed, 3);
    assert_eq!(atlas.cursor(), cursor);
}

#[test]
fn running_out_of_room_grows_and_reports_resized() {
    let mut atlas = PatchAtlas::default();
    let _ = atlas.fit(&site(), 1, 8192);
    let width = atlas.width();
    let mut more = site();
    more.extend((20..40).map(|i| shape("C", i, 6, 6)));
    let update = atlas.fit(&more, 1, 8192);
    assert!(update.resized);
    assert!(atlas.width() > width);
    assert!(atlas.allocations().all(PatchAtlasAllocation::placed));
}

#[test]
fn a_refusal_clears_and_the_generation_keeps_counting() {
    let mut atlas = PatchAtlas::default();
    let next = atlas.cursor().next_generation;
    let _ = atlas.fit(&[shape("A", 0, 100, 100)], 1, 16);
    assert_eq!(atlas.width(), 0);
    assert_eq!(atlas.allocations().count(), 0);
    assert!(atlas.cursor().next_generation > next);
}

#[test]
fn state_round_trips() {
    let mut atlas = PatchAtlas::default();
    let _ = atlas.fit(&site(), 1, 8192);
    let copy = PatchAtlas::from_state(atlas.cursor(), atlas.allocations().cloned().collect());
    assert_eq!(copy, atlas);
}

fn quads(count: usize) -> SampleGrid {
    let mut vertices = Vec::new();
    let mut triangles = Vec::new();
    for i in 0..count {
        let x = 10.0 * crate::series::count_to_f64(i);
        let base = u32::try_from(vertices.len() / 3).unwrap_or(0);
        vertices.extend_from_slice(&[x, 0.0, 0.0, x + 2.0, 0.0, 0.0, x + 2.0, 2.0, 0.0]);
        triangles.extend_from_slice(&[base, base + 1, base + 2]);
    }
    let options = SamplerOptions {
        spacing: 0.5,
        want_layouts: true,
        ..SamplerOptions::default()
    };
    build_sample_grid(&vertices, &triangles, None, &options)
}

fn samples(grid: &SampleGrid) -> AtlasSamples<'_> {
    AtlasSamples {
        layouts: &grid.layouts,
        faces: &grid.faces,
        cell_columns: &grid.cell_columns,
        cell_rows: &grid.cell_rows,
    }
}

#[test]
fn stable_triangle_tiles_survive_a_refit_and_ambiguity_falls_back() {
    let grid = quads(4);
    let meshes = [
        AtlasMesh {
            guid: "{a}",
            triangle_count: 2,
        },
        AtlasMesh {
            guid: "B",
            triangle_count: 2,
        },
    ];
    let mut allocations = PatchAtlas::default();
    let first = build_stable_triangle_atlas(
        &samples(&grid),
        &meshes,
        &mut allocations,
        None,
        &AtlasOptions::default(),
    );
    assert!(first.valid);
    assert_eq!(first.placed_faces, 4);
    let previous = allocations.clone();
    let mut again = PatchAtlas::default();
    let second = build_stable_triangle_atlas(
        &samples(&grid),
        &meshes,
        &mut again,
        Some(&previous),
        &AtlasOptions::default(),
    );
    assert_eq!(second.tiles, first.tiles);
    let duplicate = [
        AtlasMesh {
            guid: "A",
            triangle_count: 2,
        },
        AtlasMesh {
            guid: "{a}",
            triangle_count: 2,
        },
    ];
    let fallback = build_stable_triangle_atlas(
        &samples(&grid),
        &duplicate,
        &mut again,
        None,
        &AtlasOptions::default(),
    );
    assert_eq!(
        fallback,
        build_sun_study_atlas(&samples(&grid), &AtlasOptions::default())
    );
    assert_eq!(again.width(), 0);
}
