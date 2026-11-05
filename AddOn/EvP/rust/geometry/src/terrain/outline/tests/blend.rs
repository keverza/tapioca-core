//! #149: faces meeting at a crease round over the section's rounding.

use super::*;

/// The largest second difference of the height across the inside corner's
/// bisector, `d` metres out from the corner at (16, 2), over steps of `e`
/// within two metres either side: a contour's corner, wherever the crease
/// lies (leg B falls, so it is off the bisector).
fn kink(rounding: f64, d: f64, e: f64) -> Option<f64> {
    let s = std::f64::consts::FRAC_1_SQRT_2;
    let (cx, cy) = (16.0 - d * s, 2.0 + d * s);
    let at = |t: f64| inside_corner_rounded(t.mul_add(s, cx), t.mul_add(s, cy), rounding);
    let mut worst = 0.0_f64;
    let mut t = -2.0;
    while t <= 2.0 {
        worst = worst.max(((at(t + e)? + at(t - e)? - 2.0 * at(t)?) / e).abs());
        t += e;
    }
    Some(worst)
}

#[test]
fn an_inside_corner_rounds_its_crease_and_keeps_the_grade() -> Result<(), OutlineError> {
    // Past the rounded tops the sharp envelope has a crease near the
    // bisector: a slope change over a step (how much of it the grid catches
    // depends on where the step falls). Rounded, it is a curvature, so half
    // the step halves it.
    for d in [2.5, 3.0, 3.5] {
        let [sharp, round, round_finer] = [(0.0, 0.02), (1.0, 0.02), (1.0, 0.01)]
            .map(|(rounding, e)| kink(rounding, d, e).unwrap_or(f64::NAN));
        assert!(sharp > 0.2, "{d}: {sharp}");
        assert!(
            round < 0.25 * sharp && round_finer < 0.6 * round,
            "{d}: {round} {round_finer}, sharp {sharp}"
        );
    }
    // Never steeper than the rounded top's 4/3 of the 1 in 2 grade, and
    // continuous everywhere around the corner.
    let step = 0.05;
    for i in 0..120 {
        for j in 0..120 {
            let (x, y) = (
                f64::from(i).mul_add(step, 10.0),
                f64::from(j).mul_add(step, 2.5),
            );
            let here = inside_corner_rounded(x, y, 1.0).ok_or(OutlineError::Coverage)?;
            for (nx, ny) in [(x + step, y), (x, y + step)] {
                let next = inside_corner_rounded(nx, ny, 1.0).ok_or(OutlineError::Coverage)?;
                assert!(
                    (next - here).abs() <= (0.5 * 4.0 / 3.0 * 1.05f64).mul_add(step, 1e-9),
                    "({x},{y}) -> ({nx},{ny}): {here} -> {next}"
                );
            }
        }
    }
    Ok(())
}

#[test]
fn a_straight_edge_in_pieces_grades_as_one_edge() -> Result<(), OutlineError> {
    // The pavement's long side in four collinear pieces: the faces tie along
    // the normals through the joints, where a lift without the crease weight
    // would raise a bump at each one.
    let whole = (PAVED.to_vec(), PAVED_TRIANGLES.to_vec());
    let mut pieces = PAVED.to_vec();
    for x in [10.0, 20.0, 30.0] {
        pieces.extend([x, -2.0, 12.0]);
    }
    // 0 (0,-2), 4 (10,-2), 5 (20,-2), 6 (30,-2), 1 (40,-2), 2 (40,2), 3 (0,2).
    let split = (pieces, vec![0, 4, 3, 4, 5, 3, 5, 6, 3, 6, 1, 3, 1, 2, 3]);
    let (points, triangles) = base();
    let ground = Field::new(&points, &triangles).map_err(|_| OutlineError::Field)?;
    let grading = Grading {
        cut: HALF,
        fill: HALF,
        limit: 10.0,
        rounding: 1.0,
        effect: Effect::Both,
        tolerance: 1e-6,
    };
    let uniform = SurfaceGrade {
        cut: 0.5,
        fill: 0.5,
        rounding: 1.0,
    };
    let heights = |(surface, faces): &(Vec<f64>, Vec<u32>)| -> Result<Vec<f64>, OutlineError> {
        let paved = Field::new(surface, faces).map_err(|_| OutlineError::Surface)?;
        let edges = outline(surface, faces, &[], uniform);
        let graded = Faces::new(&ground, &paved, &edges, grading);
        let mut out = Vec::new();
        for x in [9.7, 10.0, 10.3, 19.9, 20.0, 29.5, 30.0] {
            for y in [-2.5, -3.0, -4.0, -5.0] {
                out.push(graded.height(x, y).ok_or(OutlineError::Coverage)?.0);
            }
        }
        Ok(out)
    };
    let (a, b) = (heights(&whole)?, heights(&split)?);
    for (p, q) in a.iter().zip(&b) {
        assert!((p - q).abs() < 1e-9, "{p} vs {q}");
    }
    Ok(())
}
