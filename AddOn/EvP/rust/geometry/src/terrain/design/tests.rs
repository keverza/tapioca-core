use super::*;

fn close(left: f64, right: f64, tolerance: f64) -> bool {
    (left - right).abs() <= tolerance
}

fn plane(box2: [f64; 4], z: impl Fn(f64, f64) -> f64) -> Result<Field, DesignError> {
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
    Field::new(&points, &[0, 1, 2, 0, 2, 3]).map_err(|_| DesignError::Coverage)
}

const HALF: Slope = Slope::RiseRun {
    rise: 1.0,
    run: 2.0,
};

/// Worked-fixture station: 4 m wide, 1:2 faces, 30 m limits, no shoulder.
fn station(x: f64, z: f64) -> Station {
    Station {
        position: [x, 0.0, z],
        half_left: 2.0,
        half_right: 2.0,
        crossfall: None,
        shoulder_left: 0.0,
        shoulder_right: 0.0,
        cut: HALF,
        fill: HALF,
        cut_limit: 30.0,
        fill_limit: 30.0,
    }
}

/// Ground `10 + 0.05 x - 0.2 y`: a 5 % grade along the path, 20 % across it.
fn worked_ground() -> Result<Field, DesignError> {
    plane([-10.0, -40.0, 50.0, 40.0], |x, y| 10.0 + 0.05 * x - 0.2 * y)
}

fn worked_path() -> [Station; 2] {
    [station(0.0, 10.0), station(40.0, 12.0)]
}

fn roots(locus: Option<&Option<Locus>>) -> Vec<f64> {
    match locus {
        Some(Some(Locus::Met { roots })) => roots.clone(),
        _ => Vec::new(),
    }
}

/// `(fill, cut)` m^2 per metre of the cross-section at `x`, integrated exactly:
/// `design - base` is linear between consecutive `breaks` (band edges and
/// daylight lines), so each piece is a trapezoid split at its zero crossing.
fn section(
    design: &Design<'_>,
    field: &Field,
    x: f64,
    breaks: &[f64],
) -> Result<(f64, f64), DesignError> {
    let mut values = Vec::new();
    for &y in breaks {
        let height = design.height(x, y)?.ok_or(DesignError::Coverage)?.height;
        let base = field.height_at(x, y).ok_or(DesignError::Coverage)?.height;
        values.push((y, height - base));
    }
    let (mut fill, mut cut) = (0.0, 0.0);
    for pair in values.windows(2) {
        let [(y0, g0), (y1, g1)] = *pair else {
            continue;
        };
        let length = y1 - y0;
        if g0 * g1 >= 0.0 {
            let area = f64::midpoint(g0, g1) * length;
            if area > 0.0 {
                fill += area;
            } else {
                cut -= area;
            }
        } else {
            let zero = length * g0 / (g0 - g1);
            let (first, second) = (0.5 * g0 * zero, 0.5 * g1 * (length - zero));
            for area in [first, second] {
                if area > 0.0 {
                    fill += area;
                } else {
                    cut -= area;
                }
            }
        }
    }
    Ok((fill, cut))
}

/// The worked fixture's lateral breaks from the design's own daylight.
fn worked_breaks(design: &Design<'_>) -> Result<Vec<f64>, DesignError> {
    let daylight = design.daylight()?;
    let left = roots(daylight.first().and_then(|sides| sides.first()));
    let right = roots(daylight.first().and_then(|sides| sides.get(1)));
    let (Some(&l), Some(&r)) = (left.first(), right.first()) else {
        return Err(DesignError::Coverage);
    };
    Ok(vec![-10.0, -2.0 - r, -2.0, 2.0, 2.0 + l, 10.0])
}

