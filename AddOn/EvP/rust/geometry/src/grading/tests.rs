use super::*;
use crate::slope::Slope;

fn close(left: f64, right: f64, tolerance: f64) -> bool {
    (left - right).abs() <= tolerance
}

/// Least-squares rows `x_a - x_b = 0` (weight 1) over a list of pairs.
struct Smooth {
    terms: Vec<Term>,
    rows: Vec<Row>,
}

impl Smooth {
    fn new(pairs: &[(u32, u32)]) -> Self {
        let mut terms = Vec::new();
        let mut rows = Vec::new();
        for (index, &(a, b)) in pairs.iter().enumerate() {
            let first = u32::try_from(index * 2).unwrap_or(u32::MAX);
            terms.push(Term {
                unknown: a,
                coefficient: 1.0,
            });
            terms.push(Term {
                unknown: b,
                coefficient: -1.0,
            });
            rows.push(Row {
                first,
                count: 2,
                target: 0.0,
                weight: 1.0,
            });
        }
        Self { terms, rows }
    }

    fn problem<'a>(&'a self, unknowns: u32, fixed: &'a [Fixed]) -> Problem<'a> {
        Problem {
            unknowns,
            terms: &self.terms,
            rows: &self.rows,
            fixed,
        }
    }
}

/// Grid edges of a `columns x rows` lattice, node `r * columns + c`.
fn lattice(columns: u32, rows: u32) -> Vec<(u32, u32)> {
    let mut pairs = Vec::new();
    for r in 0..rows {
        for c in 0..columns {
            let node = r * columns + c;
            if c + 1 < columns {
                pairs.push((node, node + 1));
            }
            if r + 1 < rows {
                pairs.push((node, node + columns));
            }
        }
    }
    pairs
}

/// Anchor rows `x_i = target_i` (weight 1).
fn anchors(targets: &[f64]) -> (Vec<Term>, Vec<Row>) {
    let terms = (0..targets.len())
        .map(|i| Term {
            unknown: u32::try_from(i).unwrap_or(u32::MAX),
            coefficient: 1.0,
        })
        .collect();
    let rows = targets
        .iter()
        .enumerate()
        .map(|(i, &target)| Row {
            first: u32::try_from(i).unwrap_or(u32::MAX),
            count: 1,
            target,
            weight: 1.0,
        })
        .collect();
    (terms, rows)
}

#[test]
fn fixed_perimeter_and_ridge_reproduce_the_analytic_transition() -> Result<(), Error> {
    // 9 x 5 lattice: columns 0 and 8 fixed at 0, column 4 is a ridge at 2 m. With
    // free top/bottom rows the discrete harmonic field is the tent 2 * (1 - |c - 4| / 4).
    let (columns, rows) = (9_u32, 5_u32);
    let smooth = Smooth::new(&lattice(columns, rows));
    let mut fixed = Vec::new();
    for r in 0..rows {
        fixed.push(Fixed {
            unknown: r * columns,
            value: 0.0,
        });
        fixed.push(Fixed {
            unknown: r * columns + 8,
            value: 0.0,
        });
        fixed.push(Fixed {
            unknown: r * columns + 4,
            value: 2.0,
        });
    }
    let solution = solve_sparse_ls(&smooth.problem(columns * rows, &fixed), Limits::default())?;
    for (node, value) in solution.values.iter().enumerate() {
        let c = f64::from(u32::try_from(node).unwrap_or(0) % columns);
        let expected = 2.0 * (1.0 - (c - 4.0).abs() / 4.0);
        assert!(
            close(*value, expected, 1e-10),
            "node {node}: {value} vs {expected}"
        );
    }
    assert!(solution.objective > 0.0);
    assert_eq!(solution.method, METHOD);
    assert_eq!(solution.factorizations, 1);
    Ok(())
}

