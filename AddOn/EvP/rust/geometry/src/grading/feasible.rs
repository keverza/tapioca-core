//! The constraint graph and a feasible starting point.
//!
//! Node 0 is the ground (value 0); node `i + 1` is unknown `i`. Every limit is a
//! directed edge `x_u - x_v <= c`: an upper bound is `(i, ground, upper)`, a lower
//! bound `(ground, i, -lower)`, a fixed value both, and a difference limit one
//! edge per finite side. Such a system is feasible exactly when the graph has no
//! negative cycle, and shortest paths from a source joined to every node by the
//! start values give a feasible point at or below that start.

use super::{Checked, Constraints, Error, Limits};
use std::collections::VecDeque;

/// One directed limit `x_u - x_v <= c` over graph nodes.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(super) struct Edge {
    pub(super) u: u32,
    pub(super) v: u32,
    pub(super) c: f64,
    /// From a fixed value: always active, never released.
    pub(super) permanent: bool,
}

fn edge(u: usize, v: usize, c: f64, permanent: bool) -> Result<Edge, Error> {
    Ok(Edge {
        u: u32::try_from(u).map_err(|_| Error::Shape)?,
        v: u32::try_from(v).map_err(|_| Error::Shape)?,
        c,
        permanent,
    })
}

/// Fixed values first (by unknown), then bounds, then differences, in caller order.
///
/// # Errors
/// [`Error::Index`] or [`Error::Bounds`] for a malformed limit.
pub(super) fn edges(
    checked: &Checked<'_>,
    constraints: Constraints<'_>,
) -> Result<Vec<Edge>, Error> {
    let n = checked.unknowns;
    let mut out = Vec::new();
    for (i, value) in checked.fixed.iter().enumerate() {
        if let Some(value) = *value {
            out.push(edge(i + 1, 0, value, true)?);
            out.push(edge(0, i + 1, -value, true)?);
        }
    }
    let valid = |lower: f64, upper: f64| {
        !lower.is_nan()
            && !upper.is_nan()
            && lower <= upper
            && lower < f64::INFINITY
            && upper > f64::NEG_INFINITY
    };
    for bound in constraints.bounds {
        let i = bound.unknown as usize;
        if i >= n {
            return Err(Error::Index);
        }
        if !valid(bound.lower, bound.upper) {
            return Err(Error::Bounds);
        }
        if bound.upper.is_finite() {
            out.push(edge(i + 1, 0, bound.upper, false)?);
        }
        if bound.lower.is_finite() {
            out.push(edge(0, i + 1, -bound.lower, false)?);
        }
    }
    for difference in constraints.differences {
        let (a, b) = (difference.a as usize, difference.b as usize);
        if a >= n || b >= n || a == b {
            return Err(Error::Index);
        }
        if !valid(difference.min, difference.max) {
            return Err(Error::Bounds);
        }
        if difference.max.is_finite() {
            out.push(edge(a + 1, b + 1, difference.max, false)?);
        }
        if difference.min.is_finite() {
            out.push(edge(b + 1, a + 1, -difference.min, false)?);
        }
    }
    Ok(out)
}

/// A feasible value per unknown at or below `initial` (shifted so the ground is 0).
///
/// Queue-based Bellman-Ford over `v -> u` relaxations of weight `c`, in a fixed
/// order. A node relaxed more than once per node in the graph proves a negative
/// cycle. Improvements below a hundredth of the feasibility tolerance are not
/// relaxations, so rounding on a zero-weight cycle cannot read as a contradiction.
///
/// # Errors
/// [`Error::SolveInconsistent`] when the limits admit no value.
pub(super) fn start(
    unknowns: usize,
    edges: &[Edge],
    initial: &[f64],
    limits: Limits,
) -> Result<Vec<f64>, Error> {
    let nodes = unknowns + 1;
    // Outgoing relaxations by source node v, stable in edge order.
    let mut first = vec![0_usize; nodes + 1];
    for e in edges {
        if let Some(slot) = first.get_mut(e.v as usize + 1) {
            *slot += 1;
        }
    }
    for i in 0..nodes {
        let before = first.get(i).copied().unwrap_or(0);
        if let Some(slot) = first.get_mut(i + 1) {
            *slot += before;
        }
    }
    let mut cursor = first.clone();
    let mut order = vec![0_usize; edges.len()];
    for (index, e) in edges.iter().enumerate() {
        if let Some(position) = cursor.get_mut(e.v as usize)
            && let Some(slot) = order.get_mut(*position)
        {
            *slot = index;
            *position += 1;
        }
    }
    let mut distance: Vec<f64> = std::iter::once(0.0)
        .chain(initial.iter().copied())
        .collect();
    let mut queued = vec![true; nodes];
    let mut relaxed = vec![0_usize; nodes];
    let mut queue: VecDeque<usize> = (0..nodes).collect();
    let slack = limits.feasibility * 1e-2;
    while let Some(v) = queue.pop_front() {
        if let Some(flag) = queued.get_mut(v) {
            *flag = false;
        }
        let from = distance.get(v).copied().unwrap_or(0.0);
        let range = first.get(v).copied().unwrap_or(0)..first.get(v + 1).copied().unwrap_or(0);
        for &index in order.get(range).unwrap_or(&[]) {
            let Some(e) = edges.get(index) else { continue };
            let u = e.u as usize;
            let candidate = from + e.c;
            let Some(current) = distance.get_mut(u) else {
                continue;
            };
            if candidate < *current - slack {
                *current = candidate;
                let count = relaxed.get_mut(u).ok_or(Error::Shape)?;
                *count += 1;
                if *count > nodes {
                    return Err(Error::SolveInconsistent);
                }
                if let Some(flag) = queued.get_mut(u)
                    && !*flag
                {
                    *flag = true;
                    queue.push_back(u);
                }
            }
        }
    }
    let ground = distance.first().copied().unwrap_or(0.0);
    let values: Vec<f64> = distance
        .iter()
        .skip(1)
        .map(|value| value - ground)
        .collect();
    // The construction satisfies every edge to within the relaxation slack.
    let value = |node: u32| {
        if node == 0 {
            0.0
        } else {
            values.get(node as usize - 1).copied().unwrap_or(0.0)
        }
    };
    if edges
        .iter()
        .any(|e| value(e.u) - value(e.v) > e.c + limits.feasibility)
    {
        return Err(Error::SolveInconsistent);
    }
    Ok(values)
}