#[test]
fn worked_fixture_classifies_daylights_and_balances_cut_and_fill() -> Result<(), DesignError> {
    let field = worked_ground()?;
    let stations = worked_path();
    let band = Band {
        stations: &stations,
        effect: Effect::Both,
    };
    let design = Design::new(&band, &field, 1e-6)?;
    for sides in design.classify()? {
        let [left, right] = sides;
        assert_eq!(left.classification, Classification::Fill);
        assert_eq!(right.classification, Classification::Cut);
        assert!(close(left.delta, 0.4, 1e-12) && close(right.delta, -0.4, 1e-12));
    }
    for sides in design.daylight()? {
        for side in &sides {
            let found = roots(Some(side));
            assert_eq!(found.len(), 1, "{side:?}");
            assert!(found.first().is_some_and(|&r| close(r, 4.0 / 3.0, 1e-12)));
        }
    }
    let breaks = worked_breaks(&design)?;
    for x in [5.0, 20.0, 35.0] {
        let (fill, cut) = section(&design, &field, x, &breaks)?;
        // Band 0.4 + wedge 0.26667 m^2/m on each side.
        assert!(
            close(fill, 2.0 / 3.0, 1e-12) && close(cut, 2.0 / 3.0, 1e-12),
            "{fill} {cut}"
        );
        // Over the 40 m run (end caps excluded, as in the fixture).
        assert!(close(fill * 40.0, 80.0 / 3.0, 1e-9) && close(cut * 40.0, 80.0 / 3.0, 1e-9));
        let z = 10.0 + 0.05 * x;
        let at = |y: f64| design.height(x, y).map(|s| s.map(|s| (s.height, s.region)));
        assert_eq!(at(1.0)?.map(|(_, r)| r), Some(Region::Band));
        assert!(at(1.0)?.is_some_and(|(h, _)| close(h, z, 1e-12)));
        // Left transition 1 m out: the fill face, 0.5 m below the edge.
        assert!(at(3.0)?.is_some_and(|(h, r)| close(h, z - 0.5, 1e-12)
            && r == Region::Transition {
                side: Hand::Left,
                classification: Classification::Fill
            }));
        // Past the daylight the ground wins.
        assert!(at(4.0)?.is_some_and(|(h, _)| close(h, z - 0.8, 1e-12)));
        assert!(at(-4.0)?.is_some_and(|(h, _)| close(h, z + 0.8, 1e-12)));
    }
    Ok(())
}

#[test]
fn a_shoulder_moves_the_daylight_out_and_keeps_its_volume_honest() -> Result<(), DesignError> {
    let field = worked_ground()?;
    let mut stations = worked_path();
    for station in &mut stations {
        station.shoulder_left = 0.5;
        station.shoulder_right = 0.5;
    }
    let band = Band {
        stations: &stations,
        effect: Effect::Both,
    };
    let design = Design::new(&band, &field, 1e-6)?;
    let breaks = worked_breaks(&design)?;
    let shoulder: Vec<f64> = vec![
        -10.0,
        -2.0 - 0.65 / 0.3,
        -2.5,
        -2.0,
        2.0,
        2.5,
        2.0 + 0.65 / 0.3,
        10.0,
    ];
    assert!(close(
        breaks.get(1).copied().unwrap_or(0.0),
        -2.0 - 0.65 / 0.3,
        1e-12
    ));
    let (fill, cut) = section(&design, &field, 20.0, &shoulder)?;
    assert!(
        close(fill * 40.0, 125.0 / 3.0, 1e-9) && close(cut * 40.0, 125.0 / 3.0, 1e-9),
        "{fill} {cut}"
    );
    Ok(())
}

#[test]
fn an_effect_restriction_keeps_only_its_own_side() -> Result<(), DesignError> {
    let field = worked_ground()?;
    let stations = worked_path();
    let design = Design::new(
        &Band {
            stations: &stations,
            effect: Effect::Cut,
        },
        &field,
        1e-6,
    )?;
    // Restricting to cut kinks the band where the design crosses the ground (y = 0).
    let breaks = vec![
        -10.0,
        -2.0 - 4.0 / 3.0,
        -2.0,
        0.0,
        2.0,
        2.0 + 4.0 / 3.0,
        10.0,
    ];
    let (fill, cut) = section(&design, &field, 20.0, &breaks)?;
    assert!(
        close(fill, 0.0, 0.0) && close(cut, 2.0 / 3.0, 1e-12),
        "{fill} {cut}"
    );
    Ok(())
}

