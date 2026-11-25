//! Exact class boundaries and area weighting (#278 Stage 1B).

use super::*;

const PLANTING: [f64; 2] = [3.0, 6.0];

fn close(a: f64, b: f64) -> bool {
    (a - b).abs() < 1e-12
}

#[test]
fn exact_boundaries_belong_to_the_upper_class() {
    assert_eq!(class_of(0.0, &PLANTING), 0);
    assert_eq!(class_of(2.999_999, &PLANTING), 0, "no display slack");
    assert_eq!(class_of(3.0, &PLANTING), 1);
    assert_eq!(class_of(5.999_999, &PLANTING), 1);
    assert_eq!(class_of(6.0, &PLANTING), 2);
    assert_eq!(class_of(16.0, &PLANTING), 2);
    assert_eq!(class_of(0.5, &[1.0, 3.0, 6.0]), 0, "optional deep shade");
}

#[test]
fn class_areas_are_weighted_not_counted() {
    let hours = [1.0, 3.0, 4.5, 6.0, 8.0];
    let areas = [10.0, 1.0, 1.0, 0.5, 0.5];
    let out = class_areas(&hours, &areas, &PLANTING).unwrap_or_default();
    assert_eq!(out.counts, vec![1, 2, 2]);
    assert!(close(out.areas.iter().copied().sum(), out.evaluated_area));
    assert!(close(out.evaluated_area, 13.0));
    let shares: Vec<f64> = out.areas.iter().map(|a| a / out.evaluated_area).collect();
    assert!(close(
        shares.first().copied().unwrap_or_default(),
        10.0 / 13.0
    ));
    assert_eq!(out.unclassified, 0);
}

#[test]
fn non_finite_samples_are_unclassified_not_shade() {
    let hours = [f64::NAN, 2.0, 7.0, 4.0];
    let areas = [1.0, 1.0, f64::INFINITY, -1.0];
    let out = class_areas(&hours, &areas, &PLANTING).unwrap_or_default();
    assert_eq!(out.counts, vec![1, 0, 0]);
    assert_eq!(out.unclassified, 3);
    assert!(close(out.evaluated_area, 1.0));
}

#[test]
fn empty_input_is_valid_and_zero() {
    let out = class_areas(&[], &[], &PLANTING).unwrap_or_default();
    assert_eq!(out.areas, vec![0.0; 3]);
    assert!(close(out.evaluated_area, 0.0));
}

#[test]
fn bad_edges_and_shapes_are_refused() {
    for edges in [
        &[][..],
        &[6.0, 3.0][..],
        &[3.0, 3.0][..],
        &[f64::NAN][..],
        &[1.0; MAX_EDGES + 1][..],
    ] {
        assert_eq!(class_areas(&[1.0], &[1.0], edges), Err(ClassRefusal::Edges));
    }
    assert_eq!(
        class_areas(&[1.0, 2.0], &[1.0], &PLANTING),
        Err(ClassRefusal::Shape)
    );
}