#[test]
fn reordering_the_unknowns_gives_the_same_field() -> Result<(), Error> {
    let (columns, rows) = (7_u32, 6_u32);
    let n = columns * rows;
    let pairs = lattice(columns, rows);
    let fixed_at = |node: u32| match node {
        0 => Some(1.0),
        n if n == columns * rows - 1 => Some(-2.0),
        n if n == 3 * columns + 3 => Some(4.0),
        _ => None,
    };
    let fixed: Vec<Fixed> = (0..n)
        .filter_map(|node| {
            fixed_at(node).map(|value| Fixed {
                unknown: node,
                value,
            })
        })
        .collect();
    let base = solve_sparse_ls(&Smooth::new(&pairs).problem(n, &fixed), Limits::default())?;
    // Reverse every index, and the row order.
    let flip = |node: u32| n - 1 - node;
    let mut flipped_pairs: Vec<(u32, u32)> =
        pairs.iter().map(|&(a, b)| (flip(b), flip(a))).collect();
    flipped_pairs.reverse();
    let flipped_fixed: Vec<Fixed> = fixed
        .iter()
        .rev()
        .map(|f| Fixed {
            unknown: flip(f.unknown),
            value: f.value,
        })
        .collect();
    let flipped = solve_sparse_ls(
        &Smooth::new(&flipped_pairs).problem(n, &flipped_fixed),
        Limits::default(),
    )?;
    for node in 0..n {
        let a = base.values.get(node as usize).copied().unwrap_or(f64::NAN);
        let b = flipped
            .values
            .get(flip(node) as usize)
            .copied()
            .unwrap_or(f64::NAN);
        assert!(close(a, b, 1e-11), "node {node}: {a} vs {b}");
    }
    Ok(())
}

#[test]
fn a_fixed_solve_is_bit_reproducible() -> Result<(), Error> {
    let smooth = Smooth::new(&lattice(12, 9));
    let fixed = [
        Fixed {
            unknown: 0,
            value: 0.25,
        },
        Fixed {
            unknown: 107,
            value: -1.5,
        },
        Fixed {
            unknown: 50,
            value: 3.0,
        },
    ];
    let first = solve_sparse_ls(&smooth.problem(108, &fixed), Limits::default())?;
    let second = solve_sparse_ls(&smooth.problem(108, &fixed), Limits::default())?;
    let bits = |values: &[f64]| values.iter().map(|v| v.to_bits()).collect::<Vec<_>>();
    assert_eq!(bits(&first.values), bits(&second.values));
    Ok(())
}

#[test]
fn contradictory_fixed_heights_are_inconsistent() {
    let smooth = Smooth::new(&[(0, 1)]);
    let fixed = [
        Fixed {
            unknown: 1,
            value: 2.0,
        },
        Fixed {
            unknown: 1,
            value: 2.5,
        },
    ];
    assert_eq!(
        solve_sparse_ls(&smooth.problem(2, &fixed), Limits::default()),
        Err(Error::SolveInconsistent)
    );
    let agreeing = [
        Fixed {
            unknown: 1,
            value: 2.0,
        },
        Fixed {
            unknown: 1,
            value: 2.0 + 1e-12,
        },
    ];
    assert!(solve_sparse_ls(&smooth.problem(2, &agreeing), Limits::default()).is_ok());
}

#[test]
fn a_free_unknown_without_rows_is_underdetermined() {
    let smooth = Smooth::new(&[(0, 1)]);
    let fixed = [Fixed {
        unknown: 0,
        value: 1.0,
    }];
    assert_eq!(
        solve_sparse_ls(&smooth.problem(3, &fixed), Limits::default()),
        Err(Error::SolveUnderdetermined)
    );
    // Only differences, nothing anchors the level.
    assert_eq!(
        solve_sparse_ls(&smooth.problem(2, &[]), Limits::default()),
        Err(Error::SolveUnderdetermined)
    );
}

#[test]
fn bounds_clamp_an_anchored_field_exactly() -> Result<(), Error> {
    let targets = [-3.0, -0.5, 0.0, 0.7, 4.0];
    let (terms, rows) = anchors(&targets);
    let problem = Problem {
        unknowns: 5,
        terms: &terms,
        rows: &rows,
        fixed: &[],
    };
    let bounds: Vec<Bound> = (0..5)
        .map(|unknown| Bound {
            unknown,
            lower: -1.0,
            upper: 1.0,
        })
        .collect();
    let constraints = Constraints {
        bounds: &bounds,
        differences: &[],
    };
    let solution = solve_constrained_ls(&problem, constraints, Limits::default(), None)?;
    for (value, target) in solution.values.iter().zip(targets) {
        assert!(close(*value, target.clamp(-1.0, 1.0), 1e-12));
    }
    assert_eq!(solution.active, 2);
    Ok(())
}

