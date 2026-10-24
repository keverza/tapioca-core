//! Mirrors `tests/cpp/test_sunstudyraster.cpp`, including its literals derived
//! from the Python `sunraster.py` oracle.

use super::*;

const EPS: f64 = 1e-9;
const BOX_MIN: Vec3 = [100.0, 200.0, 0.0];
const BOX_MAX: Vec3 = [120.0, 210.0, 30.0];

fn near(actual: f64, expected: f64, tolerance: f64) -> bool {
    (actual - expected).abs() <= tolerance
}

fn sun_xz45() -> Vec3 {
    let s = 0.5_f64.sqrt();
    [s, 0.0, s]
}

fn fit(lo: &Vec3, hi: &Vec3, sun: &Vec3) -> OrthoFrustum {
    let frustum = fit_ortho_frustum(lo, hi, sun, 1.0);
    assert!(frustum.is_some(), "fit refused");
    frustum.unwrap_or_else(|| sized(0.0, 0.0))
}

/// A hand-built frustum with only the extents set, as the C++ test builds one.
fn sized(width: f64, height: f64) -> OrthoFrustum {
    OrthoFrustum {
        left: 0.0,
        right: 0.0,
        bottom: 0.0,
        top: 0.0,
        near_plane: 0.0,
        far_plane: 0.0,
        world_width: width,
        world_height: height,
        world_depth: 0.0,
        right_axis: [1.0, 0.0, 0.0],
        up_axis: [0.0, 1.0, 0.0],
        light_axis: [0.0, 0.0, -1.0],
    }
}

fn corners(lo: &Vec3, hi: &Vec3) -> [Vec3; 8] {
    [
        [lo[0], lo[1], lo[2]],
        [hi[0], lo[1], lo[2]],
        [lo[0], hi[1], lo[2]],
        [lo[0], lo[1], hi[2]],
        [hi[0], hi[1], lo[2]],
        [hi[0], lo[1], hi[2]],
        [lo[0], hi[1], hi[2]],
        [hi[0], hi[1], hi[2]],
    ]
}

#[test]
fn fit_produces_orthonormal_basis_aligned_to_the_sun() {
    let f = fit(&BOX_MIN, &BOX_MAX, &sun_xz45());
    let sun = sun_xz45();
    for (light, toward) in f.light_axis.iter().zip(sun) {
        assert!(near(*light, -toward, EPS));
    }
    for (a, b, expected) in [
        (f.right_axis, f.right_axis, 1.0),
        (f.up_axis, f.up_axis, 1.0),
        (f.light_axis, f.light_axis, 1.0),
        (f.right_axis, f.up_axis, 0.0),
        (f.right_axis, f.light_axis, 0.0),
        (f.up_axis, f.light_axis, 0.0),
    ] {
        assert!(near(dot(&a, &b), expected, 1e-12));
    }
}

#[test]
fn every_corner_lands_inside_the_fitted_frustum() {
    let f = fit(&BOX_MIN, &BOX_MAX, &sun_xz45());
    let vp = light_view_projection(&f, None);
    for corner in corners(&BOX_MIN, &BOX_MAX) {
        let ndc = transform_point(&vp, &corner);
        assert!(ndc.iter().all(|c| (-1.0..=1.0).contains(c)), "{ndc:?}");
    }
}

#[test]
fn near_plane_is_negative_and_must_not_be_clamped() {
    let f = fit(&[-10.0, -10.0, 0.0], &[10.0, 10.0, 20.0], &sun_xz45());
    assert!(f.near_plane < 0.0);
    assert!(f.far_plane > 0.0);
    assert!(f.world_depth > 0.0);
    assert!(near(f.world_depth, f.far_plane - f.near_plane, EPS));
}

#[test]
fn both_depth_planes_may_be_negative_at_survey_coordinates() {
    let f = fit(&BOX_MIN, &BOX_MAX, &sun_xz45());
    assert!(f.near_plane < 0.0 && f.far_plane < 0.0 && f.near_plane < f.far_plane);
    assert!(f.world_depth > 0.0);
    assert!(depth_bias_ndc(0.01, f.near_plane, f.far_plane) > 0.0);
}

