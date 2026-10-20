use super::*;
use std::f64::consts::{PI, TAU};

fn request() -> ArcRequest {
    ArcRequest {
        centre: Vec3 {
            x: 0.0,
            y: 0.0,
            z: 0.0,
        },
        normal: Vec3 {
            x: 0.0,
            y: 0.0,
            z: 1.0,
        },
        radius: 2.0,
        start_radians: 0.0,
        sweep_radians: PI,
        segments: 2,
    }
}

#[test]
fn horizontal_order_and_closing_point_are_evaluated() -> Result<(), Error> {
    let points = make_arc(&request())?;
    for (&actual, expected) in points
        .iter()
        .zip([0.0, 2.0, 0.0, -2.0, 0.0, 0.0, 0.0, -2.0, 0.0])
    {
        assert!((actual - expected).abs() < 1e-14);
    }
    let circle = ArcRequest {
        sweep_radians: TAU,
        segments: 16,
        ..request()
    };
    let points = make_arc(&circle)?;
    assert_eq!(points.len(), 51);
    let first = points.first().ok_or(Error::Segments)?;
    let last_x = points.get(48).ok_or(Error::Segments)?;
    assert!((first - last_x).abs() < 1e-14);
    assert_ne!(first.to_bits(), last_x.to_bits(), "no closing-point snap");
    Ok(())
}

#[test]
fn oblique_survey_points_stay_on_radius_and_plane() -> Result<(), Error> {
    let arc = ArcRequest {
        centre: Vec3 {
            x: 700_000.0,
            y: 6_000_000.0,
            z: 120.0,
        },
        normal: Vec3 {
            x: 1.0,
            y: 2.0,
            z: 3.0,
        },
        radius: 10.0,
        start_radians: 0.31,
        sweep_radians: -TAU * 2.0,
        segments: 4096,
    };
    let before = arc;
    let points = make_arc(&arc)?;
    assert_eq!(points, make_arc(&arc)?);
    assert_eq!(arc.centre, before.centre);
    for [x, y, z] in points.as_chunks::<3>().0 {
        let (x, y, z) = (x - arc.centre.x, y - arc.centre.y, z - arc.centre.z);
        assert!(((x * x + y * y + z * z).sqrt() - 10.0).abs() < 2e-9);
        assert!((x + 2.0 * y + 3.0 * z).abs() < 2e-9);
    }
    Ok(())
}

#[test]
fn rejects_domain_bounds_and_nonfinite_values() {
    for segments in [i32::MIN, -1, 0, 1, 4097, i32::MAX] {
        assert_eq!(
            make_arc(&ArcRequest {
                segments,
                ..request()
            }),
            Err(Error::Segments)
        );
    }
    for radius in [0.0, -1.0] {
        assert_eq!(
            make_arc(&ArcRequest {
                radius,
                ..request()
            }),
            Err(Error::Degenerate)
        );
    }
    assert_eq!(
        make_arc(&ArcRequest {
            sweep_radians: -0.0,
            ..request()
        }),
        Err(Error::Degenerate)
    );
    for invalid in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY] {
        let mut cases = [request(); 9];
        {
            let [cx, cy, cz, nx, ny, nz, radius, start, sweep] = &mut cases;
            cx.centre.x = invalid;
            cy.centre.y = invalid;
            cz.centre.z = invalid;
            nx.normal.x = invalid;
            ny.normal.y = invalid;
            nz.normal.z = invalid;
            radius.radius = invalid;
            start.start_radians = invalid;
            sweep.sweep_radians = invalid;
        }
        for case in cases {
            assert_eq!(make_arc(&case), Err(Error::NonFiniteInput));
        }
    }
}

#[test]
fn refuses_squared_norm_underflow_overflow_and_angle_overflow() {
    for x in [0.0, 1e-200, 1e200] {
        let arc = ArcRequest {
            normal: Vec3 { x, y: 0.0, z: 0.0 },
            ..request()
        };
        assert_eq!(make_arc(&arc), Err(Error::Normal));
    }
    let arc = ArcRequest {
        start_radians: f64::MAX,
        sweep_radians: f64::MAX,
        ..request()
    };
    assert_eq!(make_arc(&arc), Err(Error::ArithmeticRange));
    let arc = ArcRequest {
        centre: Vec3 {
            x: 0.0,
            y: f64::MAX,
            z: 0.0,
        },
        radius: f64::MAX,
        ..request()
    };
    assert_eq!(make_arc(&arc), Err(Error::ArithmeticRange));
}

#[test]
fn finite_extreme_angles_and_tiny_radius_are_not_silently_clamped() -> Result<(), Error> {
    let arc = ArcRequest {
        start_radians: 1e100,
        sweep_radians: 1e99,
        radius: 1e-200,
        ..request()
    };
    let points = make_arc(&arc)?;
    assert_eq!(points.len(), 9);
    assert!(points.iter().all(|v| v.is_finite()));
    assert!(points.iter().any(|v| v.abs() > 0.0));
    Ok(())
}