#[test]
fn a_bound_transmits_through_smoothness() -> Result<(), Error> {
    // Chain 0-1-2 with x0 fixed 0 and a strong pull of x2 toward 6; x1 <= 1.
    // Optimum: x1 = 1 (bound active), x2 minimizes (x2 - x1)^2 + 4 (x2 - 6)^2 => 5.
    let terms = [
        Term {
            unknown: 1,
            coefficient: 1.0,
        },
        Term {
            unknown: 0,
            coefficient: -1.0,
        },
        Term {
            unknown: 2,
            coefficient: 1.0,
        },
        Term {
            unknown: 1,
            coefficient: -1.0,
        },
        Term {
            unknown: 2,
            coefficient: 1.0,
        },
    ];
    let rows = [
        Row {
            first: 0,
            count: 2,
            target: 0.0,
            weight: 1.0,
        },
        Row {
            first: 2,
            count: 2,
            target: 0.0,
            weight: 1.0,
        },
        Row {
            first: 4,
            count: 1,
            target: 6.0,
            weight: 4.0,
        },
    ];
    let fixed = [Fixed {
        unknown: 0,
        value: 0.0,
    }];
    let problem = Problem {
        unknowns: 3,
        terms: &terms,
        rows: &rows,
        fixed: &fixed,
    };
    let bounds = [Bound {
        unknown: 1,
        lower: f64::NEG_INFINITY,
        upper: 1.0,
    }];
    let constraints = Constraints {
        bounds: &bounds,
        differences: &[],
    };
    let solution = solve_constrained_ls(&problem, constraints, Limits::default(), None)?;
    assert!(close(
        solution.values.get(1).copied().unwrap_or(f64::NAN),
        1.0,
        1e-10
    ));
    assert!(close(
        solution.values.get(2).copied().unwrap_or(f64::NAN),
        5.0,
        1e-10
    ));
    assert_eq!(solution.active, 1);
    Ok(())
}

#[test]
fn a_released_constraint_does_not_stay_binding() -> Result<(), Error> {
    // Start at the upper bound; the optimum is interior, so the start's tight
    // bound must be released by its multiplier.
    let (terms, rows) = anchors(&[0.25]);
    let problem = Problem {
        unknowns: 1,
        terms: &terms,
        rows: &rows,
        fixed: &[],
    };
    let bounds = [Bound {
        unknown: 0,
        lower: -1.0,
        upper: 1.0,
    }];
    let constraints = Constraints {
        bounds: &bounds,
        differences: &[],
    };
    let solution = solve_constrained_ls(&problem, constraints, Limits::default(), Some(&[1.0]))?;
    assert!(close(
        solution.values.first().copied().unwrap_or(f64::NAN),
        0.25,
        1e-12
    ));
    assert_eq!(solution.active, 0);
    Ok(())
}

#[test]
fn infeasible_limits_are_inconsistent() {
    let (terms, rows) = anchors(&[0.0, 0.0]);
    let problem = Problem {
        unknowns: 2,
        terms: &terms,
        rows: &rows,
        fixed: &[],
    };
    let bounds = [
        Bound {
            unknown: 0,
            lower: f64::NEG_INFINITY,
            upper: 0.0,
        },
        Bound {
            unknown: 1,
            lower: 5.0,
            upper: f64::INFINITY,
        },
    ];
    let differences = [Difference {
        a: 1,
        b: 0,
        min: -1.0,
        max: 1.0,
    }];
    let constraints = Constraints {
        bounds: &bounds,
        differences: &differences,
    };
    assert_eq!(
        solve_constrained_ls(&problem, constraints, Limits::default(), None),
        Err(Error::SolveInconsistent)
    );
    // A fixed value outside its own bound is the same contradiction.
    let fixed = [Fixed {
        unknown: 0,
        value: 2.0,
    }];
    let pinned = Problem {
        fixed: &fixed,
        ..problem
    };
    let only_bound = Constraints {
        bounds: bounds.get(..1).unwrap_or(&[]),
        differences: &[],
    };
    assert_eq!(
        solve_constrained_ls(&pinned, only_bound, Limits::default(), None),
        Err(Error::SolveInconsistent)
    );
}

