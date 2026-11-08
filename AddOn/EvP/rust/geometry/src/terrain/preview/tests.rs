//! #175: the field preview's surface and volumes on fields whose answers are known exactly.

use super::*;

/// A design one metre above the base over a 10 x 4 m box: fill is its area, cut nothing; the
/// surface covers the box with two triangles per cell at the design's height.
#[test]
fn a_raised_box_fills_its_area() {
    let previewed = preview([0.0, 0.0], [10.0, 4.0], 0.5, 0.0, &|_, _| Some([1.0, 0.0]));
    assert!((previewed.fill - 40.0).abs() < 1e-9, "{}", previewed.fill);
    assert!(previewed.cut.abs() < 1e-12);
    assert_eq!(previewed.points.len() / 3, 21 * 9);
    assert_eq!(previewed.triangles.len() / 3, 2 * 20 * 8);
    assert!(previewed.points.chunks(3).all(|p| p.get(2) == Some(&1.0)));
}

/// Where there is no ground there is no surface, and no volume.
#[test]
fn no_ground_no_surface() {
    let previewed = preview([0.0, 0.0], [10.0, 4.0], 0.5, 0.0, &|x, _| {
        (x <= 5.0).then_some([-2.0, 0.0])
    });
    assert!((previewed.cut - 40.0).abs() < 1e-9, "{}", previewed.cut);
    assert_eq!(previewed.triangles.len() / 3, 2 * 10 * 8);
    assert!(
        previewed
            .points
            .chunks(3)
            .all(|p| p.first().is_some_and(|&x| x <= 5.0))
    );
}

/// A fine grid is drawn every few nodes, its last row and column kept.
#[test]
fn a_fine_grid_is_drawn_coarser() {
    let previewed = preview([0.0, 0.0], [100.0, 100.0], 0.1, 0.0, &|x, _| Some([x, 0.0]));
    assert!(previewed.points.len() / 3 <= MAX_SURFACE);
    assert!(previewed.surface_step > 0.1);
    assert!(
        previewed
            .points
            .chunks(3)
            .any(|p| p.first().is_some_and(|&x| (x - 100.0).abs() < 1e-9))
    );
}

/// Only where the design moves the base is drawn, with a cell around it.
#[test]
fn only_the_moved_cells_are_drawn() {
    // Raised over x in [4, 6] of a 10 x 4 box at 0.5 m: the cells touching that strip and one
    // more each way.
    let previewed = preview([0.0, 0.0], [10.0, 4.0], 0.5, 0.0, &|x, _| {
        Some([if (4.0..=6.0).contains(&x) { 1.0 } else { 0.0 }, 0.0])
    });
    let xs: Vec<f64> = previewed
        .points
        .chunks(3)
        .filter_map(|p| p.first().copied())
        .collect();
    let lo = xs.iter().copied().fold(f64::INFINITY, f64::min);
    let hi = xs.iter().copied().fold(f64::NEG_INFINITY, f64::max);
    assert_eq!(
        (lo, hi),
        (3.0, 7.0),
        "the strip's cells [3.5, 6.5] and one more each way"
    );
    assert_eq!(previewed.triangles.len() / 3, 2 * 8 * 8);
    // Nothing moved: nothing drawn.
    assert_eq!(
        preview([0.0, 0.0], [10.0, 4.0], 0.5, 0.0, &|_, _| Some([0.0, 0.0]))
            .triangles
            .len(),
        0
    );
}
