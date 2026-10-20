use super::*;

const IDENTITY: Transform = Transform {
    m: [
        [1.0, 0.0, 0.0, 0.0],
        [0.0, 1.0, 0.0, 0.0],
        [0.0, 0.0, 1.0, 0.0],
    ],
};

#[test]
fn composition_empty_identity_and_application_order() {
    assert_eq!(compose_transforms(&[]), Ok(Transform::IDENTITY));
    assert_eq!(compose_transforms(&[IDENTITY]), Ok(IDENTITY));
    let first = compose_transforms(&[NONUNIFORM_SCALE, TRANSLATION]);
    let second = compose_transforms(&[TRANSLATION, NONUNIFORM_SCALE]);
    assert_eq!(
        first,
        Ok(Transform {
            m: [
                [4.0, 0.0, 0.0, 10.0],
                [0.0, 1.0, 0.0, -20.0],
                [0.0, 0.0, 1.0, 0.5],
            ]
        })
    );
    assert_eq!(
        second,
        Ok(Transform {
            m: [
                [4.0, 0.0, 0.0, 40.0],
                [0.0, 1.0, 0.0, -20.0],
                [0.0, 0.0, 1.0, 0.5],
            ]
        })
    );
}

#[test]
fn composition_preserves_singular_and_reflection_semantics() -> Result<(), Error> {
    let singular = compose_transforms(&[SINGULAR, TRANSLATION])?;
    assert_eq!(
        apply_to_normal(
            &singular,
            Vec3 {
                x: 1.0,
                y: 2.0,
                z: 3.0
            }
        )?,
        Vec3 {
            x: 1.0,
            y: 2.0,
            z: 3.0
        }
    );
    let reflection = compose_transforms(&[MIRROR_X, TRANSLATION])?;
    assert!(apply_vectors(&reflection, ApplyMode::Point, &[])?.flips_orientation);
    assert_eq!(compose_transforms(&[MIRROR_X, MIRROR_X])?, IDENTITY);
    Ok(())
}

#[test]
fn composition_refuses_input_and_intermediate_overflow() {
    for input in [
        NAN_COEFFICIENT,
        INFINITE_COEFFICIENT,
        OVERFLOWING_DETERMINANT,
    ] {
        assert_eq!(
            compose_transforms(&[IDENTITY, input]),
            Err(Error::NonFiniteTransform)
        );
    }
    assert_eq!(
        compose_transforms(&[OVERFLOW_TRANSLATION, OVERFLOW_TRANSLATION]),
        Err(Error::NonFiniteOutput)
    );
    let large = Transform {
        m: [
            [1e100, 0.0, 0.0, 0.0],
            [0.0, 1e100, 0.0, 0.0],
            [0.0, 0.0, 1e100, 0.0],
        ],
    };
    assert_eq!(
        compose_transforms(&[large, large]),
        Err(Error::NonFiniteOutput)
    );
}

#[test]
fn composition_matches_sequential_application_and_is_deterministic() -> Result<(), Error> {
    let chain = [
        SHEAR_XY,
        NONUNIFORM_SCALE,
        CPP_ROTATION_Z_90,
        MIRROR_X,
        TRANSLATION,
    ];
    let before = chain;
    let composed = compose_transforms(&chain)?;
    let point = Vec3 {
        x: 2.0,
        y: -3.0,
        z: 7.0,
    };
    let mut expected = point;
    for transform in &chain {
        expected = apply_to_point(transform, expected)?;
    }
    let actual = apply_to_point(&composed, point)?;
    for (a, b) in [actual.x, actual.y, actual.z]
        .into_iter()
        .zip([expected.x, expected.y, expected.z])
    {
        assert!((a - b).abs() <= 32.0 * f64::EPSILON * b.abs().max(1.0));
    }
    assert_eq!(composed, compose_transforms(&chain)?);
    assert_eq!(chain, before);
    Ok(())
}
const TRANSLATION: Transform = Transform {
    m: [
        [1.0, 0.0, 0.0, 10.0],
        [0.0, 1.0, 0.0, -20.0],
        [0.0, 0.0, 1.0, 0.5],
    ],
};

