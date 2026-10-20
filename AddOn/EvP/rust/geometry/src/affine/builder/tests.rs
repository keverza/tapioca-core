use super::*;
use crate::affine::{apply_to_point, compose_transforms};

const ZERO: Vec3 = Vec3 {
    x: 0.0,
    y: 0.0,
    z: 0.0,
};
const Z: Vec3 = Vec3 {
    x: 0.0,
    y: 0.0,
    z: 1.0,
};

fn close(actual: Vec3, expected: Vec3, tolerance: f64) {
    for (a, b) in components(actual).into_iter().zip(components(expected)) {
        assert!((a - b).abs() <= tolerance, "{a} != {b}");
    }
}
fn point(matrix: &Transform, value: Vec3) -> Result<Vec3, BuildError> {
    apply_to_point(matrix, value).map_err(|_| BuildError::ArithmeticRange)
}

#[test]
fn translation_and_negative_nonuniform_scale_preserve_pivot() -> Result<(), BuildError> {
    let origin = Vec3 {
        x: 700_000.0,
        y: 6_000_000.0,
        z: 120.0,
    };
    let translation = build_transform(TransformBuilder::Translation(origin))?;
    close(point(&translation, ZERO)?, origin, 0.0);
    let scale = build_transform(TransformBuilder::Scaling {
        origin,
        factors: Vec3 {
            x: -2.0,
            y: 3.0,
            z: 0.5,
        },
    })?;
    close(point(&scale, origin)?, origin, 0.0);
    close(
        point(
            &scale,
            Vec3 {
                x: 700_001.0,
                y: 6_000_002.0,
                z: 122.0,
            },
        )?,
        Vec3 {
            x: 699_998.0,
            y: 6_000_006.0,
            z: 121.0,
        },
        0.0,
    );
    Ok(())
}

#[test]
fn rotation_and_mirror_have_independent_analytic_answers() -> Result<(), BuildError> {
    let origin = Vec3 {
        x: 3.0,
        y: 5.0,
        z: 7.0,
    };
    let rotation = build_transform(TransformBuilder::Rotation {
        origin,
        axis: Z,
        radians: std::f64::consts::FRAC_PI_2,
    })?;
    close(point(&rotation, origin)?, origin, 1e-14);
    close(
        point(
            &rotation,
            Vec3 {
                x: 4.0,
                y: 5.0,
                z: 7.0,
            },
        )?,
        Vec3 {
            x: 3.0,
            y: 6.0,
            z: 7.0,
        },
        1e-14,
    );
    let mirror = build_transform(TransformBuilder::Mirroring { origin, normal: Z })?;
    close(
        point(
            &mirror,
            Vec3 {
                x: 4.0,
                y: 6.0,
                z: 8.0,
            },
        )?,
        Vec3 {
            x: 4.0,
            y: 6.0,
            z: 6.0,
        },
        0.0,
    );
    let twice = compose_transforms(&[mirror, mirror]).map_err(|_| BuildError::ArithmeticRange)?;
    assert_eq!(twice, Transform::IDENTITY);
    Ok(())
}

#[test]
fn zero_factors_and_unusable_squared_direction_norms_refuse() {
    for factor in [0.0, -0.0] {
        for factors in [
            Vec3 {
                x: factor,
                y: 1.0,
                z: 1.0,
            },
            Vec3 {
                x: 1.0,
                y: factor,
                z: 1.0,
            },
            Vec3 {
                x: 1.0,
                y: 1.0,
                z: factor,
            },
        ] {
            assert_eq!(
                build_transform(TransformBuilder::Scaling {
                    origin: ZERO,
                    factors
                }),
                Err(BuildError::ZeroScale)
            );
        }
    }
    for z in [0.0, 1e-200, 1e200] {
        let direction = Vec3 { x: 0.0, y: 0.0, z };
        assert_eq!(
            build_transform(TransformBuilder::Rotation {
                origin: ZERO,
                axis: direction,
                radians: 1.0
            }),
            Err(BuildError::Direction)
        );
        assert_eq!(
            build_transform(TransformBuilder::Mirroring {
                origin: ZERO,
                normal: direction
            }),
            Err(BuildError::Direction)
        );
    }
}