#[test]
fn an_exhausted_budget_is_not_converged_and_returns_nothing() {
    // Five anchors outside their bounds need several blocking steps.
    let targets = [-3.0, -2.0, 0.0, 2.0, 3.0];
    let (terms, rows) = anchors(&targets);
    let problem = Problem {
        unknowns: 5,
        terms: &terms,
        rows: &rows,
        fixed: &[],
    };
    let bounds: Vec<Bound> = (0..5)
        .map(|unknown| Bound {
            unknown,
            lower: -1.0,
            upper: 1.0,
        })
        .collect();
    let differences = [Difference {
        a: 4,
        b: 0,
        min: f64::NEG_INFINITY,
        max: 0.5,
    }];
    let constraints = Constraints {
        bounds: &bounds,
        differences: &differences,
    };
    let previous = [0.0; 5];
    let tight = Limits {
        max_iterations: 1,
        ..Limits::default()
    };
    assert_eq!(
        solve_constrained_ls(&problem, constraints, tight, Some(&previous)),
        Err(Error::SolveNotConverged)
    );
    assert_eq!(previous, [0.0; 5]); // borrowed, never written
    assert!(
        solve_constrained_ls(&problem, constraints, Limits::default(), Some(&previous)).is_ok()
    );
}

#[test]
fn malformed_requests_are_refused_by_name() {
    let (terms, rows) = anchors(&[1.0, 2.0]);
    let base = Problem {
        unknowns: 2,
        terms: &terms,
        rows: &rows,
        fixed: &[],
    };
    let limits = Limits::default();
    let outside = [Term {
        unknown: 2,
        coefficient: 1.0,
    }];
    assert_eq!(
        solve_sparse_ls(
            &Problem {
                terms: &outside,
                rows: rows.get(..1).unwrap_or(&[]),
                ..base
            },
            limits
        ),
        Err(Error::Index)
    );
    let long = [Row {
        first: 1,
        count: 2,
        target: 0.0,
        weight: 1.0,
    }];
    assert_eq!(
        solve_sparse_ls(
            &Problem {
                rows: &long,
                ..base
            },
            limits
        ),
        Err(Error::Shape)
    );
    let nan = [Row {
        first: 0,
        count: 1,
        target: f64::NAN,
        weight: 1.0,
    }];
    assert_eq!(
        solve_sparse_ls(&Problem { rows: &nan, ..base }, limits),
        Err(Error::NonFinite)
    );
    let zero = [Row {
        first: 0,
        count: 1,
        target: 0.0,
        weight: 0.0,
    }];
    assert_eq!(
        solve_sparse_ls(
            &Problem {
                rows: &zero,
                ..base
            },
            limits
        ),
        Err(Error::Weight)
    );
    let missing = [Fixed {
        unknown: 9,
        value: 0.0,
    }];
    assert_eq!(
        solve_sparse_ls(
            &Problem {
                fixed: &missing,
                ..base
            },
            limits
        ),
        Err(Error::Index)
    );
    for bad in [
        Limits {
            max_iterations: 0,
            ..limits
        },
        Limits {
            feasibility: 0.0,
            ..limits
        },
        Limits {
            residual: f64::NAN,
            ..limits
        },
    ] {
        assert_eq!(solve_sparse_ls(&base, bad), Err(Error::Limits));
    }
    let inverted = [Bound {
        unknown: 0,
        lower: 1.0,
        upper: 0.0,
    }];
    let constraints = Constraints {
        bounds: &inverted,
        differences: &[],
    };
    assert_eq!(
        solve_constrained_ls(&base, constraints, limits, None),
        Err(Error::Bounds)
    );
    let same = [Difference {
        a: 1,
        b: 1,
        min: 0.0,
        max: 0.0,
    }];
    let constraints = Constraints {
        bounds: &[],
        differences: &same,
    };
    assert_eq!(
        solve_constrained_ls(&base, constraints, limits, None),
        Err(Error::Index)
    );
    assert_eq!(
        solve_constrained_ls(&base, Constraints::default(), limits, Some(&[0.0])),
        Err(Error::Shape)
    );
    for error in [
        Error::Shape,
        Error::NonFinite,
        Error::Index,
        Error::Weight,
        Error::Bounds,
        Error::Limits,
        Error::SolveInconsistent,
        Error::SolveUnderdetermined,
        Error::SolveNotConverged,
    ] {
        assert!(error.message().contains(':'));
    }
}