/// `Rotation({0,0,0}, {0,0,1}, pi/2)` coefficients as produced by the C++
/// builder: `cos(pi/2)` rounds to `6.123233995736766e-17`, not zero.
const CPP_COS_90: f64 = 6.123_233_995_736_766e-17;
const CPP_ROTATION_Z_90: Transform = Transform {
    m: [
        [CPP_COS_90, -1.0, 0.0, 0.0],
        [1.0, CPP_COS_90, 0.0, 0.0],
        [0.0, 0.0, 1.0, 0.0],
    ],
};
const NONUNIFORM_SCALE: Transform = Transform {
    m: [
        [4.0, 0.0, 0.0, 0.0],
        [0.0, 1.0, 0.0, 0.0],
        [0.0, 0.0, 1.0, 0.0],
    ],
};
const MIRROR_X: Transform = Transform {
    m: [
        [-1.0, 0.0, 0.0, 0.0],
        [0.0, 1.0, 0.0, 0.0],
        [0.0, 0.0, 1.0, 0.0],
    ],
};
const SHEAR_XY: Transform = Transform {
    m: [
        [1.0, 2.0, 0.0, 0.0],
        [0.0, 1.0, 0.0, 0.0],
        [0.0, 0.0, 1.0, 0.0],
    ],
};
/// `Compose(Translation({10,20,30}), Scaling({0,0,0}, {2,3,4}))`, the C++
/// `second`-then-`first` argument order applied to a literal.
const CPP_COMPOSED: Transform = Transform {
    m: [
        [2.0, 0.0, 0.0, 10.0],
        [0.0, 3.0, 0.0, 20.0],
        [0.0, 0.0, 4.0, 30.0],
    ],
};

const SINGULAR: Transform = Transform {
    m: [
        [1.0, 2.0, 0.0, 0.0],
        [2.0, 4.0, 0.0, 0.0],
        [0.0, 0.0, 1.0, 0.0],
    ],
};
const SINGULAR_NEGATIVE_ZERO: Transform = Transform {
    m: [
        [-0.0, 0.0, 0.0, 0.0],
        [0.0, 1.0, 0.0, 0.0],
        [0.0, 0.0, 1.0, 0.0],
    ],
};

const NAN_COEFFICIENT: Transform = Transform {
    m: [
        [f64::NAN, 0.0, 0.0, 0.0],
        [0.0, 1.0, 0.0, 0.0],
        [0.0, 0.0, 1.0, 0.0],
    ],
};
const INFINITE_COEFFICIENT: Transform = Transform {
    m: [
        [1.0, 0.0, 0.0, f64::INFINITY],
        [0.0, 1.0, 0.0, 0.0],
        [0.0, 0.0, 1.0, 0.0],
    ],
};
const OVERFLOWING_DETERMINANT: Transform = Transform {
    m: [
        [1e200, 0.0, 0.0, 0.0],
        [0.0, 1e200, 0.0, 0.0],
        [0.0, 0.0, 1e200, 0.0],
    ],
};
const OVERFLOW_TRANSLATION: Transform = Transform {
    m: [
        [1.0, 0.0, 0.0, 1e308],
        [0.0, 1.0, 0.0, 0.0],
        [0.0, 0.0, 1.0, 0.0],
    ],
};
const TINY_SCALE: Transform = Transform {
    m: [
        [1e-100, 0.0, 0.0, 0.0],
        [0.0, 1e-100, 0.0, 0.0],
        [0.0, 0.0, 1e-100, 0.0],
    ],
};
const GEOREFERRED: Transform = Transform {
    m: [
        [1.0, 0.0, 0.0, 4_500_000.0],
        [0.0, 1.0, 0.0, 5_300_000.0],
        [0.0, 0.0, 1.0, 250.0],
    ],
};

fn vec3(x: f64, y: f64, z: f64) -> Vec3 {
    Vec3 { x, y, z }
}

fn nan_vec3() -> Vec3 {
    vec3(f64::NAN, f64::NAN, f64::NAN)
}

