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
    let previewed = preview([0.0, 0.0], [100.0, 100.0], 0.1, 0.0, &|x, _| Some([x, x]));
    assert!(previewed.points.len() / 3 <= MAX_SURFACE);
    assert!(previewed.surface_step > 0.1);
    assert!(
        previewed
            .points
            .chunks(3)
            .any(|p| p.first().is_some_and(|&x| (x - 100.0).abs() < 1e-9))
    );
}
