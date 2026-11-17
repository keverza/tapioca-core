//! The built terrain at a level of detail: the features exact, every built vertex within the
//! tolerance, and far fewer triangles where the shape allows.

use super::*;

/// An `n` x `n` grid of 1 m cells, z from `height`.
fn grid(n: u32, height: impl Fn(f64, f64) -> f64) -> (Vec<f64>, Vec<u32>) {
    let mut points = Vec::new();
    for j in 0..=n {
        for i in 0..=n {
            let (x, y) = (f64::from(i), f64::from(j));
            points.extend([x, y, height(x, y)]);
        }
    }
    let mut triangles = Vec::new();
    let at = |i: u32, j: u32| j * (n + 1) + i;
    for j in 0..n {
        for i in 0..n {
            triangles.extend([at(i, j), at(i + 1, j), at(i + 1, j + 1)]);
            triangles.extend([at(i, j), at(i + 1, j + 1), at(i, j + 1)]);
        }
    }
    (points, triangles)
}

/// The worst height difference at the built vertices; `None` where the result does not cover one.
fn worst_error(points: &[f64], out: &Detailed) -> Option<f64> {
    let field = Field::new(&out.points, &out.triangles).ok()?;
    let mut worst = 0.0_f64;
    for &[x, y, z] in points.as_chunks::<3>().0 {
        worst = worst.max((field.height_at(x, y)?.height - z).abs());
    }
    Some(worst)
}

/// The plan centroids of the result's triangles.
fn centroids(out: &Detailed) -> Vec<[f64; 2]> {
    let vertices = out.points.as_chunks::<3>().0;
    out.triangles
        .as_chunks::<3>()
        .0
        .iter()
        .filter_map(|corners| {
            let [a, b, c] = corners.map(|v| vertices.get(v as usize).copied());
            let ([ax, ay, _], [bx, by, _], [cx, cy, _]) = (a?, b?, c?);
            Some([(ax + bx + cx) / 3.0, (ay + by + cy) / 3.0])
        })
        .collect()
}

fn request<'a>(
    points: &'a [f64],
    triangles: &'a [u32],
    keep: &'a [bool],
    lines: &'a [[u32; 2]],
    tolerance: f64,
) -> DetailRequest<'a> {
    DetailRequest {
        points,
        triangles,
        keep,
        lines,
        tolerance,
    }
}

#[test]
fn a_plane_is_its_four_corners() -> Result<(), DetailError> {
    let (points, triangles) = grid(20, |x, y| 0.1f64.mul_add(x, -0.05 * y) + 3.0);
    let out = simplify(&request(&points, &triangles, &[], &[], 0.01))?;
    assert_eq!(
        out.points.len() / 3,
        4,
        "a straight boundary keeps its corners only"
    );
    assert_eq!(out.triangles.len() / 3, 2);
    assert!(worst_error(&points, &out).is_some_and(|e| e < 1e-9));
    Ok(())
}

#[test]
fn a_hill_is_within_the_tolerance_with_fewer_triangles() -> Result<(), DetailError> {
    let hill = |x: f64, y: f64| 0.02f64.mul_add(-((x - 15.0).powi(2) + (y - 15.0).powi(2)), 5.0);
    let (points, triangles) = grid(30, hill);
    for tolerance in [0.01, 0.05, 0.2] {
        let out = simplify(&request(&points, &triangles, &[], &[], tolerance))?;
        let error = worst_error(&points, &out);
        assert!(
            error.is_some_and(|e| e <= tolerance + 1e-9),
            "{tolerance}: {error:?}"
        );
        assert!(out.error <= tolerance + 1e-9);
        // At 1 cm a 1 m grid of this curvature (its own chord error is 5 mm) needs most of its
        // points; coarser, far fewer. Its curved boundary stays to a millimetre either way.
        let most = if tolerance < 0.02 {
            triangles.len()
        } else {
            triangles.len() / 2
        };
        assert!(
            out.triangles.len() <= most,
            "{tolerance}: {}",
            out.triangles.len() / 3
        );
    }
    Ok(())
}

#[test]
fn kept_points_and_lines_stay_exact() -> Result<(), DetailError> {
    let (points, triangles) = grid(10, |_, _| 1.0);
    // A kept point and a kept line across a level grid: nothing else would keep them.
    let mut keep = vec![false; points.len() / 3];
    if let Some(flag) = keep.get_mut(5 * 11 + 3) {
        *flag = true;
    }
    // y = 7, x 2..8: a straight line given as its six edges.
    let line: Vec<[u32; 2]> = (2..8).map(|i| [7 * 11 + i, 7 * 11 + i + 1]).collect();
    let out = simplify(&request(&points, &triangles, &keep, &line, 0.05))?;
    assert!(out.source.contains(&(5 * 11 + 3)), "the kept point");
    assert!(
        out.source.contains(&(7 * 11 + 2)) && out.source.contains(&(7 * 11 + 8)),
        "the line's ends"
    );
    assert!(
        !out.source.contains(&(7 * 11 + 5)),
        "a straight line's middle is not needed"
    );
    let ends = [7 * 11 + 2, 7 * 11 + 8];
    let built = |v: u32| out.source.get(v as usize).copied();
    assert!(
        out.lines.iter().any(|&[a, b]| {
            let edge = [built(a), built(b)];
            edge == [Some(ends[0]), Some(ends[1])] || edge == [Some(ends[1]), Some(ends[0])]
        }),
        "the line is an edge"
    );
    Ok(())
}

#[test]
fn a_notch_gets_no_triangle() -> Result<(), DetailError> {
    // An L: the grid without its top-right quarter.
    let (points, all) = grid(10, |x, y| 0.03 * x * y);
    let vertices = points.as_chunks::<3>().0;
    let triangles: Vec<u32> = all
        .as_chunks::<3>()
        .0
        .iter()
        .filter(|corners| {
            let plan = corners.map(|v| vertices.get(v as usize).copied().unwrap_or_default());
            let cx = plan.iter().map(|p| p[0]).sum::<f64>() / 3.0;
            let cy = plan.iter().map(|p| p[1]).sum::<f64>() / 3.0;
            !(cx > 5.0 && cy > 5.0)
        })
        .flatten()
        .copied()
        .collect();
    let out = simplify(&request(&points, &triangles, &[], &[], 0.02))?;
    for [cx, cy] in centroids(&out) {
        assert!(
            !(cx > 5.0 && cy > 5.0),
            "a triangle over the notch at {cx},{cy}"
        );
    }
    let field = Field::new(&out.points, &out.triangles).map_err(|_| DetailError::Field)?;
    assert!(field.height_at(2.0, 8.0).is_some() && field.height_at(8.0, 2.0).is_some());
    assert!(field.height_at(8.0, 8.0).is_none(), "the notch stays open");
    Ok(())
}

#[test]
fn a_bad_request_is_refused() {
    let (points, triangles) = grid(2, |_, _| 0.0);
    for tolerance in [0.0, -1.0, f64::NAN, f64::INFINITY] {
        assert_eq!(
            simplify(&request(&points, &triangles, &[], &[], tolerance)).err(),
            Some(DetailError::Parameters)
        );
    }
    assert_eq!(
        simplify(&request(&points, &triangles, &[], &[[0, 99]], 0.01)).err(),
        Some(DetailError::Parameters),
        "a line to no point"
    );
    assert_eq!(
        simplify(&request(&points, &triangles, &[true], &[], 0.01)).err(),
        Some(DetailError::Parameters),
        "a keep list of the wrong length"
    );
}