fn or_nan(result: Result<Vec3, Error>) -> Vec3 {
    result.unwrap_or(nan_vec3())
}

fn vec_bits(value: Vec3) -> [u64; 3] {
    [value.x.to_bits(), value.y.to_bits(), value.z.to_bits()]
}

fn bits_of(values: &[f64]) -> Vec<u64> {
    values.iter().map(|value| value.to_bits()).collect()
}

fn dot(left: Vec3, right: Vec3) -> f64 {
    left.x * right.x + left.y * right.y + left.z * right.z
}

fn summary(result: Result<AppliedVectors, Error>) -> Result<(usize, bool), Error> {
    result.map(|applied| (applied.values.len(), applied.flips_orientation))
}

fn apply_bits(
    transform: &Transform,
    mode: ApplyMode,
    coordinates: &[f64],
) -> Result<(Vec<u64>, bool), Error> {
    apply_vectors(transform, mode, coordinates).map(|applied| {
        (
            applied.values.iter().map(|value| value.to_bits()).collect(),
            applied.flips_orientation,
        )
    })
}

fn assert_point(transform: &Transform, input: Vec3, expected: Vec3) {
    assert_eq!(
        apply_to_point(transform, input).map(vec_bits),
        Ok(vec_bits(expected)),
        "point {input:?} should become {expected:?}"
    );
}

fn assert_direction(transform: &Transform, input: Vec3, expected: Vec3) {
    assert_eq!(
        apply_to_direction(transform, input).map(vec_bits),
        Ok(vec_bits(expected)),
        "direction {input:?} should become {expected:?}"
    );
}

fn assert_normal(transform: &Transform, input: Vec3, expected: Vec3) {
    assert_eq!(
        apply_to_normal(transform, input).map(vec_bits),
        Ok(vec_bits(expected)),
        "normal {input:?} should become {expected:?}"
    );
}

fn seeded_values(seed: u64, count: usize) -> Vec<f64> {
    let mut state = seed;
    (0..count * 3)
        .map(|_| {
            state = state
                .wrapping_mul(6_364_136_223_846_793_005)
                .wrapping_add(1_442_695_040_888_963_407);
            let fraction = f64::from_bits(0x3FF0_0000_0000_0000 | (state >> 12)) - 1.5;
            fraction * 2_000.0
        })
        .collect()
}

fn applied_values(transform: &Transform, mode: ApplyMode, coordinates: &[f64]) -> Vec<f64> {
    apply_vectors(transform, mode, coordinates).map_or_else(
        |_| vec![f64::NAN; coordinates.len()],
        |applied| applied.values,
    )
}

#[test]
fn identity_and_translation_follow_cpp_point_and_direction_rules() {
    assert_point(&IDENTITY, vec3(1.0, 2.0, 3.0), vec3(1.0, 2.0, 3.0));
    assert_direction(&IDENTITY, vec3(1.0, 2.0, 3.0), vec3(1.0, 2.0, 3.0));
    assert_normal(&IDENTITY, vec3(1.0, 2.0, 3.0), vec3(1.0, 2.0, 3.0));

    assert_point(&TRANSLATION, vec3(1.0, 2.0, 3.0), vec3(11.0, -18.0, 3.5));
    assert_direction(&TRANSLATION, vec3(1.0, 2.0, 3.0), vec3(1.0, 2.0, 3.0));
    assert_normal(&TRANSLATION, vec3(1.0, 2.0, 3.0), vec3(1.0, 2.0, 3.0));

    assert_eq!(
        apply_bits(
            &TRANSLATION,
            ApplyMode::Point,
            &[1.0, 2.0, 3.0, -1.0, 0.0, 0.5]
        ),
        Ok((bits_of(&[11.0, -18.0, 3.5, 9.0, -20.0, 1.0]), false))
    );
}

