use super::*;
use crate::slope::Slope;

fn close(left: f64, right: f64, tolerance: f64) -> bool {
    (left - right).abs() <= tolerance
}

/// `side x side` vertices at 1 m, heights `z(x, y)`, two triangles per cell.
fn grid(side: u32, z: impl Fn(f64, f64) -> f64) -> (Vec<f64>, Vec<u32>) {
    let mut points = Vec::new();
    for row in 0..side {
        for column in 0..side {
            let (x, y) = (f64::from(column), f64::from(row));
            points.extend([x, y, z(x, y)]);
        }
    }
    let mut triangles = Vec::new();
    for row in 0..side - 1 {
        for column in 0..side - 1 {
            let a = row * side + column;
            triangles.extend([a, a + 1, a + side + 1, a, a + side + 1, a + side]);
        }
    }
    (points, triangles)
}

fn vertex(side: u32, column: u32, row: u32) -> usize {
    (row * side + column) as usize
}

fn height(edited: &Edited, side: u32, column: u32, row: u32) -> f64 {
    edited
        .heights
        .get(vertex(side, column, row))
        .copied()
        .unwrap_or(f64::NAN)
}

fn request<'a>(points: &'a [f64], triangles: &'a [u32], edits: &'a [Edit<'a>]) -> EditRequest<'a> {
    EditRequest {
        points,
        triangles,
        edits,
        clip: None,
        protected: &[],
        smoothing: None,
    }
}

fn point(
    position: [f64; 3],
    radius: f64,
    mode: Mode,
    effect: Effect,
    transition: Transition,
) -> Edit<'static> {
    Edit {
        target: Target::Point { position, radius },
        mode,
        effect,
        transition,
        max_distance: 100.0,
        strength: 1.0,
    }
}

const BLUR4: Transition = Transition::Falloff { blur: 4.0 };

#[test]
fn smoother_step_has_the_documented_shape() {
    assert!(close(smoother_step(0.0), 0.0, 0.0));
    assert!(close(smoother_step(0.5), 0.5, 1e-15));
    assert!(close(smoother_step(1.0), 1.0, 0.0));
    assert!(close(smoother_step(-1.0), 0.0, 0.0) && close(smoother_step(2.0), 1.0, 0.0));
    let samples: Vec<f64> = (0..=20)
        .map(|i| smoother_step(f64::from(i) / 20.0))
        .collect();
    assert!(samples.windows(2).all(|pair| pair.first() <= pair.get(1)));
}

#[test]
fn a_point_fill_is_a_smooth_cone_and_cut_cannot_raise() -> Result<(), EditError> {
    let (points, triangles) = grid(11, |_, _| 0.0);
    let fill = [point(
        [5.0, 5.0, 2.0],
        0.0,
        Mode::Absolute,
        Effect::Fill,
        BLUR4,
    )];
    let edited = edit_terrain(&request(&points, &triangles, &fill))?;
    assert!(close(height(&edited, 11, 5, 5), 2.0, 0.0));
    // d = 2 of blur 4: influence 1 - SmootherStep(0.5) = 0.5.
    assert!(close(height(&edited, 11, 7, 5), 1.0, 1e-12));
    assert!(close(height(&edited, 11, 9, 5), 0.0, 0.0)); // d = blur
    assert_eq!(edited.owners.get(vertex(11, 5, 5)), Some(&Some(0)));
    assert_eq!(edited.owners.get(vertex(11, 0, 0)), Some(&None));
    let cut = [point(
        [5.0, 5.0, 2.0],
        0.0,
        Mode::Absolute,
        Effect::Cut,
        BLUR4,
    )];
    let untouched = edit_terrain(&request(&points, &triangles, &cut))?;
    assert!(untouched.owners.iter().all(Option::is_none));
    assert_eq!(untouched.reports.first().map(|r| r.moved), Some(0));
    Ok(())
}

