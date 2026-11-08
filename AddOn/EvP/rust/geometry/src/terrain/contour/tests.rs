//! #175: the grid contour tracer on fields whose contours are known exactly.

use super::*;

/// A plane rising 0.1 per metre along x: every level is one straight line across the box at
/// `x = level / 0.1`, exact under bilinear interpolation. The top level is where nothing is
/// above it, so it has no line; the lowest bounds everything above it, at the box's edge.
#[test]
fn a_plane_has_one_straight_line_per_level() {
    let traced = trace([0.0, 0.0], [10.0, 6.0], 0.25, 0.2, &|x, _| Some(0.1 * x));
    let levels: Vec<f64> = traced.lines.iter().map(|line| line.level).collect();
    assert_eq!(levels.len(), 5, "{levels:?}");
    for line in &traced.lines {
        assert!(!line.closed);
        // Straight, so simplified to its two ends, across the whole box.
        assert_eq!(line.points.len(), 2, "{:?}", line.points);
        for p in &line.points {
            assert!((p[0] - line.level / 0.1).abs() < 1e-9, "{p:?}");
            assert_eq!(p[2].to_bits(), line.level.to_bits());
        }
        let ys: Vec<f64> = line.points.iter().map(|p| p[1]).collect();
        assert!(
            ys.contains(&0.0) && ys.iter().any(|&y| (y - 6.0).abs() < 1e-9),
            "{ys:?}"
        );
    }
}

/// A cone: each level inside the box is one closed loop, a circle within the grid's chord error;
/// the levels past the box's half-width are its four corner arcs, open.
#[test]
fn a_cone_has_closed_circles() {
    let traced = trace([-5.5, -5.5], [5.5, 5.5], 0.1, 1.0, &|x, y| Some(x.hypot(y)));
    assert_eq!(traced.interval, 1.0);
    let radii: Vec<f64> = traced.lines.iter().map(|line| line.level).collect();
    // Level 0 is the apex itself, a node exactly on it: a tiny loop around what rises above it.
    assert_eq!(
        radii,
        [
            0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 6.0, 6.0, 6.0, 7.0, 7.0, 7.0, 7.0
        ]
    );
    for line in &traced.lines {
        assert_eq!(line.closed, line.level < 5.5, "level {}", line.level);
        for p in &line.points {
            let tolerance = if line.level == 0.0 { 0.1 } else { 0.01 };
            assert!((p[0].hypot(p[1]) - line.level).abs() < tolerance, "{p:?}");
        }
    }
}

/// Where there is no ground the lines stop at its edge, open.
#[test]
fn lines_stop_where_the_ground_ends() {
    let traced = trace([-5.0, -5.0], [5.0, 5.0], 0.1, 1.0, &|x, y| {
        (x > 0.0).then(|| x.hypot(y))
    });
    assert_ne!(traced.lines.len(), 0);
    for line in &traced.lines {
        assert!(!line.closed);
        assert!(line.points.iter().all(|p| p[0] >= -1e-9));
    }
}

/// No interval asked: a round one giving about ten levels over the range.
#[test]
fn an_interval_is_chosen_when_none_is_asked() {
    let traced = trace([0.0, 0.0], [10.0, 10.0], 0.5, 0.0, &|x, _| Some(3.0 * x));
    assert_eq!(traced.interval, 2.5);
    assert_eq!(traced.range, [0.0, 30.0]);
    assert_eq!(
        trace([0.0, 0.0], [1.0, 1.0], 0.5, 0.0, &|_, _| None)
            .lines
            .len(),
        0
    );
}

/// A large box is sampled coarser rather than without bound.
#[test]
fn a_large_box_is_sampled_coarser() {
    let traced = trace([0.0, 0.0], [10_000.0, 10_000.0], 0.1, 1.0, &|x, _| {
        Some(x / 1000.0)
    });
    assert!(traced.step > 4.0 && traced.step < 6.0, "{}", traced.step);
    assert_eq!(traced.lines.len(), 10);
}

type Exact<'a> = &'a (dyn Fn(f64, f64) -> f64 + Sync);

/// Every node is the field at that node, on a crease (a toe) and on a cone alike.
#[test]
fn every_node_is_the_field_there() -> Result<(), String> {
    let fields: [(&str, Exact<'_>); 3] = [
        ("plane", &|x, y| 0.1f64.mul_add(x, 0.05 * y)),
        ("crease", &|x, y| 0.5 * (x - 13.37 - 0.3 * y).abs()),
        ("cone", &|x, y| x.hypot(y) * 0.5),
    ];
    for (name, field) in fields {
        let grid = Grid::sample([-20.0, -15.0], [20.0, 15.0], 0.25, &|x, y| {
            Some([field(x, y), 0.0])
        })
        .ok_or_else(|| format!("{name}: no grid"))?;
        let mut worst = 0.0_f64;
        for row in 0..grid.rows {
            for column in 0..grid.columns {
                let [x, y] = grid.node(column, row);
                worst = worst.max((grid.value(column, row) - field(x, y)).abs());
            }
        }
        assert!(worst <= 5e-3, "{name}: worst {worst}");
    }
    Ok(())
}
