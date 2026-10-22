use super::*;
use crate::slope::Slope;

fn close(left: f64, right: f64, tolerance: f64) -> bool {
    (left - right).abs() <= tolerance
}

/// Two counter-clockwise triangles over `[x0, x1] x [y0, y1]` carrying `z`.
fn plane(box2: [f64; 4], z: impl Fn(f64, f64) -> f64) -> Result<Field, DaylightError> {
    let [x0, y0, x1, y1] = box2;
    let points = [
        x0,
        y0,
        z(x0, y0),
        x1,
        y0,
        z(x1, y0),
        x1,
        y1,
        z(x1, y1),
        x0,
        y1,
        z(x0, y1),
    ];
    Field::new(&points, &[0, 1, 2, 0, 2, 3]).map_err(|_| DaylightError::Coverage)
}

/// A strip `x in [-5, 5]` whose height varies along y through `profile` rows.
fn strip(profile: &[(f64, f64)]) -> Result<Field, DaylightError> {
    let mut points = Vec::new();
    for &(y, z) in profile {
        points.extend([-5.0, y, z, 5.0, y, z]);
    }
    let mut triangles = Vec::new();
    for row in 0..u32::try_from(profile.len()).unwrap_or(1) - 1 {
        let (a, b, c, d) = (2 * row, 2 * row + 1, 2 * row + 3, 2 * row + 2);
        triangles.extend([a, b, c, a, c, d]);
    }
    Field::new(&points, &triangles).map_err(|_| DaylightError::Coverage)
}

fn roots(locus: &Locus) -> &[f64] {
    match locus {
        Locus::Met { roots } => roots,
        _ => &[],
    }
}

fn face(slope: Slope, limit: f64) -> Daylight {
    Daylight {
        slope,
        extension: Some(limit),
    }
}

const HALF: Slope = Slope::RiseRun {
    rise: 1.0,
    run: 2.0,
};

/// The worked fixture's base: `10 + 0.05 x - 0.2 y`.
fn worked() -> Result<Field, DaylightError> {
    plane([-10.0, -40.0, 50.0, 40.0], |x, y| 10.0 + 0.05 * x - 0.2 * y)
}

#[test]
fn a_plane_reproduces_the_closed_form_root_on_both_senses() -> Result<(), DaylightError> {
    let field = worked()?;
    for (edge, outward, sense) in [
        ([20.0, 2.0, 11.0], [0.0, 1.0], Sense::Fill),
        ([20.0, -2.0, 11.0], [0.0, -1.0], Sense::Cut),
    ] {
        let ray = Ray {
            edge,
            outward,
            shoulder: 0.0,
        };
        let locus = daylight_roots(&ray, sense, face(HALF, 30.0), &field)?;
        assert_eq!(roots(&locus).len(), 1, "{locus:?}");
        let root = roots(&locus).first().copied().unwrap_or(f64::NAN);
        assert!(close(root, 4.0 / 3.0, 1e-12), "{root}");
        // C0 at the daylight: the cone equals the ground there.
        let at = [edge[0] + outward[0] * root, edge[1] + outward[1] * root];
        let sign = if sense == Sense::Cut { 1.0 } else { -1.0 };
        let ground = field.height_at(at[0], at[1]).map(|s| s.height);
        assert!(ground.is_some_and(|g| close(edge[2] + sign * 0.5 * root, g, 1e-12)));
    }
    Ok(())
}

#[test]
fn a_shoulder_holds_the_edge_height_before_the_face_starts() -> Result<(), DaylightError> {
    let field = worked()?;
    let ray = Ray {
        edge: [20.0, 2.0, 11.0],
        outward: [0.0, 7.0], // Normalised internally.
        shoulder: 0.5,
    };
    let locus = daylight_roots(&ray, Sense::Fill, face(HALF, 30.0), &field)?;
    assert_eq!(roots(&locus).len(), 1);
    assert!(
        roots(&locus)
            .first()
            .is_some_and(|&r| close(r, 0.65 / 0.3, 1e-12))
    );
    Ok(())
}

/// Ground 0 to y=2, up to 2 at y=4, down to -1 at y=6, flat to y=12.
fn undulating() -> Result<Field, DaylightError> {
    strip(&[
        (-1.0, 0.0),
        (2.0, 0.0),
        (4.0, 2.0),
        (6.0, -1.0),
        (12.0, -1.0),
    ])
}

const QUARTER: Slope = Slope::Percent(25.0);

#[test]
fn undulating_ground_returns_every_root_in_order() -> Result<(), DaylightError> {
    let field = undulating()?;
    let ray = Ray {
        edge: [0.0, 0.0, 1.0],
        outward: [0.0, 1.0],
        shoulder: 0.0,
    };
    let locus = daylight_roots(&ray, Sense::Fill, face(QUARTER, 9.0), &field)?;
    let found = roots(&locus);
    assert_eq!(found.len(), 3, "{locus:?}");
    for (root, expected) in found.iter().zip([2.4, 5.6, 8.0]) {
        assert!(close(*root, expected, 1e-12), "{root} vs {expected}");
    }
    Ok(())
}

#[test]
fn a_short_limit_reports_the_root_beyond_it_without_clamping() -> Result<(), DaylightError> {
    let field = undulating()?;
    let ray = Ray {
        edge: [0.0, 0.0, 1.0],
        outward: [0.0, 1.0],
        shoulder: 0.0,
    };
    let locus = daylight_roots(&ray, Sense::Fill, face(QUARTER, 2.0), &field)?;
    let Locus::AtLimit { distance, residual } = locus else {
        return Err(DaylightError::Parameters);
    };
    assert!(close(distance, 2.4, 1e-12) && close(residual, 0.5, 1e-12));
    Ok(())
}