#[test]
fn relative_edits_displace_and_both_chooses_per_vertex() -> Result<(), EditError> {
    let (points, triangles) = grid(11, |x, _| x);
    let raise = [point(
        [5.0, 5.0, 1.5],
        1.0,
        Mode::Relative,
        Effect::Both,
        BLUR4,
    )];
    let edited = edit_terrain(&request(&points, &triangles, &raise))?;
    assert!(close(height(&edited, 11, 5, 5), 6.5, 1e-12));
    assert!(close(height(&edited, 11, 6, 5), 7.5, 1e-12)); // inside the flat radius
    // Flatten an area to its mean: the low side rises, the high side falls.
    let ring = [3.0, 3.0, 0.0, 7.0, 3.0, 0.0, 7.0, 7.0, 0.0, 3.0, 7.0, 0.0];
    let flatten = [Edit {
        target: Target::Area {
            ring: &ring,
            level: AreaLevel::Flatten,
        },
        mode: Mode::Absolute,
        effect: Effect::Both,
        transition: Transition::Falloff { blur: 2.0 },
        max_distance: 10.0,
        strength: 1.0,
    }];
    let flat = edit_terrain(&request(&points, &triangles, &flatten))?;
    // Vertices strictly inside: x = 4, 5, 6 -> mean 5.
    for column in 3..=7 {
        assert!(
            close(height(&flat, 11, column, 5), 5.0, 1e-12),
            "column {column}"
        );
    }
    assert!(close(height(&flat, 11, 1, 5), 1.0, 0.0)); // d = 2 = blur
    Ok(())
}

#[test]
fn daylight_cuts_vertically_fills_at_one_in_three_and_reports_the_dangling_edge()
-> Result<(), EditError> {
    // Low ground: a 2 m platform filled at 1:3 with a 4 m extension.
    let (low, triangles) = grid(21, |_, _| 0.0);
    let ring = [
        8.0, 8.0, 0.0, 12.0, 8.0, 0.0, 12.0, 12.0, 0.0, 8.0, 12.0, 0.0,
    ];
    let platform = |extension: Option<f64>| Edit {
        target: Target::Area {
            ring: &ring,
            level: AreaLevel::Height(2.0),
        },
        mode: Mode::Absolute,
        effect: Effect::Both,
        transition: Transition::Daylight {
            cut: Daylight {
                slope: Slope::Angle {
                    radians: std::f64::consts::FRAC_PI_2,
                },
                extension: None,
            },
            fill: Daylight {
                slope: Slope::RiseRun {
                    rise: 1.0,
                    run: 3.0,
                },
                extension,
            },
        },
        max_distance: 50.0,
        strength: 1.0,
    };
    let edits = [platform(Some(4.0))];
    let filled = edit_terrain(&request(&low, &triangles, &edits))?;
    assert!(close(height(&filled, 21, 10, 10), 2.0, 0.0));
    assert!(close(height(&filled, 21, 15, 10), 1.0, 1e-12)); // d = 3
    assert!(close(height(&filled, 21, 17, 10), 0.0, 0.0)); // d = 5 > extension
    let clipped = filled.reports.first().map_or(0, |r| r.extension_clipped);
    assert!(clipped > 0, "the terminated fill must be reported");
    // Located: one row per counted vertex, each beyond the 4 m extension, raising.
    let rows: Vec<&Clip> = filled
        .clips
        .iter()
        .filter(|c| c.kind == ClipKind::Extension)
        .collect();
    assert_eq!(rows.len(), clipped as usize);
    assert!(
        rows.iter()
            .all(|c| c.edit == 0 && c.distance > 4.0 && c.withheld > 0.0)
    );
    assert!(rows.iter().any(|c| c.vertex == 10 * 21 + 17));
    // Without a limit the same daylight meets the ground at d = 6: nothing clipped.
    let full = edit_terrain(&request(&low, &triangles, &[platform(None)]))?;
    assert!(close(height(&full, 21, 17, 10), 1.0 / 3.0, 1e-12));
    assert_eq!(full.reports.first().map(|r| r.extension_clipped), Some(0));
    // High ground: the vertical cut lowers the platform only.
    let (high, _) = grid(21, |_, _| 4.0);
    let cut = edit_terrain(&request(&high, &triangles, &[platform(None)]))?;
    assert!(close(height(&cut, 21, 10, 10), 2.0, 0.0));
    assert!(close(height(&cut, 21, 13, 10), 4.0, 0.0)); // just outside: unchanged
    Ok(())
}

