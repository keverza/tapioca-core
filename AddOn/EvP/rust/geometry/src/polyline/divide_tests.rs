use super::*;

fn bits(values: &[f64]) -> Vec<u64> {
    values.iter().map(|value| value.to_bits()).collect()
}

#[test]
fn segment_count_means_plus_or_minus_one_positions() -> Result<(), Error> {
    let line = [0.0, 0.0, 0.0, 0.0, 0.0, 8.0];
    assert_eq!(
        bits(&divide_polyline(&line, 2, true)?),
        bits(&[0.0, 0.0, 0.0, 0.0, 0.0, 4.0, 0.0, 0.0, 8.0])
    );
    assert_eq!(
        bits(&divide_polyline(&line, 2, false)?),
        bits(&[0.0, 0.0, 4.0])
    );
    assert_eq!(divide_polyline(&line, 1, false)?, [] as [f64; 0]);
    assert_eq!(bits(&divide_polyline(&line, 1, true)?), bits(&line));
    for count in [1, 2, 100_000] {
        for include in [false, true] {
            assert_eq!(
                divide_polyline(&line, count, include)?.len(),
                division_point_count(count, include)? * 3
            );
        }
    }
    Ok(())
}

#[test]
fn uneven_lengths_repeated_vertices_and_explicit_closure_reuse_sampling() -> Result<(), Error> {
    for points in [
        vec![0.0, 0.0, 0.0, 3.0, 4.0, 0.0, 3.0, 4.0, 15.0],
        vec![-0.0, 0.0, 0.0, -0.0, 0.0, 0.0, 0.0, 2.0, 0.0, 0.0, 2.0, 0.0],
        vec![2.0; 9],
        vec![
            0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0,
        ],
        vec![0.0, 0.0, 0.0, 1e-200, 0.0, 0.0],
        vec![0.0, 0.0, 0.0, 1e16, 0.0, 0.0, 1e16, 1.0, 0.0],
    ] {
        for include in [false, true] {
            let parameters: Vec<_> = (u32::from(!include)..if include { 9 } else { 8 })
                .map(|index| f64::from(index) / 8.0)
                .collect();
            let expected = sample_polyline(&points, &parameters)?.points;
            assert_eq!(
                bits(&divide_polyline(&points, 8, include)?),
                bits(&expected)
            );
        }
    }
    Ok(())
}

#[test]
fn empty_division_does_not_evaluate_unused_overflowing_lengths() -> Result<(), Error> {
    let points = [-f64::MAX, 0.0, 0.0, f64::MAX, 0.0, 0.0];
    assert_eq!(divide_polyline(&points, 1, false)?, [] as [f64; 0]);
    assert_eq!(
        divide_polyline(&points, 1, true),
        Err(Error::ArithmeticRange)
    );
    assert_eq!(
        divide_polyline(&points, 2, false),
        Err(Error::ArithmeticRange)
    );
    Ok(())
}

#[test]
fn count_shape_nonfinite_and_used_arithmetic_refuse() {
    let line = [0.0; 6];
    for count in [0, 100_001, u32::MAX] {
        assert_eq!(divide_polyline(&line, count, true), Err(Error::DivideCount));
    }
    for len in [0, 3, 5, 7] {
        assert_eq!(
            divide_polyline(&vec![0.0; len], 1, false),
            Err(Error::Shape)
        );
    }
    for bad in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY] {
        let points = [0.0, 0.0, 0.0, bad, 0.0, 0.0];
        for include in [false, true] {
            assert_eq!(
                divide_polyline(&points, 1, include),
                Err(Error::NonFiniteInput)
            );
        }
    }
    assert_eq!(
        divide_polyline(&[0.0, 0.0, 0.0, 1e200, 0.0, 0.0], 2, true),
        Err(Error::ArithmeticRange)
    );
}

#[test]
fn seeded_survey_polylines_are_deterministic_and_do_not_modify_inputs() -> Result<(), Error> {
    let mut state = 0xD171_2026_u32;
    for trial in 0..64_u32 {
        let mut points = Vec::new();
        for _ in 0..12 {
            state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            let x = f64::from(state % 1024) / 8.0;
            points.extend([700_000.0 + x, 6_000_000.0 + x / 2.0, 120.0 + x / 4.0]);
        }
        let before = bits(&points);
        let first = divide_polyline(&points, trial + 1, true)?;
        assert_eq!(
            bits(&first),
            bits(&divide_polyline(&points, trial + 1, true)?)
        );
        assert_eq!(bits(&points), before);
    }
    Ok(())
}
