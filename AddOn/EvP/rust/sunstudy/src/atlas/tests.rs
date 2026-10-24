//! Mirrors the cases of `tests/cpp/test_sunstudyatlas.cpp`.

use super::*;
use crate::sampler::{SampleGrid, SamplerOptions, build_sample_grid};
use std::collections::BTreeSet;

fn boxes(count: usize) -> SampleGrid {
    // `count` quads of varied size along x, sampled with layouts.
    let mut vertices = Vec::new();
    let mut triangles = Vec::new();
    for i in 0..count {
        let x = 20.0 * crate::series::count_to_f64(i);
        let s = 1.0 + crate::series::count_to_f64(i % 4) * 1.7;
        let base = u32::try_from(vertices.len() / 3).unwrap_or(0);
        vertices.extend_from_slice(&[x, 0.0, 0.0, x + s, 0.0, 0.0, x + s, s, 0.0, x, s, 0.0]);
        triangles.extend_from_slice(&[base, base + 1, base + 2, base, base + 2, base + 3]);
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

fn atlas(grid: &SampleGrid) -> SunStudyAtlas {
    build_sun_study_atlas(&samples(grid), &AtlasOptions::default())
}

#[test]
fn without_layouts_there_is_no_atlas() {
    let grid = SampleGrid::default();
    assert!(!atlas(&grid).valid);
}

#[test]
fn every_gridded_face_is_placed_in_a_power_of_two_square() {
    let grid = boxes(9);
    let a = atlas(&grid);
    assert!(a.valid);
    assert_eq!(
        a.placed_faces,
        grid.layouts.iter().filter(|l| l.gridded).count()
    );
    assert_eq!(a.width, a.height);
    assert!(a.width.is_power_of_two());
}

#[test]
fn tiles_never_overlap_fit_inside_and_keep_a_gutter() {
    let grid = boxes(12);
    let a = atlas(&grid);
    let placed: Vec<_> = a.tiles.iter().filter(|t| t.placed()).collect();
    for (i, p) in placed.iter().enumerate() {
        assert!(p.x >= 1 && p.y >= 1);
        assert!(p.x + p.width < a.width && p.y + p.height < a.height);
        for q in placed.iter().skip(i + 1) {
            let apart = p.x + p.width < q.x
                || q.x + q.width < p.x
                || p.y + p.height < q.y
                || q.y + q.height < p.y;
            assert!(apart, "{p:?} {q:?}");
        }
    }
}

#[test]
fn zero_gutter_is_raised_to_one() {
    let grid = boxes(3);
    let zero = build_sun_study_atlas(
        &samples(&grid),
        &AtlasOptions {
            gutter: 0,
            ..AtlasOptions::default()
        },
    );
    assert_eq!(zero, atlas(&grid));
}

#[test]
fn every_sample_gets_a_distinct_texel_and_packing_is_deterministic() {
    let grid = boxes(7);
    let a = atlas(&grid);
    let distinct: BTreeSet<_> = a.texels.iter().collect();
    assert_eq!(distinct.len(), grid.count());
    assert_eq!(a, atlas(&grid));
}

#[test]
fn a_model_too_large_for_the_ceiling_is_refused_not_truncated() {
    let grid = boxes(8);
    let a = build_sun_study_atlas(
        &samples(&grid),
        &AtlasOptions {
            gutter: 1,
            max_dimension: 8,
        },
    );
    assert!(!a.valid);
    assert!(a.tiles.iter().all(|t| !t.placed()));
}

#[test]
fn scattered_values_land_on_their_texels_and_the_rest_is_the_sentinel() {
    let grid = boxes(4);
    let a = atlas(&grid);
    let values: Vec<f64> = (0..grid.count()).map(crate::series::count_to_f64).collect();
    let image = scatter_to_atlas(&a, &values, -1.0);
    assert_eq!(image.len(), a.texel_count());
    for (texel, value) in a.texels.iter().zip(&values) {
        let at = image
            .get(usize::try_from(*texel).unwrap_or(usize::MAX))
            .copied();
        assert_eq!(at, Some(f64_to_f32(*value)));
    }
    let sentinels = image.iter().filter(|v| **v < 0.0).count();
    assert_eq!(sentinels, a.texel_count() - grid.count());
    assert_eq!(
        scatter_to_atlas(&SunStudyAtlas::default(), &values, -1.0),
        Vec::<f32>::new()
    );
}

#[test]
fn a_uv_lands_inside_its_own_tile_and_clamps_outside_points() {
    let grid = boxes(2);
    let a = atlas(&grid);
    let uv = atlas_uv(&a, &grid.layouts, 0, &[0.3, 0.2, 0.0], 0.5);
    let tile = a.tiles.first().copied().unwrap_or_default();
    let w = f64::from(a.width);
    assert!(uv.is_some_and(|[u, v]| {
        let (u, v) = (f64::from(u) * w, f64::from(v) * w);
        u >= f64::from(tile.x) && u <= f64::from(tile.x + tile.width) && v >= f64::from(tile.y)
    }));
    let far = atlas_uv(&a, &grid.layouts, 0, &[-50.0, -50.0, 0.0], 0.5);
    assert!(far.is_some_and(|[u, v]| {
        (f64::from(u) * w - f64::from(tile.x)).abs() < 1e-3
            && (f64::from(v) * w - f64::from(tile.y)).abs() < 1e-3
    }));
}

#[test]
fn uv_refusals() {
    let grid = boxes(2);
    let a = atlas(&grid);
    assert_eq!(atlas_uv(&a, &grid.layouts, 99, &[0.0; 3], 0.5), None);
    assert_eq!(atlas_uv(&a, &grid.layouts, 0, &[0.0; 3], 0.0), None);
    assert_eq!(
        atlas_uv(&SunStudyAtlas::default(), &grid.layouts, 0, &[0.0; 3], 0.5),
        None
    );
}