#[test]
fn crossfall_twists_both_ways_and_per_side_widths_hold() -> Result<(), DesignError> {
    let field = plane([-10.0, -20.0, 50.0, 20.0], |_, _| 0.0)?;
    for (hand, sign) in [(Hand::Left, -1.0), (Hand::Right, 1.0)] {
        let mut stations = [station(0.0, 1.0), station(40.0, 1.0)];
        for station in &mut stations {
            station.half_left = 3.0;
            station.half_right = 1.0;
            station.crossfall = Some(Crossfall {
                slope: Slope::Percent(2.0),
                falls_to: hand,
            });
        }
        let band = Band {
            stations: &stations,
            effect: Effect::Both,
        };
        let design = Design::new(&band, &field, 1e-6)?;
        let edges = design.edges(0).ok_or(DesignError::Stations)?;
        assert_eq!(edges.map(|e| [e[0], e[1]]), [[0.0, 3.0], [0.0, -1.0]]);
        assert!(
            close(edges[0][2], 1.0 + sign * 0.06, 1e-12)
                && close(edges[1][2], 1.0 - sign * 0.02, 1e-12)
        );
        let at = |y: f64| {
            design
                .height(20.0, y)
                .map(|s| s.map(|s| (s.height, s.region)))
        };
        assert!(
            at(2.5)?.is_some_and(|(h, r)| close(h, 1.0 + sign * 0.05, 1e-12) && r == Region::Band)
        );
        assert!(at(-1.5)?.is_some_and(|(_, r)| matches!(
            r,
            Region::Transition {
                side: Hand::Right,
                ..
            }
        )));
    }
    Ok(())
}

#[test]
fn at_grade_vertical_and_past_the_ends_leave_the_base() -> Result<(), DesignError> {
    // Ground equal to the profile: both edges at grade, nothing beyond them.
    let flat = plane([-10.0, -40.0, 50.0, 40.0], |x, _| 10.0 + 0.05 * x)?;
    let stations = worked_path();
    let band = Band {
        stations: &stations,
        effect: Effect::Both,
    };
    let design = Design::new(&band, &flat, 1e-6)?;
    for sides in design.classify()? {
        assert!(
            sides
                .iter()
                .all(|side| side.classification == Classification::AtGrade)
        );
    }
    assert!(
        design
            .daylight()?
            .iter()
            .all(|sides| sides.iter().all(Option::is_none))
    );
    assert_eq!(
        design.height(20.0, 3.0)?.map(|s| s.region),
        Some(Region::Base)
    );

    let field = worked_ground()?;
    let design = Design::new(&band, &field, 1e-6)?;
    for (x, y) in [(-1.0, 0.0), (41.0, 1.0)] {
        assert!(design.height(x, y)?.is_some_and(
            |s| s.region == Region::Base && close(s.height, 10.0 + 0.05 * x - 0.2 * y, 1e-12)
        ));
    }
    assert_eq!(design.height(500.0, 0.0)?, None);

    let mut walls = worked_path();
    for station in &mut walls {
        station.cut = Slope::Angle {
            radians: std::f64::consts::FRAC_PI_2,
        };
    }
    let band = Band {
        stations: &walls,
        effect: Effect::Both,
    };
    let design = Design::new(&band, &field, 1e-6)?;
    assert!(matches!(
        design.daylight()?.first().and_then(|s| s.get(1)),
        Some(Some(Locus::Vertical))
    ));
    assert!(
        design
            .height(20.0, -3.0)?
            .is_some_and(|s| close(s.height, 11.0 + 0.6, 1e-12))
    );
    Ok(())
}

#[test]
fn malformed_bands_refuse_with_stable_codes() -> Result<(), DesignError> {
    let field = worked_ground()?;
    let make = |stations: &[Station], tolerance: f64| {
        Design::new(
            &Band {
                stations,
                effect: Effect::Both,
            },
            &field,
            tolerance,
        )
        .err()
    };
    let good = worked_path();
    assert_eq!(make(&good[..1], 0.0), Some(DesignError::Stations));
    assert_eq!(make(&[good[0], good[0]], 0.0), Some(DesignError::Stations));
    assert_eq!(make(&good, f64::NAN), Some(DesignError::Parameters));
    let mut bad = good;
    bad[1].half_left = -1.0;
    assert_eq!(make(&bad, 0.0), Some(DesignError::Parameters));
    let mut bad = good;
    bad[0].fill = Slope::Percent(f64::NAN);
    assert_eq!(make(&bad, 0.0), Some(DesignError::Slope));
    let mut bad = good;
    bad[0].crossfall = Some(Crossfall {
        slope: Slope::Angle {
            radians: std::f64::consts::FRAC_PI_2,
        },
        falls_to: Hand::Left,
    });
    assert_eq!(make(&bad, 0.0), Some(DesignError::Slope));
    let mut wide = good;
    wide[0].half_left = 100.0;
    let design = Design::new(
        &Band {
            stations: &wide,
            effect: Effect::Both,
        },
        &field,
        0.0,
    )?;
    assert_eq!(design.classify().err(), Some(DesignError::Coverage));
    assert!(
        DesignError::Daylight(DaylightError::Slope)
            .message()
            .starts_with("DaylightSlope:")
    );
    Ok(())
}