#[test]
fn curves_take_the_nearest_height_and_paths_carry_crossfall() -> Result<(), EditError> {
    let (points, triangles) = grid(11, |_, _| 0.0);
    let line = [0.0, 5.0, 1.0, 10.0, 5.0, 3.0];
    let curve = [Edit {
        target: Target::Curve {
            points: &line,
            half_width: 0.0,
        },
        mode: Mode::Absolute,
        effect: Effect::Both,
        transition: Transition::Falloff { blur: 2.0 },
        max_distance: 10.0,
        strength: 1.0,
    }];
    let edited = edit_terrain(&request(&points, &triangles, &curve))?;
    assert!(close(height(&edited, 11, 5, 5), 2.0, 1e-12)); // on the curve, halfway
    assert!(close(height(&edited, 11, 5, 6), 1.0, 1e-12)); // d = 1 of 2
    let centre = [0.0, 5.0, 1.0, 10.0, 5.0, 1.0];
    let path = [Edit {
        target: Target::Path {
            centreline: &centre,
            width: 4.0,
            crossfall: Some(Crossfall {
                slope: Slope::Percent(10.0),
                falls_to: Hand::Left,
            }),
        },
        mode: Mode::Absolute,
        effect: Effect::Both,
        transition: Transition::Falloff { blur: 2.0 },
        max_distance: 10.0,
        strength: 1.0,
    }];
    let paved = edit_terrain(&request(&points, &triangles, &path))?;
    // Travelling +x, left is +y: the left edge is lower.
    assert!(close(height(&paved, 11, 5, 6), 0.9, 1e-12));
    assert!(close(height(&paved, 11, 5, 4), 1.1, 1e-12));
    assert!(close(height(&paved, 11, 5, 7), 0.8, 1e-12)); // band edge
    Ok(())
}

#[test]
fn a_surface_edit_daylights_from_its_boundary() -> Result<(), EditError> {
    let (ground, triangles) = grid(11, |_, _| 0.0);
    let (pad, pad_triangles) = grid(3, |_, _| 3.0);
    // Move the 2 x 2 m pad to [4, 6] x [4, 6].
    let moved: Vec<f64> = pad
        .as_chunks::<3>()
        .0
        .iter()
        .flat_map(|&[x, y, z]| [x + 4.0, y + 4.0, z])
        .collect();
    let field = Field::new(&moved, &pad_triangles).map_err(|_| EditError::Shape)?;
    let edits = [Edit {
        target: Target::Surface { field: &field },
        mode: Mode::Absolute,
        effect: Effect::Fill,
        transition: Transition::Daylight {
            cut: Daylight {
                slope: Slope::Percent(100.0),
                extension: None,
            },
            fill: Daylight {
                slope: Slope::Percent(100.0),
                extension: None,
            },
        },
        max_distance: 10.0,
        strength: 1.0,
    }];
    let edited = edit_terrain(&request(&ground, &triangles, &edits))?;
    assert!(close(height(&edited, 11, 5, 5), 3.0, 1e-12));
    assert!(close(height(&edited, 11, 7, 5), 2.0, 1e-12)); // 1 m outside, 45 degrees
    assert!(close(height(&edited, 11, 9, 5), 0.0, 0.0)); // met the ground at 3 m
    Ok(())
}