#[test]
fn cpp_quarter_turn_literal_preserves_rows_and_translation_role() {
    assert_point(
        &CPP_ROTATION_Z_90,
        vec3(1.0, 0.0, 0.0),
        vec3(CPP_COS_90, 1.0, 0.0),
    );
    assert_direction(
        &CPP_ROTATION_Z_90,
        vec3(0.0, 1.0, 0.0),
        vec3(-1.0, CPP_COS_90, 0.0),
    );
    assert_normal(
        &CPP_ROTATION_Z_90,
        vec3(1.0, 0.0, 0.0),
        vec3(CPP_COS_90, 1.0, 0.0),
    );
    assert_eq!(
        summary(apply_vectors(
            &CPP_ROTATION_Z_90,
            ApplyMode::Point,
            &[1.0, 0.0, 0.0]
        )),
        Ok((3, false))
    );
}

#[test]
fn nonuniform_scale_mirror_and_shear_normals_use_the_cofactor_inverse_transpose() {
    let scaled_tangent = or_nan(apply_to_direction(&NONUNIFORM_SCALE, vec3(1.0, 1.0, 0.0)));
    let scaled_normal = or_nan(apply_to_normal(&NONUNIFORM_SCALE, vec3(1.0, -1.0, 0.0)));
    assert_eq!(vec_bits(scaled_tangent), vec_bits(vec3(4.0, 1.0, 0.0)));
    assert_eq!(vec_bits(scaled_normal), vec_bits(vec3(0.25, -1.0, 0.0)));
    assert!(dot(scaled_tangent, scaled_normal).abs() <= 1e-12);

    // Transforming the normal as a direction would no longer be perpendicular,
    // which is exactly the bug the cofactor path exists to prevent.
    let wrong = or_nan(apply_to_direction(&NONUNIFORM_SCALE, vec3(1.0, -1.0, 0.0)));
    assert!(dot(wrong, scaled_tangent).abs() > 1e-6);

    assert_point(&MIRROR_X, vec3(1.0, 2.0, 3.0), vec3(-1.0, 2.0, 3.0));
    assert_direction(&MIRROR_X, vec3(1.0, 0.0, 0.0), vec3(-1.0, 0.0, 0.0));
    // Dividing the positive-zero cofactor products by the negative determinant
    // yields negative zero on y and z, matching the C++ expression exactly.
    assert_normal(&MIRROR_X, vec3(1.0, 0.0, 0.0), vec3(-1.0, -0.0, -0.0));
    let mirror_tangent = or_nan(apply_to_direction(&MIRROR_X, vec3(0.0, 1.0, 0.0)));
    let mirror_normal = or_nan(apply_to_normal(&MIRROR_X, vec3(1.0, 0.0, 0.0)));
    assert!(dot(mirror_tangent, mirror_normal).abs() <= 1e-12);
    assert_eq!(
        summary(apply_vectors(
            &MIRROR_X,
            ApplyMode::Direction,
            &[1.0, 0.0, 0.0]
        )),
        Ok((3, true))
    );

    assert_direction(&SHEAR_XY, vec3(0.0, 1.0, 0.0), vec3(2.0, 1.0, 0.0));
    assert_normal(&SHEAR_XY, vec3(1.0, 0.0, 0.0), vec3(1.0, -2.0, 0.0));
    let shear_tangent = or_nan(apply_to_direction(&SHEAR_XY, vec3(0.0, 1.0, 0.0)));
    let shear_normal = or_nan(apply_to_normal(&SHEAR_XY, vec3(1.0, 0.0, 0.0)));
    assert!(dot(shear_tangent, shear_normal).abs() <= 1e-12);
}

#[test]
fn composed_cpp_matrix_literal_applies_linear_and_translation_rows() {
    assert_point(&CPP_COMPOSED, vec3(1.0, 1.0, 1.0), vec3(12.0, 23.0, 34.0));
    assert_direction(&CPP_COMPOSED, vec3(1.0, 1.0, 1.0), vec3(2.0, 3.0, 4.0));
    assert_normal(&CPP_COMPOSED, vec3(2.0, 3.0, 4.0), vec3(1.0, 1.0, 1.0));

    let normal = or_nan(apply_to_normal(&CPP_COMPOSED, vec3(2.0, 3.0, 4.0)));
    let tangent = or_nan(apply_to_direction(&CPP_COMPOSED, vec3(3.0, -2.0, 0.0)));
    assert!(dot(normal, tangent).abs() <= 1e-12);
    assert_eq!(
        apply_bits(&CPP_COMPOSED, ApplyMode::Point, &[0.0, 0.0, 0.0]),
        Ok((bits_of(&[10.0, 20.0, 30.0]), false))
    );
}