#[test]
fn overhead_sun_does_not_collapse_the_basis() {
    let f = fit(&BOX_MIN, &BOX_MAX, &[0.0, 0.0, 1.0]);
    assert!(!f.right_axis[0].is_nan() && !f.up_axis[1].is_nan());
    assert!(f.world_width > 0.0 && f.world_height > 0.0);
}

#[test]
fn degenerate_input_is_refused_not_thrown() {
    assert!(fit_ortho_frustum(&BOX_MIN, &BOX_MAX, &[0.0; 3], 1.0).is_none());
    assert!(fit_ortho_frustum(&BOX_MAX, &BOX_MIN, &sun_xz45(), 1.0).is_none());
}

#[test]
fn rtc_origin_is_the_rounded_centre() {
    assert_eq!(rtc_origin(&BOX_MIN, &BOX_MAX), [110.0, 205.0, 15.0]);
    assert!(near(rtc_origin(&[0.0; 3], &[1.3; 3])[0], 1.0, EPS));
    // Half away from zero, as std::round.
    assert!(near(rtc_origin(&[0.0; 3], &[-5.0; 3])[0], -3.0, EPS));
}

#[test]
fn rtc_shifted_matrix_matches_unshifted_on_unshifted_points() {
    let f = fit(&BOX_MIN, &BOX_MAX, &sun_xz45());
    let origin = rtc_origin(&BOX_MIN, &BOX_MAX);
    let plain = light_view_projection(&f, None);
    let shifted = light_view_projection(&f, Some(&origin));
    for p in [
        [100.0, 200.0, 0.0],
        [110.5, 204.25, 12.5],
        [120.0, 210.0, 30.0],
    ] {
        let a = transform_point(&plain, &p);
        let rtc = [p[0] - origin[0], p[1] - origin[1], p[2] - origin[2]];
        let b = transform_point(&shifted, &rtc);
        assert!(a.iter().zip(b).all(|(x, y)| near(*x, y, 1e-9)));
    }
}

#[test]
fn bias_is_two_texels_with_a_floor() {
    assert!(near(derive_bias(0.01, MIN_BIAS_METRES), 0.02, EPS));
    assert!(near(
        derive_bias(0.0001, MIN_BIAS_METRES),
        MIN_BIAS_METRES,
        EPS
    ));
    assert!(near(
        derive_bias(0.0, MIN_BIAS_METRES),
        MIN_BIAS_METRES,
        EPS
    ));
}

#[test]
fn texel_size_uses_the_larger_frustum_side() {
    let f = sized(100.0, 20.0);
    assert!(near(texel_world_size(&f, 1000), 0.1, EPS));
    assert!(near(texel_world_size(&f, 0), 0.0, EPS));
}

#[test]
fn depth_bias_is_a_fraction_of_the_depth_span_and_is_clamped() {
    assert!(near(depth_bias_ndc(1.0, -50.0, 50.0), 0.01, EPS));
    assert!(near(depth_bias_ndc(2.0, -10.0, 10.0), 0.1, EPS));
    assert!(near(depth_bias_ndc(1.0, 5.0, 5.0), 0.0, EPS));
    assert!(near(depth_bias_ndc(1.0, 10.0, 0.0), 0.0, EPS));
    assert!(near(depth_bias_ndc(100.0, 0.0, 1.0), 0.5, EPS));
}

#[test]
fn pick_bias_takes_the_maximum_and_floors_an_empty_sequence() {
    let (tight, wide) = (sized(10.0, 10.0), sized(1000.0, 1000.0));
    let expected = derive_bias_from_frustum(&wide, 1000, MIN_BIAS_METRES);
    assert!(near(
        pick_bias(&[tight, wide], 1000, MIN_BIAS_METRES),
        expected,
        EPS
    ));
    assert!(near(
        pick_bias(&[], 1000, MIN_BIAS_METRES),
        MIN_BIAS_METRES,
        EPS
    ));
}

