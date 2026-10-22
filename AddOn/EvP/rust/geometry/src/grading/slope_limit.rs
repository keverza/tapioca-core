//! `reduce_slope`: the nearest surface (least squares in height) whose every mesh
//! edge is no steeper than a maximum grade, with chosen vertices held.
//!
//! Each vertex height is an unknown anchored to its current height by a unit-
//! weight row; each distinct triangle edge contributes `|z_a - z_b| <= g * d_ab`
//! for plan length `d_ab` and gradient `g`. The plan positions never move.

use super::{
    Constraints, Difference, Error, Fixed, Limits, Problem, Row, Term, solve_constrained_ls,
};
use crate::slope::Slope;

/// Borrowed slope-limit request over an existing triangle mesh.
#[derive(Clone, Copy, Debug)]
pub struct SlopeLimitRequest<'a> {
    /// Flat xyz world metres.
    pub points: &'a [f64],
    /// Three vertex indices per triangle; every distinct edge is limited.
    pub triangles: &'a [u32],
    /// Steepest grade allowed along an edge; vertical means no limit.
    pub max_slope: Slope,
    /// Vertices whose height must not change.
    pub fixed: &'a [u32],
    /// Solve budgets and tolerances.
    pub limits: Limits,
}

/// Limited heights and what moved.
#[derive(Clone, Debug, PartialEq)]
pub struct SlopeLimited {
    /// Vertices whose height changed by more than the feasibility tolerance,
    /// ascending.
    pub changed: Vec<u32>,
    /// The underlying solve; `values` holds one height per input vertex (metres).
    pub solution: super::Solution,
}

/// Distinct undirected edges `(low, high)` in ascending order.
fn edges(triangles: &[u32], vertices: usize) -> Result<Vec<(u32, u32)>, Error> {
    if !triangles.len().is_multiple_of(3) {
        return Err(Error::Shape);
    }
    let mut out = Vec::with_capacity(triangles.len());
    for &[a, b, c] in triangles.as_chunks::<3>().0 {
        if [a, b, c].iter().any(|&v| v as usize >= vertices) {
            return Err(Error::Index);
        }
        for (p, q) in [(a, b), (b, c), (c, a)] {
            if p != q {
                out.push((p.min(q), p.max(q)));
            }
        }
    }
    out.sort_unstable();
    out.dedup();
    Ok(out)
}

/// Limit every edge's grade, moving heights as little as possible.
///
/// # Errors
/// [`Error::Shape`]/[`Error::Index`]/[`Error::NonFinite`] for a malformed mesh,
/// [`Error::Bounds`] for an invalid slope, [`Error::SolveInconsistent`] when held
/// vertices are already steeper apart than the limit allows, and the solve's
/// own refusals. Nothing partial is returned.
pub fn reduce_slope(request: &SlopeLimitRequest<'_>) -> Result<SlopeLimited, Error> {
    let points = request.points;
    if !points.len().is_multiple_of(3) || points.is_empty() {
        return Err(Error::Shape);
    }
    if points.iter().any(|value| !value.is_finite()) {
        return Err(Error::NonFinite);
    }
    let vertices = points.len() / 3;
    let unknowns = u32::try_from(vertices).map_err(|_| Error::Shape)?;
    let gradient = request.max_slope.gradient().map_err(|_| Error::Bounds)?;
    let mut differences = Vec::new();
    if gradient.is_finite() {
        for (a, b) in edges(request.triangles, vertices)? {
            let at =
                |v: u32, axis: usize| points.get(v as usize * 3 + axis).copied().unwrap_or(0.0);
            let run = (at(a, 0) - at(b, 0)).hypot(at(a, 1) - at(b, 1));
            let rise = gradient * run;
            differences.push(Difference {
                a,
                b,
                min: -rise,
                max: rise,
            });
        }
    } else {
        edges(request.triangles, vertices)?;
    }
    let heights: Vec<f64> = points
        .as_chunks::<3>()
        .0
        .iter()
        .map(|&[_, _, z]| z)
        .collect();
    let terms: Vec<Term> = (0..unknowns)
        .map(|unknown| Term {
            unknown,
            coefficient: 1.0,
        })
        .collect();
    let rows: Vec<Row> = (0..unknowns)
        .zip(&heights)
        .map(|(first, &target)| Row {
            first,
            count: 1,
            target,
            weight: 1.0,
        })
        .collect();
    let fixed = request
        .fixed
        .iter()
        .map(|&unknown| {
            heights
                .get(unknown as usize)
                .map(|&value| Fixed { unknown, value })
                .ok_or(Error::Index)
        })
        .collect::<Result<Vec<_>, _>>()?;
    let problem = Problem {
        unknowns,
        terms: &terms,
        rows: &rows,
        fixed: &fixed,
    };
    let constraints = Constraints {
        bounds: &[],
        differences: &differences,
    };
    let solution = solve_constrained_ls(&problem, constraints, request.limits, Some(&heights))?;
    let changed = (0..unknowns)
        .zip(solution.values.iter().zip(&heights))
        .filter(|(_, (new, old))| (*new - *old).abs() > request.limits.feasibility)
        .map(|(vertex, _)| vertex)
        .collect();
    Ok(SlopeLimited { changed, solution })
}