#[test]
fn overlapping_edits_keep_the_largest_displacement() -> Result<(), EditError> {
    let (points, triangles) = grid(11, |_, _| 0.0);
    let edits = [
        point([5.0, 5.0, 1.0], 2.0, Mode::Relative, Effect::Both, BLUR4),
        point([6.0, 5.0, -2.0], 0.0, Mode::Relative, Effect::Both, BLUR4),
    ];
    let edited = edit_terrain(&request(&points, &triangles, &edits))?;
    assert!(close(height(&edited, 11, 6, 5), -2.0, 1e-12));
    assert_eq!(edited.owners.get(vertex(11, 6, 5)), Some(&Some(1)));
    // At (3, 5), d_a = 0 (+1), d_b = 3: 2 * (1 - SmootherStep(0.75)) < 1.
    assert_eq!(edited.owners.get(vertex(11, 3, 5)), Some(&Some(0)));
    assert!(close(height(&edited, 11, 3, 5), 1.0, 1e-12));
    let moved = edited.reports.iter().map(|r| r.moved).sum::<u32>();
    assert_eq!(
        moved,
        u32::try_from(edited.owners.iter().flatten().count()).unwrap_or(0)
    );
    // Faces of moved vertices are attributed, ascending, without duplicates.
    assert!(
        edited
            .faces
            .windows(2)
            .all(|pair| pair.first() < pair.get(1))
    );
    assert!(edited.faces.iter().any(|row| row.edit == 1));
    Ok(())
}

#[test]
fn clip_and_protected_rings_freeze_vertices_and_limits_are_reported() -> Result<(), EditError> {
    let (points, triangles) = grid(11, |_, _| 0.0);
    let raise = [point(
        [5.0, 5.0, 1.0],
        10.0,
        Mode::Relative,
        Effect::Both,
        BLUR4,
    )];
    let clip = [2.0, 2.0, 0.0, 8.0, 2.0, 0.0, 8.0, 8.0, 0.0, 2.0, 8.0, 0.0];
    let keep = [4.5, 4.5, 0.0, 5.5, 4.5, 0.0, 5.5, 5.5, 0.0, 4.5, 5.5, 0.0];
    let protected = [&keep[..]];
    let edited = edit_terrain(&EditRequest {
        clip: Some(&clip),
        protected: &protected,
        ..request(&points, &triangles, &raise)
    })?;
    assert!(close(height(&edited, 11, 3, 3), 1.0, 0.0));
    assert!(close(height(&edited, 11, 2, 5), 0.0, 0.0)); // on the line
    assert!(close(height(&edited, 11, 1, 5), 0.0, 0.0)); // outside it
    assert!(close(height(&edited, 11, 5, 5), 0.0, 0.0)); // protected
    // Max distance: a point raise limited to 1 m reports what it did not reach.
    let limited = [Edit {
        max_distance: 1.0,
        ..point([5.0, 5.0, 1.0], 0.0, Mode::Relative, Effect::Both, BLUR4)
    }];
    let near = edit_terrain(&request(&points, &triangles, &limited))?;
    assert!(close(height(&near, 11, 7, 5), 0.0, 0.0));
    assert!(near.reports.first().is_some_and(|r| r.limit_clipped > 0));
    let limit = near.reports.first().map_or(0, |r| r.limit_clipped) as usize;
    assert_eq!(near.clips.len(), limit);
    assert!(
        near.clips
            .iter()
            .all(|c| c.kind == ClipKind::Limit && c.distance > 1.0 && c.withheld > 0.0)
    );
    assert!(
        near.clips
            .windows(2)
            .all(|pair| matches!(pair, [a, b] if (a.vertex, a.edit) < (b.vertex, b.edit)))
    );
    Ok(())
}

