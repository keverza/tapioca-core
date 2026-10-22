//! Opt-in S3b.3 grading-solve measurement, never linked into the shipped archive.
//!
//! `cargo run --release --offline --locked -p tapioca-core-ffi --example grading_measure [runs]`
//! prints one JSON line per workload: 10k unknowns on a 100 x 100 lattice (1 m
//! spacing), median and p95 wall time in milliseconds over `runs` fresh solves
//! (default 21), and the solve's own iteration/factorization/active counts.
#![forbid(unsafe_code)]

use std::time::Instant;
use tapioca_geometry::grading::{
    Bound, Constraints, Error, Fixed, Limits, Problem, Row, SlopeLimitRequest, Solution, Term,
    reduce_slope, solve_constrained_ls, solve_sparse_ls,
};
use tapioca_geometry::slope::Slope;

const SIDE: u32 = 100;

fn node(column: u32, row: u32) -> u32 {
    row * SIDE + column
}

/// A deterministic bumpy terrain with small pseudo-random roughness, metres.
fn terrain(column: u32, row: u32) -> f64 {
    let (x, y) = (f64::from(column), f64::from(row));
    let noise = f64::from((column * 7919 + row * 104_729) % 101) / 101.0 - 0.5;
    3.0 * (x / 7.0).sin() * (y / 5.0).cos() + 0.04 * noise
}

struct Rows {
    terms: Vec<Term>,
    rows: Vec<Row>,
}

impl Rows {
    fn push(&mut self, entries: &[(u32, f64)], target: f64, weight: f64) {
        let first = u32::try_from(self.terms.len()).unwrap_or(u32::MAX);
        for &(unknown, coefficient) in entries {
            self.terms.push(Term {
                unknown,
                coefficient,
            });
        }
        self.rows.push(Row {
            first,
            count: u32::try_from(entries.len()).unwrap_or(0),
            target,
            weight,
        });
    }
}

/// Lattice smoothness rows `x_a - x_b = 0`, plus optional anchors to the terrain.
fn lattice(anchor_weight: Option<f64>) -> Rows {
    let mut rows = Rows {
        terms: Vec::new(),
        rows: Vec::new(),
    };
    for row in 0..SIDE {
        for column in 0..SIDE {
            if column + 1 < SIDE {
                rows.push(
                    &[(node(column, row), 1.0), (node(column + 1, row), -1.0)],
                    0.0,
                    1.0,
                );
            }
            if row + 1 < SIDE {
                rows.push(
                    &[(node(column, row), 1.0), (node(column, row + 1), -1.0)],
                    0.0,
                    1.0,
                );
            }
            if let Some(weight) = anchor_weight {
                rows.push(&[(node(column, row), 1.0)], terrain(column, row), weight);
            }
        }
    }
    rows
}

fn measure(name: &str, runs: usize, mut solve: impl FnMut() -> Result<Solution, Error>) {
    let mut times = Vec::with_capacity(runs);
    let mut last = None;
    for _ in 0..runs {
        let started = Instant::now();
        let result = solve();
        times.push(started.elapsed().as_secs_f64() * 1e3);
        last = Some(result);
    }
    times.sort_by(f64::total_cmp);
    // Nearest-rank percentile in integer arithmetic: index round((n - 1) * p / 100).
    let at = |percent: usize| {
        let index = (times.len().saturating_sub(1) * percent + 50) / 100;
        times.get(index).copied().unwrap_or(f64::NAN)
    };
    match last {
        Some(Ok(solution)) => println!(
            "{{\"workload\":\"{name}\",\"unknowns\":{},\"runs\":{runs},\"medianMs\":{:.3},\"p95Ms\":{:.3},\"iterations\":{},\"factorizations\":{},\"active\":{},\"method\":\"{}\"}}",
            solution.values.len(),
            at(50),
            at(95),
            solution.iterations,
            solution.factorizations,
            solution.active,
            solution.method
        ),
        Some(Err(error)) => println!(
            "{{\"workload\":\"{name}\",\"error\":\"{}\"}}",
            error.message()
        ),
        None => {}
    }
}

