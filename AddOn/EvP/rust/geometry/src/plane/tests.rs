use super::*;

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
fn plane() -> Plane {
    Plane {
        origin: ZERO,
        normal: Z,
        u: ZERO,
    }
}
fn close(a: Vec3, b: Vec3, tolerance: f64) {
    for (x, y) in components(a).into_iter().zip(components(b)) {
        assert!((x - y).abs() <= tolerance, "{x} != {y}");
    }
}

#[test]
fn horizontal_and_authored_axes_have_independent_answers() -> Result<(), Error> {
    let p = Plane {
        origin: Vec3 { z: 103.25, ..ZERO },
        ..plane()
    };
    let result = project_to_plane(&p, &[4.0, 6.0, -10.0, 1.0, 2.0, 110.0])?;
    assert_eq!(result.points, vec![4.0, 6.0, 103.25, 1.0, 2.0, 103.25]);
    assert_eq!(result.frame.u, Vec3 { x: 1.0, ..ZERO });
    assert_eq!(result.frame.v, Vec3 { y: 1.0, ..ZERO });
    let p = Plane {
        u: Vec3 {
            x: 0.6,
            y: 0.8,
            z: 5.0,
        },
        ..p
    };
    let result = project_to_plane(&p, &[])?;
    close(
        result.frame.u,
        Vec3 {
            x: 0.6,
            y: 0.8,
            z: 0.0,
        },
        1e-15,
    );
    close(
        result.frame.v,
        Vec3 {
            x: -0.8,
            y: 0.6,
            z: 0.0,
        },
        1e-15,
    );
    assert_eq!(result.frame.plane, p);
    Ok(())
}

#[test]
fn tiny_normal_and_tangent_fallbacks_preserve_threshold_semantics() -> Result<(), Error> {
    for x in [0.0, 1e-200, 0.5e-12] {
        let p = Plane {
            normal: Vec3 { x, ..ZERO },
            ..plane()
        };
        let result = project_to_plane(&p, &[1.0, 2.0, 3.0])?;
        assert_eq!(result.points, vec![1.0, 2.0, 0.0]);
        assert_eq!(result.frame.plane, p);
    }
    let p = Plane {
        normal: Vec3 { x: 1e-12, ..ZERO },
        ..plane()
    };
    let result = project_to_plane(&p, &[1.0, 2.0, 3.0])?;
    assert_eq!(result.points, vec![0.0, 2.0, 3.0]);
    for (length, expected) in [
        (0.5e-12, Vec3 { x: 1.0, ..ZERO }),
        (1e-12, Vec3 { y: 1.0, ..ZERO }),
    ] {
        let result = project_to_plane(
            &Plane {
                u: Vec3 { y: length, ..ZERO },
                ..plane()
            },
            &[],
        )?;
        assert_eq!(result.frame.u, expected);
    }
    Ok(())
}

#[test]
fn raw_plane_fields_including_signed_zero_round_trip_exactly() -> Result<(), Error> {
    let p = Plane {
        origin: Vec3 {
            x: -0.0,
            y: 700_000.0,
            z: 120.0,
        },
        normal: Vec3 {
            x: -0.0,
            y: 0.0,
            z: 7.0,
        },
        u: Vec3 {
            x: 3.0,
            y: 4.0,
            z: -0.0,
        },
    };
    let result = project_to_plane(&p, &[])?;
    for (a, b) in [p.origin, p.normal, p.u].into_iter().zip([
        result.frame.plane.origin,
        result.frame.plane.normal,
        result.frame.plane.u,
    ]) {
        assert_eq!(
            components(a).map(f64::to_bits),
            components(b).map(f64::to_bits)
        );
    }
    assert_eq!(result.points, [] as [f64; 0]);
    Ok(())
}

#[test]
fn oblique_survey_projection_is_in_plane_and_idempotent() -> Result<(), Error> {
    let p = Plane {
        origin: Vec3 {
            x: 700_000.0,
            y: 6_000_000.0,
            z: 120.0,
        },
        normal: Vec3 {
            x: 1.0,
            y: 2.0,
            z: 3.0,
        },
        u: ZERO,
    };
    let input = [700_003.0, 6_000_004.0, 125.0, 700_010.0, 6_000_020.0, 150.0];
    let before = input.map(f64::to_bits);
    let result = project_to_plane(&p, &input)?;
    let repeat = project_to_plane(&p, &result.points)?;
    assert_eq!(input.map(f64::to_bits), before);
    for (&a, &b) in result.points.iter().zip(&repeat.points) {
        assert!((a - b).abs() < 2e-9);
    }
    for &point in result.points.as_chunks::<3>().0 {
        assert!(dot(sub(vector(point), p.origin)?, p.normal)?.abs() < 3e-9);
    }
    assert!(dot(result.frame.u, result.frame.v)?.abs() < 1e-15);
    assert!(dot(cross(result.frame.u, result.frame.v)?, p.normal)? > 0.0);
    assert_eq!(result, project_to_plane(&p, &input)?);
    Ok(())
}

#[test]
fn malformed_shape_and_every_nonfinite_plane_field_refuse() {
    assert_eq!(project_to_plane(&plane(), &[1.0]), Err(Error::Shape));
    for invalid in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY] {
        for axis in 0..3 {
            let mut bad = [0.0; 3];
            if let Some(value) = bad.get_mut(axis) {
                *value = invalid;
            }
            for p in [
                Plane {
                    origin: vector(bad),
                    ..plane()
                },
                Plane {
                    normal: vector(bad),
                    ..plane()
                },
                Plane {
                    u: vector(bad),
                    ..plane()
                },
            ] {
                assert_eq!(project_to_plane(&p, &[]), Err(Error::NonFiniteInput));
            }
        }
        assert_eq!(
            project_to_plane(&plane(), &[1.0, invalid, 3.0]),
            Err(Error::NonFiniteInput)
        );
    }
}

#[test]
fn nonfinite_derived_arithmetic_is_not_silently_fallen_back() {
    for p in [
        Plane {
            normal: Vec3 { x: 1e200, ..ZERO },
            ..plane()
        },
        Plane {
            u: Vec3 { x: 1e200, ..ZERO },
            ..plane()
        },
    ] {
        assert_eq!(project_to_plane(&p, &[]), Err(Error::ArithmeticRange));
    }
    let p = Plane {
        origin: Vec3 {
            x: -f64::MAX,
            ..ZERO
        },
        ..plane()
    };
    assert_eq!(
        project_to_plane(&p, &[f64::MAX, 0.0, 0.0]),
        Err(Error::ArithmeticRange)
    );
    let p = Plane {
        normal: Vec3 {
            x: 1.0,
            y: 1.0,
            z: 1.0,
        },
        ..plane()
    };
    assert_eq!(
        project_to_plane(&p, &[f64::MAX, f64::MAX, f64::MAX]),
        Err(Error::ArithmeticRange)
    );
}