#[test]
fn raw_singular_normal_is_returned_unchanged_including_negative_zero() {
    let normal = vec3(-0.0, 0.5, -0.0);
    assert_eq!(
        apply_to_normal(&SINGULAR, normal).map(vec_bits),
        Ok(vec_bits(normal))
    );
    assert_eq!(
        apply_bits(&SINGULAR, ApplyMode::Normal, &[-0.0, 0.5, -0.0]),
        Ok((bits_of(&[-0.0, 0.5, -0.0]), false))
    );
    assert_eq!(
        apply_to_normal(&SINGULAR_NEGATIVE_ZERO, normal).map(vec_bits),
        Ok(vec_bits(normal))
    );

    // Point and direction application still uses the linear rows; only the
    // normal inverse has the documented exactly-singular legacy return.
    assert_point(&SINGULAR, vec3(1.0, 2.0, 3.0), vec3(5.0, 10.0, 3.0));
    assert_direction(&SINGULAR, vec3(1.0, 2.0, 3.0), vec3(5.0, 10.0, 3.0));
    assert_eq!(
        summary(apply_vectors(&SINGULAR, ApplyMode::Point, &[1.0, 2.0, 3.0])),
        Ok((3, false))
    );
}

#[test]
fn large_georeferenced_positions_retain_f64_precision() {
    assert_point(
        &GEOREFERRED,
        vec3(123.5, -67.25, 0.125),
        vec3(4_500_123.5, 5_299_932.75, 250.125),
    );
    assert_direction(
        &GEOREFERRED,
        vec3(123.5, -67.25, 0.125),
        vec3(123.5, -67.25, 0.125),
    );
    assert_eq!(
        apply_bits(&GEOREFERRED, ApplyMode::Point, &[123.5, -67.25, 0.125]),
        Ok((bits_of(&[4_500_123.5, 5_299_932.75, 250.125]), false,))
    );
}

#[test]
fn empty_input_is_valid_and_still_reports_orientation() {
    assert_eq!(
        summary(apply_vectors(&IDENTITY, ApplyMode::Point, &[])),
        Ok((0, false))
    );
    assert_eq!(
        summary(apply_vectors(&MIRROR_X, ApplyMode::Normal, &[])),
        Ok((0, true))
    );
    assert_eq!(
        summary(apply_vectors(&SINGULAR, ApplyMode::Normal, &[])),
        Ok((0, false))
    );
}

#[test]
fn malformed_shapes_and_nonfinite_values_are_refused() {
    for length in [1, 2, 4, 5] {
        let coordinates = vec![0.0; length];
        assert_eq!(
            apply_vectors(&IDENTITY, ApplyMode::Point, &coordinates),
            Err(Error::InputLengthNotMultipleOfThree)
        );
    }

    for value in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY] {
        assert_eq!(
            apply_vectors(&IDENTITY, ApplyMode::Direction, &[value, 0.0, 0.0]),
            Err(Error::NonFiniteInput)
        );
        assert_eq!(
            apply_to_point(&IDENTITY, vec3(value, 0.0, 0.0)),
            Err(Error::NonFiniteInput)
        );
    }

    for transform in [
        NAN_COEFFICIENT,
        INFINITE_COEFFICIENT,
        OVERFLOWING_DETERMINANT,
    ] {
        assert_eq!(
            apply_vectors(&transform, ApplyMode::Point, &[]),
            Err(Error::NonFiniteTransform)
        );
        assert_eq!(
            apply_to_point(&transform, vec3(0.0, 0.0, 0.0)),
            Err(Error::NonFiniteTransform)
        );
        assert_eq!(
            apply_to_normal(&transform, vec3(1.0, 0.0, 0.0)),
            Err(Error::NonFiniteTransform)
        );
    }
}