#[test]
fn ground_steeper_than_the_fill_is_not_reached_with_its_residual() -> Result<(), DaylightError> {
    let field = plane([-5.0, -1.0, 5.0, 20.0], |_, y| -0.5 * y)?;
    let ray = Ray {
        edge: [0.0, 0.0, 1.0],
        outward: [0.0, 1.0],
        shoulder: 0.0,
    };
    let locus = daylight_roots(&ray, Sense::Fill, face(QUARTER, 9.0), &field)?;
    let Locus::NotReached { at_limit, residual } = locus else {
        return Err(DaylightError::Parameters);
    };
    assert!(close(at_limit, 9.0, 0.0) && close(residual, 3.25, 1e-12));
    // Leaving the surface first ends the search where it was last covered.
    let locus = daylight_roots(&ray, Sense::Fill, face(QUARTER, 50.0), &field)?;
    let Locus::NotReached { at_limit, residual } = locus else {
        return Err(DaylightError::Parameters);
    };
    assert!(close(at_limit, 20.0, 1e-9) && close(residual, 6.0, 1e-9));
    Ok(())
}

#[test]
fn a_vertical_face_is_a_wall_not_a_steep_ramp() -> Result<(), DaylightError> {
    let field = worked()?;
    let ray = Ray {
        edge: [20.0, -2.0, 11.0],
        outward: [0.0, -1.0],
        shoulder: 0.0,
    };
    let vertical = Slope::Angle {
        radians: std::f64::consts::FRAC_PI_2,
    };
    assert_eq!(
        daylight_roots(&ray, Sense::Cut, face(vertical, 30.0), &field)?,
        Locus::Vertical
    );
    Ok(())
}

#[test]
fn malformed_rays_slopes_and_uncovered_edges_refuse() -> Result<(), DaylightError> {
    let field = worked()?;
    let good = Ray {
        edge: [20.0, 2.0, 11.0],
        outward: [0.0, 1.0],
        shoulder: 0.0,
    };
    let refuse = |ray: Ray, face: Daylight| daylight_roots(&ray, Sense::Fill, face, &field).err();
    assert_eq!(
        refuse(
            Ray {
                outward: [0.0, 0.0],
                ..good
            },
            face(HALF, 30.0)
        ),
        Some(DaylightError::Parameters)
    );
    assert_eq!(
        refuse(
            Ray {
                shoulder: -1.0,
                ..good
            },
            face(HALF, 30.0)
        ),
        Some(DaylightError::Parameters)
    );
    assert_eq!(
        refuse(
            Ray {
                edge: [f64::NAN, 0.0, 0.0],
                ..good
            },
            face(HALF, 30.0)
        ),
        Some(DaylightError::Parameters)
    );
    assert_eq!(
        refuse(good, face(HALF, -1.0)),
        Some(DaylightError::Parameters)
    );
    assert_eq!(
        refuse(good, face(Slope::Percent(-5.0), 30.0)),
        Some(DaylightError::Slope)
    );
    assert_eq!(
        refuse(
            Ray {
                edge: [500.0, 0.0, 0.0],
                ..good
            },
            face(HALF, 30.0)
        ),
        Some(DaylightError::Coverage)
    );
    assert!(
        DaylightError::Coverage
            .message()
            .starts_with("DaylightCoverage:")
    );
    Ok(())
}

#[test]
fn seeded_planar_bases_match_the_closed_form() -> Result<(), DaylightError> {
    // Fixed-seed LCG; no property-testing crate.
    let mut state = 0x2545_f491_4f6c_dd1d_u64;
    let mut next = move || {
        state = state
            .wrapping_mul(6_364_136_223_846_793_005)
            .wrapping_add(1_442_695_040_888_963_407);
        f64::from(u32::try_from(state >> 40).unwrap_or(0)) / f64::from(1_u32 << 24)
    };
    for _ in 0..64 {
        let rise = -0.2 + 0.8 * next(); // Ground grade along the ray.
        let across = -0.3 + 0.6 * next(); // Irrelevant cross grade.
        let gradient = 0.3 + 0.7 * next();
        let delta = 0.05 + 2.0 * next(); // Edge above the ground: fill.
        let offset = 1000.0 * next(); // Large world coordinates.
        let field = plane(
            [offset - 50.0, offset - 50.0, offset + 50.0, offset + 50.0],
            |x, y| 3.0 + across * (x - offset) + rise * (y - offset),
        )?;
        let base = 3.0;
        let ray = Ray {
            edge: [offset, offset, base + delta],
            outward: [0.0, 1.0],
            shoulder: 0.0,
        };
        let locus = daylight_roots(
            &ray,
            Sense::Fill,
            face(Slope::Percent(gradient * 100.0), 45.0),
            &field,
        )?;
        let expected = delta / (gradient + rise);
        let found = roots(&locus);
        assert_eq!(found.len(), 1, "{locus:?}");
        let root = found.first().copied().unwrap_or(f64::NAN);
        assert!(
            close(root, expected, 1e-12 * expected.max(1.0)),
            "{root} vs {expected}"
        );
        let ground = field.height_at(offset, offset + root).map(|s| s.height);
        assert!(ground.is_some_and(|g| close(base + delta - gradient * root, g, 1e-11)));
    }
    Ok(())
}