#[test]
fn smoothing_keeps_constants_spreads_a_spike_and_zero_is_identity() -> Result<(), EditError> {
    let (points, triangles) = grid(11, |_, _| 0.0);
    let all = [
        0.0, 0.0, 0.0, 10.0, 0.0, 0.0, 10.0, 10.0, 0.0, 0.0, 10.0, 0.0,
    ];
    let lift = [Edit {
        target: Target::Area {
            ring: &all,
            level: AreaLevel::Height(1.0),
        },
        mode: Mode::Relative,
        effect: Effect::Both,
        transition: BLUR4,
        max_distance: 0.0,
        strength: 1.0,
    }];
    let constant = edit_terrain(&EditRequest {
        smoothing: Some(2.0),
        ..request(&points, &triangles, &lift)
    })?;
    assert!(constant.heights.iter().all(|z| close(*z, 1.0, 1e-12)));
    let spike = [point(
        [5.0, 5.0, 3.0],
        0.0,
        Mode::Relative,
        Effect::Both,
        Transition::Falloff { blur: 0.5 },
    )];
    let raw = edit_terrain(&request(&points, &triangles, &spike))?;
    assert_eq!(raw.owners.iter().flatten().count(), 1);
    let smoothed = edit_terrain(&EditRequest {
        smoothing: Some(1.5),
        ..request(&points, &triangles, &spike)
    })?;
    let at = |column, row| height(&smoothed, 11, column, row);
    assert!(at(5, 5) < 3.0 && at(5, 5) > at(6, 5));
    assert!(
        close(at(6, 5), at(4, 5), 1e-12)
            && close(at(6, 5), at(5, 6), 1e-12)
            && close(at(5, 4), at(6, 5), 1e-12)
    );
    assert!(at(6, 5) > 0.0);
    assert_eq!(smoothed.owners.get(vertex(11, 6, 5)), Some(&Some(0)));
    let identity = edit_terrain(&EditRequest {
        smoothing: Some(0.0),
        ..request(&points, &triangles, &spike)
    })?;
    assert_eq!(identity, raw);
    Ok(())
}

#[test]
fn malformed_edits_are_refused_by_name() {
    let (points, triangles) = grid(3, |_, _| 0.0);
    let run = |edit: Edit<'_>| edit_terrain(&request(&points, &triangles, &[edit])).map(|_| ());
    let base = point([1.0, 1.0, 1.0], 0.0, Mode::Absolute, Effect::Both, BLUR4);
    assert_eq!(
        run(Edit {
            transition: Transition::Falloff { blur: 0.0 },
            ..base
        }),
        Err(EditError::Parameters)
    );
    assert_eq!(
        run(Edit {
            strength: 0.0,
            ..base
        }),
        Err(EditError::Parameters)
    );
    assert_eq!(
        run(Edit {
            strength: 1.5,
            ..base
        }),
        Err(EditError::Parameters)
    );
    assert_eq!(
        run(Edit {
            max_distance: f64::NAN,
            ..base
        }),
        Err(EditError::NonFinite)
    );
    let daylight = Transition::Daylight {
        cut: Daylight {
            slope: Slope::Percent(50.0),
            extension: None,
        },
        fill: Daylight {
            slope: Slope::Percent(-1.0),
            extension: None,
        },
    };
    assert_eq!(
        run(Edit {
            transition: daylight,
            ..base
        }),
        Err(EditError::Slope)
    );
    assert_eq!(
        run(Edit {
            mode: Mode::Relative,
            transition: daylight,
            ..base
        }),
        Err(EditError::Parameters)
    );
    let short = [0.0, 0.0, 0.0];
    assert_eq!(
        run(Edit {
            target: Target::Curve {
                points: &short,
                half_width: 0.0
            },
            ..base
        }),
        Err(EditError::Shape)
    );
    assert_eq!(
        edit_terrain(&request(&points, &[0, 1, 9], &[])).map(|_| ()),
        Err(EditError::Index)
    );
    for error in [
        EditError::Shape,
        EditError::NonFinite,
        EditError::Index,
        EditError::Parameters,
        EditError::Slope,
    ] {
        assert!(error.message().starts_with("Edit"));
    }
}
