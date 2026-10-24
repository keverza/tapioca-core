use super::*;

const HALF: Slope = Slope::RiseRun {
    rise: 1.0,
    run: 2.0,
};

/// A 5-m grid over `[-20.5, 59.5] x [-30.5, 29.5]` at 10 m.
fn base() -> (Vec<f64>, Vec<u32>) {
    let (columns, rows) = (17_u32, 13_u32);
    let mut points = Vec::new();
    for row in 0..rows {
        for column in 0..columns {
            points.extend([
                -20.5 + 5.0 * f64::from(column),
                -30.5 + 5.0 * f64::from(row),
                10.0,
            ]);
        }
    }
    let mut triangles = Vec::new();
    for row in 0..rows - 1 {
        for column in 0..columns - 1 {
            let a = row * columns + column;
            triangles.extend([a, a + 1, a + columns + 1, a, a + columns + 1, a + columns]);
        }
    }
    (points, triangles)
}

const REGION: [f64; 12] = [
    -10.0, -15.0, 0.0, 50.0, -15.0, 0.0, 50.0, 15.0, 0.0, -10.0, 15.0, 0.0,
];
/// A level 40 x 4 m pavement at 12 m: 2 m above the ground.
const PAVED: [f64; 12] = [
    0.0, -2.0, 12.0, 40.0, -2.0, 12.0, 40.0, 2.0, 12.0, 0.0, 2.0, 12.0,
];
const PAVED_TRIANGLES: [u32; 6] = [0, 1, 2, 0, 2, 3];

/// The rectangle grown by `d` as two open halves (a breakline is an open line).
fn ring(d: f64) -> [Vec<f64>; 2] {
    let (x0, y0, x1, y1) = (-d, -2.0 - d, 40.0 + d, 2.0 + d);
    [
        vec![x0, y0, 0.0, x1, y0, 0.0, x1, y1, 0.0],
        vec![x1, y1, 0.0, x0, y1, 0.0, x0, y0, 0.0],
    ]
}

fn run(rounding: f64, limit: f64) -> Result<Outlined, OutlineError> {
    let (points, triangles) = base();
    let rings: Vec<Vec<f64>> = [0.0, 0.25, 0.5, 0.75, 1.0, 2.0, 3.0, 4.0, 5.0]
        .into_iter()
        .flat_map(ring)
        .collect();
    let lines: Vec<&[f64]> = rings.iter().map(Vec::as_slice).collect();
    build(&OutlineRequest {
        points: &points,
        triangles: &triangles,
        region: &REGION,
        surface_points: &PAVED,
        surface_triangles: &PAVED_TRIANGLES,
        breaklines: &lines,
        spots: &[],
        grading: Grading {
            cut: HALF,
            fill: HALF,
            limit,
            rounding,
            effect: Effect::Both,
            tolerance: 1e-6,
        },
    })
}

/// The proposed height at the output vertex nearest `(x, y)`.
fn at(built: &Outlined, x: f64, y: f64) -> f64 {
    plan::vertices(&built.points)
        .iter()
        .min_by(|a, b| {
            (a[0] - x)
                .hypot(a[1] - y)
                .total_cmp(&(b[0] - x).hypot(b[1] - y))
        })
        .map_or(f64::NAN, |p| p[2])
}

#[test]
fn the_pavement_is_kept_and_its_faces_fall_to_the_ground() -> Result<(), OutlineError> {
    let built = run(0.0, 10.0)?;
    // On the pavement, its own height; its fill face falls 1 in 2 to meet the
    // ground 4 m out, and the ground stays beyond.
    assert!((at(&built, 20.0, 2.0) - 12.0).abs() < 1e-9);
    assert!((at(&built, 20.0, 3.0) - 11.5).abs() < 1e-9);
    assert!((at(&built, 20.0, 4.0) - 11.0).abs() < 1e-9);
    assert!((at(&built, 20.0, 6.0) - 10.0).abs() < 1e-9);
    assert!((at(&built, 20.0, 7.0) - 10.0).abs() < 1e-9);
    // Around the ends too: the area's own outline, not a band that stops.
    assert!((at(&built, 42.0, 0.0) - 11.0).abs() < 1e-9);
    assert!((at(&built, -2.0, 0.0) - 11.0).abs() < 1e-9);
    assert_eq!(built.unreached, 0);
    assert!(built.volumes.site.fill > 0.0);
    assert!(built.volumes.site.cut.abs() < 1e-9);
    assert!(built.volumes.context.fill.abs() < 1e-12);
    Ok(())
}

#[test]
fn rounding_eases_the_top_and_keeps_the_straight_face() -> Result<(), OutlineError> {
    let built = run(1.0, 10.0)?;
    // A quarter metre out the rounded top has barely left the pavement's level
    // (the sharp face would be 0.125 m down); the straight face is unchanged.
    let quarter = at(&built, 20.0, 2.25);
    assert!(quarter < 12.0 && 12.0 - quarter < 0.06, "{quarter}");
    assert!((at(&built, 20.0, 4.0) - 11.0).abs() < 1e-9);
    Ok(())
}

#[test]
fn a_face_cut_short_by_its_limit_is_counted() -> Result<(), OutlineError> {
    let built = run(0.0, 1.0)?;
    assert!(built.unreached > 0);
    Ok(())
}