#[test]
fn zero_unknowns_are_a_shape_error_before_indices() {
    let problem = Problem {
        unknowns: 0,
        terms: &[],
        rows: &[],
        fixed: &[],
    };
    assert_eq!(
        solve_sparse_ls(&problem, Limits::default()),
        Err(Error::Shape)
    );
}

/// Points on a line at x = 0..n, y = 0, heights `z`, as a strip of triangles
/// whose only plan-diagonal edges are long enough not to bind.
fn chain(z: &[f64]) -> (Vec<f64>, Vec<u32>) {
    let mut points = Vec::new();
    for (i, &height) in z.iter().enumerate() {
        points.extend([f64::from(u32::try_from(i).unwrap_or(0)), 0.0, height]);
    }
    let n = u32::try_from(z.len()).unwrap_or(0);
    // Degenerate in plan but edge-complete: (i, i+1, i+1) contributes edge i-(i+1).
    let triangles = (0..n.saturating_sub(1))
        .flat_map(|i| [i, i + 1, i + 1])
        .collect();
    (points, triangles)
}

#[test]
fn two_vertices_split_an_excess_rise_equally() -> Result<(), Error> {
    let (points, triangles) = chain(&[0.0, 3.0]);
    let request = SlopeLimitRequest {
        points: &points,
        triangles: &triangles,
        max_slope: Slope::RiseRun {
            rise: 1.0,
            run: 1.0,
        },
        fixed: &[],
        limits: Limits::default(),
    };
    let limited = reduce_slope(&request)?;
    assert!(close(
        limited.solution.values.first().copied().unwrap_or(f64::NAN),
        1.0,
        1e-10
    ));
    assert!(close(
        limited.solution.values.get(1).copied().unwrap_or(f64::NAN),
        2.0,
        1e-10
    ));
    assert_eq!(limited.changed, [0, 1]);
    // Holding the low vertex moves only the high one, down to the limit.
    let held = reduce_slope(&SlopeLimitRequest {
        fixed: &[0],
        ..request
    })?;
    assert!(close(
        held.solution.values.first().copied().unwrap_or(f64::NAN),
        0.0,
        0.0
    ));
    assert!(close(
        held.solution.values.get(1).copied().unwrap_or(f64::NAN),
        1.0,
        1e-10
    ));
    assert_eq!(held.changed, [1]);
    Ok(())
}

#[test]
fn a_cliff_becomes_a_symmetric_ramp_within_the_limit() -> Result<(), Error> {
    let z = [0.0, 0.0, 0.0, 0.0, 0.0, 2.0, 2.0, 2.0, 2.0, 2.0];
    let (points, triangles) = chain(&z);
    let request = SlopeLimitRequest {
        points: &points,
        triangles: &triangles,
        max_slope: Slope::Percent(50.0),
        fixed: &[],
        limits: Limits::default(),
    };
    let limited = reduce_slope(&request)?;
    let values = &limited.solution.values;
    for pair in values.windows(2) {
        if let [a, b] = *pair {
            assert!((b - a).abs() <= 0.5 + 1e-9, "{a} -> {b}");
        }
    }
    // Antisymmetric about the cliff: z_i + z_(9-i) = 2.
    for i in 0..5 {
        let low = values.get(i).copied().unwrap_or(f64::NAN);
        let high = values.get(9 - i).copied().unwrap_or(f64::NAN);
        assert!(close(low + high, 2.0, 1e-9), "{i}: {low} + {high}");
    }
    // Optimal ramp: the step's four central vertices move to 0.25/0.75/1.25/1.75.
    for (i, expected) in [(3, 0.25), (4, 0.75), (5, 1.25), (6, 1.75)] {
        assert!(close(
            values.get(i).copied().unwrap_or(f64::NAN),
            expected,
            1e-9
        ));
    }
    Ok(())
}