fn main() {
    let runs = std::env::args()
        .nth(1)
        .and_then(|text| text.parse().ok())
        .unwrap_or(21);
    let unknowns = SIDE * SIDE;

    // Harmonic transition: perimeter fixed to the terrain, a ridge column at 5 m.
    let smooth = lattice(None);
    let mut fixed = Vec::new();
    for row in 0..SIDE {
        for column in 0..SIDE {
            let edge = column == 0 || row == 0 || column == SIDE - 1 || row == SIDE - 1;
            if edge || column == SIDE / 2 {
                let value = if column == SIDE / 2 {
                    5.0
                } else {
                    terrain(column, row)
                };
                fixed.push(Fixed {
                    unknown: node(column, row),
                    value,
                });
            }
        }
    }
    let harmonic = Problem {
        unknowns,
        terms: &smooth.terms,
        rows: &smooth.rows,
        fixed: &fixed,
    };
    measure("harmonic-fixed-perimeter-ridge", runs, || {
        solve_sparse_ls(&harmonic, Limits::default())
    });

    // Grading fit: smoothness plus weak anchors to the existing surface, 200 spots.
    let anchored = lattice(Some(0.1));
    let spots: Vec<Fixed> = (0..200)
        .map(|i| {
            let (column, row) = ((i * 37) % SIDE, (i * 53) % SIDE);
            Fixed {
                unknown: node(column, row),
                value: terrain(column, row) + 0.5,
            }
        })
        .collect();
    let fit = Problem {
        unknowns,
        terms: &anchored.terms,
        rows: &anchored.rows,
        fixed: &spots,
    };
    measure("anchored-fit-200-spots", runs, || {
        solve_sparse_ls(&fit, Limits::default())
    });

    // The same fit with cut/fill limits of 0.25 m around the existing surface.
    let bounds: Vec<Bound> = (0..unknowns)
        .map(|unknown| {
            let z = terrain(unknown % SIDE, unknown / SIDE);
            Bound {
                unknown,
                lower: z - 0.25,
                upper: z + 0.25,
            }
        })
        .collect();
    let spots_in_bounds: Vec<Fixed> = spots
        .iter()
        .map(|spot| Fixed {
            value: spot.value - 0.3,
            ..*spot
        })
        .collect();
    let bounded = Problem {
        fixed: &spots_in_bounds,
        ..fit
    };
    let existing: Vec<f64> = (0..unknowns).map(|u| terrain(u % SIDE, u / SIDE)).collect();
    let constraints = Constraints {
        bounds: &bounds,
        differences: &[],
    };
    measure("anchored-fit-cut-fill-bounds", runs, || {
        solve_constrained_ls(&bounded, constraints, Limits::default(), Some(&existing))
    });

    // Slope limit on the bumpy terrain, two triangles per cell.
    let mut points = Vec::new();
    for row in 0..SIDE {
        for column in 0..SIDE {
            points.extend([f64::from(column), f64::from(row), terrain(column, row)]);
        }
    }
    let mut triangles = Vec::new();
    for row in 0..SIDE - 1 {
        for column in 0..SIDE - 1 {
            let (a, b, c, d) = (
                node(column, row),
                node(column + 1, row),
                node(column + 1, row + 1),
                node(column, row + 1),
            );
            triangles.extend([a, b, c, a, c, d]);
        }
    }
    let request = SlopeLimitRequest {
        points: &points,
        triangles: &triangles,
        max_slope: Slope::Percent(33.0),
        fixed: &[],
        limits: Limits::default(),
    };
    measure("reduce-slope-33-percent", runs, || {
        reduce_slope(&request).map(|limited| limited.solution)
    });
}
