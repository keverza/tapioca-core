use super::*;

#[test]
fn sample_output_scalar_and_byte_capacity_overflow_is_a_typed_refusal() {
    assert_eq!(sample_capacity(usize::MAX), Err(Error::ArithmeticRange));
    assert_eq!(
        sample_capacity(isize::MAX.unsigned_abs() / size_of::<f64>()),
        Err(Error::ArithmeticRange)
    );
    assert_eq!(sample_capacity(3), Ok(9));
}

fn close(actual: &[f64], expected: &[f64]) {
    assert_eq!(actual.len(), expected.len());
    for (&a, &b) in actual.iter().zip(expected) {
        assert!(
            (a - b).abs() <= 16.0 * f64::EPSILON * b.abs().max(1.0),
            "{a} != {b}"
        );
    }
}

#[test]
fn uneven_3d_lengths_clamping_and_incoming_join_tangents() -> Result<(), Error> {
    let result = sample_polyline(
        &[0.0, 0.0, 0.0, 3.0, 4.0, 0.0, 3.0, 4.0, 15.0],
        &[-2.0, 0.0, 0.25, 0.5, 1.0, 2.0],
    )?;
    close(&result.cumulative, &[0.0, 5.0, 20.0]);
    close(&[result.total_length], &[20.0]);
    close(
        &result.points,
        &[
            0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 3.0, 4.0, 0.0, 3.0, 4.0, 5.0, 3.0, 4.0, 15.0, 3.0, 4.0,
            15.0,
        ],
    );
    close(
        &result.tangents,
        &[
            0.6, 0.8, 0.0, 0.6, 0.8, 0.0, 0.6, 0.8, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0,
            1.0,
        ],
    );
    Ok(())
}

#[test]
fn repeated_vertices_and_zero_length_curves_keep_legacy_fallbacks() -> Result<(), Error> {
    let points = [2.0, 3.0, 4.0, 2.0, 3.0, 4.0, 2.0, 3.0, 4.0];
    let result = sample_polyline(&points, &[0.0, 0.5, 1.0])?;
    close(&result.points, &points);
    close(&result.cumulative, &[0.0; 3]);
    close(
        &result.tangents,
        &[1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0],
    );
    let result = sample_polyline(
        &[0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 2.0, 0.0, 0.0, 2.0, 0.0],
        &[0.0, 1.0],
    )?;
    close(&result.tangents, &[1.0, 0.0, 0.0, 0.0, 1.0, 0.0]);
    Ok(())
}

#[test]
fn empty_samples_measure_without_implicit_closure_or_input_changes() -> Result<(), Error> {
    let points = [1.0e9, 2.0e8, 5.0, 1.0e9 + 3.0, 2.0e8 + 4.0, 5.0];
    let before = points.map(f64::to_bits);
    let result = sample_polyline(&points, &[])?;
    close(&result.cumulative, &[0.0, 5.0]);
    assert!(result.points.is_empty() && result.tangents.is_empty());
    assert_eq!(points.map(f64::to_bits), before);
    Ok(())
}

#[test]
fn shape_nonfinite_and_derived_overflow_refuse_without_a_partial_result() {
    for points in [&[][..], &[0.0, 0.0, 0.0], &[0.0; 7]] {
        assert_eq!(sample_polyline(points, &[]), Err(Error::Shape));
    }
    for bad in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY] {
        assert_eq!(
            sample_polyline(&[0.0, 0.0, 0.0, bad, 0.0, 0.0], &[]),
            Err(Error::NonFiniteInput)
        );
        assert_eq!(
            sample_polyline(&[0.0; 6], &[0.0, bad]),
            Err(Error::NonFiniteInput)
        );
    }
    for extent in [1.0e200, f64::MAX] {
        assert_eq!(
            sample_polyline(&[-extent, 0.0, 0.0, extent, 0.0, 0.0], &[0.5]),
            Err(Error::ArithmeticRange)
        );
    }
}

#[test]
fn explicit_closed_self_crossing_and_tiny_curves_are_not_repaired() -> Result<(), Error> {
    let closed = [
        0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0,
    ];
    let result = sample_polyline(&closed, &[0.0, 1.0])?;
    close(&[result.total_length], &[2.0 + 2.0 * 2.0_f64.sqrt()]);
    close(&result.points, &[0.0; 6]);
    // Match the existing squared-norm underflow rule, not a new distance epsilon.
    let result = sample_polyline(&[0.0, 0.0, 0.0, 1.0e-200, 0.0, 0.0], &[1.0])?;
    close(&result.points, &[0.0; 3]);
    close(&result.tangents, &[1.0, 0.0, 0.0]);
    Ok(())
}

#[test]
fn seeded_axis_curves_preserve_length_order_and_determinism() -> Result<(), Error> {
    let mut state = 0x1462_a913_u32;
    for _ in 0..64 {
        let mut points = vec![0.0, 0.0, 0.0];
        let mut x = 0.0;
        for _ in 0..32 {
            state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            x += f64::from(state % 20);
            points.extend([x, 0.0, 0.0]);
        }
        let parameters = [0.0, 0.125, 0.5, 0.875, 1.0];
        let a = sample_polyline(&points, &parameters)?;
        let b = sample_polyline(&points, &parameters)?;
        assert_eq!(a, b);
        assert!(
            a.cumulative
                .windows(2)
                .all(|pair| matches!(pair, [a,b] if a <= b))
        );
        for (point, t) in a.points.as_chunks::<3>().0.iter().zip(parameters) {
            close(point, &[x * t, 0.0, 0.0]);
        }
    }
    Ok(())
}