#[test]
fn unrepresentable_results_refuse_the_whole_batch_without_partial_output() {
    assert_eq!(
        apply_to_point(&OVERFLOW_TRANSLATION, vec3(1e308, 0.0, 0.0)),
        Err(Error::NonFiniteOutput)
    );
    assert_eq!(
        apply_vectors(
            &OVERFLOW_TRANSLATION,
            ApplyMode::Point,
            &[1.0, 2.0, 3.0, 1e308, 0.0, 0.0]
        ),
        Err(Error::NonFiniteOutput)
    );
    assert_eq!(
        apply_to_normal(&TINY_SCALE, vec3(1e307, 0.0, 0.0)),
        Err(Error::NonFiniteOutput)
    );
    assert_eq!(
        apply_vectors(
            &TINY_SCALE,
            ApplyMode::Normal,
            &[1.0, 0.0, 0.0, 1e307, 0.0, 0.0]
        ),
        Err(Error::NonFiniteOutput)
    );
}

#[test]
fn seeded_batches_match_scalar_calls_repeat_exactly_and_do_not_mutate_input() {
    let coordinates = seeded_values(0x0BAD_5EED_1234_5678, 48);
    let before = bits_of(&coordinates);

    for mode in [ApplyMode::Point, ApplyMode::Direction, ApplyMode::Normal] {
        let batch = apply_vectors(&CPP_COMPOSED, mode, &coordinates).map(|applied| {
            applied
                .values
                .as_chunks::<3>()
                .0
                .iter()
                .map(|&[x, y, z]| vec3(x, y, z))
                .collect::<Vec<_>>()
        });
        let scalar: Result<Vec<Vec3>, Error> = coordinates
            .as_chunks::<3>()
            .0
            .iter()
            .map(|&[x, y, z]| {
                let value = vec3(x, y, z);
                match mode {
                    ApplyMode::Point => apply_to_point(&CPP_COMPOSED, value),
                    ApplyMode::Direction => apply_to_direction(&CPP_COMPOSED, value),
                    ApplyMode::Normal => apply_to_normal(&CPP_COMPOSED, value),
                }
            })
            .collect();
        assert_eq!(
            scalar.map(|values| values.into_iter().map(vec_bits).collect::<Vec<_>>()),
            batch.map(|values| values.into_iter().map(vec_bits).collect::<Vec<_>>())
        );

        let first = apply_bits(&CPP_COMPOSED, mode, &coordinates);
        let second = apply_bits(&CPP_COMPOSED, mode, &coordinates);
        assert_eq!(first, second);
    }

    assert_eq!(before, bits_of(&coordinates));
}

#[test]
fn seeded_direction_and_normal_maps_are_linear() {
    let left = seeded_values(0x1111_2222_3333_4444, 16);
    let right = seeded_values(0x5555_6666_7777_8888, 16);
    let combined: Vec<f64> = left
        .iter()
        .zip(&right)
        .map(|(left, right)| left + right)
        .collect();

    for mode in [ApplyMode::Direction, ApplyMode::Normal] {
        let left_values = applied_values(&CPP_COMPOSED, mode, &left);
        let right_values = applied_values(&CPP_COMPOSED, mode, &right);
        let combined_values = applied_values(&CPP_COMPOSED, mode, &combined);
        for ((&left, &right), &sum) in left_values.iter().zip(&right_values).zip(&combined_values) {
            let expected = left + right;
            let scale = expected.abs().max(sum.abs()).max(1.0);
            assert!(
                (expected - sum).abs() <= 1e-12 * scale,
                "expected {expected} but got {sum}"
            );
        }
    }
}

#[test]
fn error_messages_are_stable_and_named() {
    for (error, prefix) in [
        (
            Error::InputLengthNotMultipleOfThree,
            "InputLengthNotMultipleOfThree:",
        ),
        (Error::NonFiniteInput, "NonFiniteInput:"),
        (Error::NonFiniteTransform, "NonFiniteTransform:"),
        (Error::NonFiniteOutput, "NonFiniteOutput:"),
    ] {
        assert!(error.message().starts_with(prefix));
        assert_eq!(error.to_string(), error.message());
    }
}