#[test]
fn accum_layout_is_roughly_square_and_covers_every_sample() {
    for n in [1_usize, 2, 99, 100, 84_052] {
        let layout = compute_accum_layout(n, MAX_TEXTURE_SIZE);
        assert!(layout.is_some_and(|l| {
            u64::from(l.width) * u64::from(l.height) >= u64::try_from(n).unwrap_or(u64::MAX)
                && l.width <= MAX_TEXTURE_SIZE
                && l.height <= MAX_TEXTURE_SIZE
        }));
    }
    assert_eq!(
        compute_accum_layout(100, MAX_TEXTURE_SIZE),
        Some(AccumLayout {
            width: 10,
            height: 10
        })
    );
    assert_eq!(
        compute_accum_layout(0, MAX_TEXTURE_SIZE),
        Some(AccumLayout {
            width: 1,
            height: 1
        })
    );
    assert_eq!(compute_accum_layout(5, 0), None);
}

#[test]
fn accum_layout_refuses_rather_than_truncating() {
    let side = usize::try_from(MAX_TEXTURE_SIZE).unwrap_or(0);
    assert_eq!(
        compute_accum_layout(side * side + 1, MAX_TEXTURE_SIZE),
        None
    );
    assert!(compute_accum_layout(side * side, MAX_TEXTURE_SIZE).is_some());
}

#[test]
fn sample_index_maps_to_row_major_texels() {
    assert_eq!(sample_index_to_texel(0, 10), (0, 0));
    assert_eq!(sample_index_to_texel(9, 10), (9, 0));
    assert_eq!(sample_index_to_texel(10, 10), (0, 1));
    assert_eq!(sample_index_to_texel(23, 10), (3, 2));
    assert_eq!(sample_index_to_texel(23, 0), (0, 0));
}

#[test]
fn grid_ladder_always_ends_at_the_requested_grid() {
    assert_eq!(grid_ladder(100.0, 1.0, 20000.0, 5000.0), [1.0]);
    assert_eq!(grid_ladder(10000.0, 1.0, 20000.0, 5000.0), [2.0, 1.0]);
    assert_eq!(
        grid_ladder(100_000.0, 1.0, 20000.0, 5000.0),
        [4.0, 2.0, 1.0]
    );
}

#[test]
fn grid_ladder_tolerates_a_non_positive_target() {
    assert_eq!(grid_ladder(1000.0, 0.0, 20000.0, 5000.0), [0.0]);
}

#[test]
fn parity_with_python_sun_raster() {
    let (lo, hi) = ([0.0; 3], [10.0; 3]);
    let f = fit(&lo, &hi, &[0.0, 0.0, 1.0]);
    let expected = [
        (f.right_axis[0], -1.0),
        (f.right_axis[1], 0.0),
        (f.right_axis[2], 0.0),
        (f.up_axis[1], 1.0),
        (f.light_axis[2], -1.0),
        (f.left, -11.0),
        (f.right, 1.0),
        (f.bottom, -1.0),
        (f.top, 11.0),
        (f.near_plane, -11.0),
        (f.far_plane, 1.0),
        (f.world_width, 12.0),
        (f.world_height, 12.0),
        (f.world_depth, 12.0),
        (texel_world_size(&f, 2048), 12.0 / 2048.0),
        (
            derive_bias_from_frustum(&f, 2048, MIN_BIAS_METRES),
            12.0 / 1024.0,
        ),
        (
            depth_bias_ndc(0.005, f.near_plane, f.far_plane),
            0.005 / 12.0,
        ),
        (rtc_origin(&lo, &hi)[0], 5.0),
    ];
    for (index, (actual, wanted)) in expected.iter().enumerate() {
        assert!(near(*actual, *wanted, EPS), "{index}: {actual} != {wanted}");
    }
    let centre = transform_point(&light_view_projection(&f, None), &[5.0; 3]);
    assert!(near(centre[0], 0.0, 1e-12) && near(centre[1], 0.0, 1e-12));
}

#[test]
fn ground_pad_reaches_out_by_the_models_height() {
    assert!(near(
        auto_ground_pad(&[0.0; 3], &[20.0, 20.0, 30.0], 2.0),
        30.0,
        EPS
    ));
    assert!(near(
        auto_ground_pad(&[0.0; 3], &[200.0, 200.0, 2.0], 2.0),
        50.0,
        EPS
    ));
    assert!(near(auto_ground_pad(&[0.0; 3], &[1.0; 3], 2.0), 2.0, EPS));
}

const MAX_SAMPLES: usize = 4_000_000;