/// A `side x side` grid at 1 m with both diagonals' halves (two triangles per
/// cell), heights from `height`.
fn grid(side: u32, height: impl Fn(f64, f64) -> f64) -> (Vec<f64>, Vec<u32>) {
    let mut points = Vec::new();
    for row in 0..side {
        for column in 0..side {
            let (x, y) = (f64::from(column), f64::from(row));
            points.extend([x, y, height(x, y)]);
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

fn grade_ok(points: &[f64], triangles: &[u32], heights: &[f64], gradient: f64) -> bool {
    triangles.as_chunks::<3>().0.iter().all(|&[a, b, c]| {
        [(a, b), (b, c), (c, a)].iter().all(|&(p, q)| {
            let at = |v: u32, axis: usize| {
                points
                    .get(v as usize * 3 + axis)
                    .copied()
                    .unwrap_or(f64::NAN)
            };
            let run = (at(p, 0) - at(q, 0)).hypot(at(p, 1) - at(q, 1));
            let rise = heights.get(p as usize).copied().unwrap_or(f64::NAN)
                - heights.get(q as usize).copied().unwrap_or(f64::NAN);
            rise.abs() <= gradient * run + 1e-8
        })
    })
}

#[test]
fn a_steep_cone_converges_to_the_projection_onto_the_grade_limit() -> Result<(), Error> {
    // A 6 m cone on a 16 x 16 grid limited to 25 %: the triangle edges bind in
    // cycles, so the cold primal-dual start cannot settle and the split
    // estimate seeds the exact solve.
    let (points, triangles) = grid(16, |x, y| (6.0 - 0.9 * (x - 7.5).hypot(y - 7.5)).max(0.0));
    let request = SlopeLimitRequest {
        points: &points,
        triangles: &triangles,
        max_slope: Slope::Percent(25.0),
        fixed: &[],
        limits: Limits::default(),
    };
    let limited = reduce_slope(&request)?;
    let p = &limited.solution.values;
    assert!(grade_ok(&points, &triangles, p, 0.25));
    assert!(limited.solution.active > 0);
    // Projection onto a convex set: (z0 - p) . (q - p) <= 0 for every feasible q.
    // Feasible q: any constant, and any blend of p with a constant.
    let z0: Vec<f64> = points
        .as_chunks::<3>()
        .0
        .iter()
        .map(|&[_, _, z]| z)
        .collect();
    let mean = z0.iter().sum::<f64>() / f64::from(u32::try_from(z0.len()).unwrap_or(1));
    for q in [
        vec![0.0; p.len()],
        vec![mean; p.len()],
        p.iter().map(|v| 0.5 * v + 0.5 * mean).collect(),
    ] {
        let inner: f64 = z0
            .iter()
            .zip(p)
            .zip(&q)
            .map(|((z, p), q)| (z - p) * (q - p))
            .sum();
        assert!(inner <= 1e-7, "variational inequality violated: {inner}");
    }
    // Bit-reproducible through every phase.
    let again = reduce_slope(&request)?;
    let bits = |values: &[f64]| values.iter().map(|v| v.to_bits()).collect::<Vec<_>>();
    assert_eq!(bits(p), bits(&again.solution.values));
    Ok(())
}

#[test]
fn held_vertices_steeper_than_the_limit_are_inconsistent() {
    let (points, triangles) = chain(&[0.0, 3.0]);
    let request = SlopeLimitRequest {
        points: &points,
        triangles: &triangles,
        max_slope: Slope::Angle {
            radians: std::f64::consts::FRAC_PI_4,
        },
        fixed: &[0, 1],
        limits: Limits::default(),
    };
    assert_eq!(reduce_slope(&request), Err(Error::SolveInconsistent));
}

#[test]
fn a_vertical_limit_changes_nothing_and_a_bad_slope_is_refused() -> Result<(), Error> {
    let (points, triangles) = chain(&[0.0, 3.0, -1.0]);
    let request = SlopeLimitRequest {
        points: &points,
        triangles: &triangles,
        max_slope: Slope::RiseRun {
            rise: 1.0,
            run: 0.0,
        },
        fixed: &[],
        limits: Limits::default(),
    };
    let limited = reduce_slope(&request)?;
    assert_eq!(limited.changed, Vec::<u32>::new());
    let bad = SlopeLimitRequest {
        max_slope: Slope::Percent(-5.0),
        ..request
    };
    assert_eq!(reduce_slope(&bad), Err(Error::Bounds));
    let out_of_range = [0, 1, 7];
    assert_eq!(
        reduce_slope(&SlopeLimitRequest {
            triangles: &out_of_range,
            ..request
        }),
        Err(Error::Index)
    );
    Ok(())
}