#[test]
fn nonfinite_used_inputs_refuse_for_every_builder() {
    for invalid in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY] {
        for bad in [
            Vec3 { x: invalid, ..ZERO },
            Vec3 { y: invalid, ..ZERO },
            Vec3 { z: invalid, ..ZERO },
        ] {
            for builder in [
                TransformBuilder::Translation(bad),
                TransformBuilder::Rotation {
                    origin: bad,
                    axis: Z,
                    radians: 0.0,
                },
                TransformBuilder::Rotation {
                    origin: ZERO,
                    axis: bad,
                    radians: 0.0,
                },
                TransformBuilder::Scaling {
                    origin: bad,
                    factors: Z,
                },
                TransformBuilder::Scaling {
                    origin: ZERO,
                    factors: bad,
                },
                TransformBuilder::Mirroring {
                    origin: bad,
                    normal: Z,
                },
                TransformBuilder::Mirroring {
                    origin: ZERO,
                    normal: bad,
                },
                TransformBuilder::Rotation {
                    origin: ZERO,
                    axis: Z,
                    radians: invalid,
                },
            ] {
                assert_eq!(build_transform(builder), Err(BuildError::NonFiniteInput));
            }
        }
    }
}

#[test]
fn overflowing_pivot_and_determinant_refuse_but_underflowing_determinant_is_preserved()
-> Result<(), BuildError> {
    let huge = Vec3 {
        x: f64::MAX,
        y: 0.0,
        z: 0.0,
    };
    assert_eq!(
        build_transform(TransformBuilder::Scaling {
            origin: huge,
            factors: Vec3 {
                x: -2.0,
                y: 1.0,
                z: 1.0
            }
        }),
        Err(BuildError::ArithmeticRange)
    );
    assert_eq!(
        build_transform(TransformBuilder::Mirroring {
            origin: huge,
            normal: Vec3 { x: 1.0, ..ZERO }
        }),
        Err(BuildError::ArithmeticRange)
    );
    assert_eq!(
        build_transform(TransformBuilder::Rotation {
            origin: huge,
            axis: Z,
            radians: std::f64::consts::PI
        }),
        Err(BuildError::ArithmeticRange)
    );
    assert_eq!(
        build_transform(TransformBuilder::Scaling {
            origin: ZERO,
            factors: Vec3 {
                x: 1e200,
                y: 1e200,
                z: 1.0
            }
        }),
        Err(BuildError::ArithmeticRange)
    );
    let tiny = build_transform(TransformBuilder::Scaling {
        origin: ZERO,
        factors: Vec3 {
            x: 1e-200,
            y: 1e-200,
            z: 1e-200,
        },
    })?;
    assert_eq!(
        super::super::determinant(&tiny.m).to_bits(),
        0.0_f64.to_bits()
    );
    assert!(tiny.m.iter().flatten().all(|v| v.is_finite()));
    let reflected_tiny = build_transform(TransformBuilder::Scaling {
        origin: ZERO,
        factors: Vec3 {
            x: -1e-200,
            y: 1e-200,
            z: 1e-200,
        },
    })?;
    // Retain the existing determinant arithmetic, not mathematical sign recovery.
    assert!(super::super::determinant(&reflected_tiny.m).abs() <= 0.0);
    Ok(())
}

#[test]
fn finite_extreme_angle_and_zero_rotation_remain_valid() -> Result<(), BuildError> {
    for radians in [0.0, -0.0, 1e100] {
        let builder = TransformBuilder::Rotation {
            origin: ZERO,
            axis: Vec3 {
                x: 0.0,
                y: 0.0,
                z: 1e-150,
            },
            radians,
        };
        let result = build_transform(builder)?;
        assert_eq!(result, build_transform(builder)?);
        assert!(result.m.iter().flatten().all(|v| v.is_finite()));
    }
    Ok(())
}