fn grid_capped(lo: &Vec3, hi: &Vec3, spacing: f64, pad: f64, max: usize) -> Option<GroundGrid> {
    make_ground_sample_grid(&GroundGridRequest {
        aabb_min: *lo,
        aabb_max: *hi,
        spacing,
        pad,
        z_offset: 0.0,
        max_samples: max,
    })
}

fn grid(lo: &Vec3, hi: &Vec3, spacing: f64, pad: f64, z: f64) -> Option<GroundGrid> {
    make_ground_sample_grid(&GroundGridRequest {
        aabb_min: *lo,
        aabb_max: *hi,
        spacing,
        pad,
        z_offset: z,
        max_samples: MAX_SAMPLES,
    })
}

#[test]
fn ground_grid_covers_the_padded_footprint() {
    let g = grid(&[0.0; 3], &[10.0; 3], 1.0, 5.0, 0.1);
    assert!(g.is_some());
    let Some(g) = g else { return };
    assert_eq!((g.columns, g.rows), (21, 21));
    let positions = g.positions();
    let normals = g.normals();
    assert_eq!(positions.len(), 21 * 21 * 3);
    assert_eq!(normals.len(), positions.len());
    assert!(near(g.ground_z, 0.1, EPS) && near(g.pad, 5.0, EPS));
    for (p, n) in positions
        .as_chunks::<3>()
        .0
        .iter()
        .zip(normals.as_chunks::<3>().0)
    {
        assert_eq!(*n, [0.0, 0.0, 1.0]);
        assert!(
            p.first()
                .is_some_and(|x| (-5.0 - EPS..=15.0 + EPS).contains(x))
        );
        assert!(
            p.get(1)
                .is_some_and(|y| (-5.0 - EPS..=15.0 + EPS).contains(y))
        );
        assert!(p.get(2).is_some_and(|z| near(*z, 0.1, EPS)));
    }
}

#[test]
fn ground_grid_is_centred_when_the_spacing_does_not_divide_evenly() {
    let g = grid(&[0.0; 3], &[10.0, 10.0, 1.0], 3.0, 0.0, 0.0);
    assert_eq!(g.map(|g| g.columns), Some(4));
    let positions = g.map(|g| g.positions()).unwrap_or_default();
    let first = positions.first().copied().unwrap_or(f64::NAN);
    let last = positions.get(9).copied().unwrap_or(f64::NAN);
    assert!(near(first, 10.0 - last, 1e-9));
}

#[test]
fn ground_grid_auto_pads_when_no_pad_is_given() {
    let g = grid(&[0.0; 3], &[20.0, 20.0, 30.0], 5.0, -1.0, 0.1);
    assert!(g.is_some_and(|g| near(g.pad, 30.0, EPS)));
}

#[test]
fn ground_grid_refuses_rather_than_truncating() {
    assert!(grid(&[0.0; 3], &[10.0; 3], 0.0, -1.0, 0.1).is_none());
    assert!(grid(&[0.0; 3], &[10.0; 3], -1.0, -1.0, 0.1).is_none());
    assert!(grid(&[10.0; 3], &[0.0; 3], 1.0, -1.0, 0.1).is_none());
    assert!(grid_capped(&[0.0; 3], &[1000.0, 1000.0, 1.0], 0.001, 0.0, 1000).is_none());
    assert!(grid(&[0.0; 3], &[f64::INFINITY; 3], 1.0, 0.0, 0.0).is_none());
    assert!(grid_capped(&[0.0; 3], &[1.0; 3], 1.0, 0.0, 0).is_none());
}

#[test]
fn a_degenerate_footprint_still_produces_a_sample() {
    let g = grid(&[5.0, 5.0, 0.0], &[5.0, 5.0, 0.0], 1.0, 0.0, 0.0);
    assert!(g.is_some_and(|g| g.columns == 1 && g.rows == 1 && g.positions().len() == 3));
}

#[test]
fn write_positions_stops_at_a_short_buffer() {
    let g = grid(&[0.0; 3], &[1.0; 3], 1.0, 0.0, 0.0);
    let mut short = [9.0; 4];
    if let Some(g) = g {
        g.write_positions(&mut short);
    }
    assert_eq!(short, [0.0, 0.0, 0.0, 9.0]);
}
