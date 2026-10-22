//! Active sets over the constraint graph.
//!
//! The working set is a set of edges held with equality. Its components are
//! groups: the ground's component is fixed, every other component is one free
//! column with exact offsets taken from the edge constants along a spanning tree
//! (breadth-first, lowest node and edge first). Active edges that close a cycle
//! are implied by the tree and leave the working set unless permanent, so each
//! tree edge's Lagrange multiplier is unique: the gradient summed over the
//! subtree it cuts off.
//!
//! **Phase 1, primal-dual (cold).** From the start's tight edges, solve the
//! group system, then keep an active edge while its multiplier is non-negative
//! and add every inactive edge the solution violates. With bounds only (M-matrix
//! Hessians) this settles in a few factorizations. It stops only at an exact KKT
//! point (no violated edge, no negative multiplier), and gives up as soon as its
//! flip count stops shrinking: on a steep mesh, edges of one triangle bind
//! together and a tree path implies more rise than the direct edge allows, so a
//! cold working set cycles.
//!
//! **Phase 2, split estimate then primal-dual (warm).** One factorization of an
//! operator-splitting system ([`super::admm`]) estimates the multipliers; its
//! dual-positive edges seed the same primal-dual iteration near the optimum,
//! where it converges like Newton's method and the seed is consistent.
//!
//! **Phase 3, primal.** If neither settles, a feasible point is rebuilt below
//! the last iterate (shortest paths) and a primal active set finishes: each
//! iteration either stops at the first blocking inactive edge (adding every edge
//! tied there) or, at the group optimum, releases the most negative multiplier.
//!
//! [`Limits::max_iterations`] bounds factorizations across all phases: a quarter
//! for phase 1, up to three quarters with phase 2, the rest for phase 3.
//! Phase 2 polishes at successively tighter split accuracies, each polish a
//! short guarded primal-dual run from the split's estimate.

use super::admm;
use super::certify;
use super::feasible::{self, Edge};
use super::normal::{self, Slot};
use super::{Checked, Error, Limits, METHOD, Solution, objective};
use std::collections::VecDeque;

/// The working set's groups for one iteration.
struct Groups {
    slots: Vec<Slot>,
    columns: usize,
    /// Breadth-first order per component; parents before children.
    order: Vec<u32>,
    /// Tree edge joining each node to its parent.
    parent_edge: Vec<Option<usize>>,
}

fn node_value(values: &[f64], node: u32) -> f64 {
    if node == 0 {
        0.0
    } else {
        values.get(node as usize - 1).copied().unwrap_or(0.0)
    }
}

/// How far `x` violates edge `e` (positive when violated), in its units.
fn violation(e: &Edge, x: &[f64]) -> f64 {
    node_value(x, e.u) - node_value(x, e.v) - e.c
}

/// Spanning forest of the active edges; drops implied non-permanent edges.
fn groups(nodes: usize, edges: &[Edge], active: &mut [bool]) -> Result<Groups, Error> {
    let mut adjacent: Vec<Vec<usize>> = vec![Vec::new(); nodes];
    for (index, e) in edges.iter().enumerate() {
        if active.get(index).copied().unwrap_or(false) {
            adjacent
                .get_mut(e.u as usize)
                .ok_or(Error::Shape)?
                .push(index);
            adjacent
                .get_mut(e.v as usize)
                .ok_or(Error::Shape)?
                .push(index);
        }
    }
    let mut seen = vec![false; nodes];
    let mut potential = vec![0.0_f64; nodes];
    let mut root_column: Vec<Option<u32>> = vec![None; nodes];
    let mut parent_edge = vec![None; nodes];
    let mut tree = vec![false; edges.len()];
    let mut order = Vec::with_capacity(nodes);
    let mut columns = 0_u32;
    let mut queue = VecDeque::new();
    for root in 0..nodes {
        if seen.get(root).copied().unwrap_or(true) {
            continue;
        }
        let column = if root == 0 {
            None
        } else {
            columns += 1;
            Some(columns - 1)
        };
        if let Some(flag) = seen.get_mut(root) {
            *flag = true;
        }
        queue.push_back(root);
        while let Some(node) = queue.pop_front() {
            order.push(u32::try_from(node).map_err(|_| Error::Shape)?);
            if let Some(slot) = root_column.get_mut(node) {
                *slot = column;
            }
            let base = potential.get(node).copied().unwrap_or(0.0);
            for &index in adjacent.get(node).map_or(&[][..], Vec::as_slice) {
                let Some(e) = edges.get(index) else { continue };
                // x_u - x_v = c along this edge.
                let (other, offset) = if e.v as usize == node {
                    (e.u as usize, base + e.c)
                } else {
                    (e.v as usize, base - e.c)
                };
                if seen.get(other).copied().unwrap_or(true) {
                    continue;
                }
                if let Some(flag) = seen.get_mut(other) {
                    *flag = true;
                }
                if let Some(slot) = potential.get_mut(other) {
                    *slot = offset;
                }
                if let Some(slot) = parent_edge.get_mut(other) {
                    *slot = Some(index);
                }
                if let Some(flag) = tree.get_mut(index) {
                    *flag = true;
                }
                queue.push_back(other);
            }
        }
    }
    for ((flag, e), in_tree) in active.iter_mut().zip(edges).zip(&tree) {
        if *flag && !in_tree && !e.permanent {
            *flag = false;
        }
    }
    let slots = (1..nodes)
        .map(|node| {
            let offset = potential.get(node).copied().unwrap_or(0.0);
            match root_column.get(node).copied().flatten() {
                None => Slot::Fixed(offset),
                Some(column) => Slot::Free { column, offset },
            }
        })
        .collect();
    Ok(Groups {
        slots,
        columns: columns as usize,
        order,
        parent_edge,
    })
}

/// One Lagrange multiplier per edge: subtree gradient flows on tree edges,
/// zero for every other edge.
fn multipliers(edges: &[Edge], groups: &Groups, gradient: &[f64]) -> Vec<f64> {
    let nodes = groups.parent_edge.len();
    let mut subtree: Vec<f64> = (0..nodes)
        .map(|node| {
            if node == 0 {
                0.0
            } else {
                gradient.get(node - 1).copied().unwrap_or(0.0)
            }
        })
        .collect();
    let mut lambda = vec![0.0; edges.len()];
    for &node in groups.order.iter().rev() {
        let node = node as usize;
        let Some(index) = groups.parent_edge.get(node).copied().flatten() else {
            continue;
        };
        let Some(e) = edges.get(index) else { continue };
        let sum = subtree.get(node).copied().unwrap_or(0.0);
        let parent = if e.u as usize == node { e.v } else { e.u } as usize;
        if let Some(slot) = subtree.get_mut(parent) {
            *slot += sum;
        }
        // Stationarity over the cut-off subtree: sum(g) + sign * lambda = 0.
        if let Some(slot) = lambda.get_mut(index) {
            *slot = if e.u as usize == node { -sum } else { sum };
        }
    }
    lambda
}

/// Multiplier tolerance relative to the largest gradient component.
fn dual_tolerance(gradient: &[f64], limits: Limits) -> f64 {
    limits.residual * 1e2 * gradient.iter().fold(1.0_f64, |most, g| most.max(g.abs()))
}

/// Step from feasible `x` toward `target`, returning the step length and the
/// inactive edges that block at it. An edge whose difference moves by less than a
/// thousandth of the feasibility tolerance over the whole step cannot block: an
/// edge implied by the tree has a rounding-level rate and would stall at zero.
fn ratio(
    edges: &[Edge],
    active: &[bool],
    x: &[f64],
    target: &[f64],
    limits: Limits,
) -> (f64, Vec<usize>) {
    let tie = 1e-12;
    let mut alpha = 1.0_f64;
    let mut steps = Vec::new();
    for (index, e) in edges.iter().enumerate() {
        if active.get(index).copied().unwrap_or(true) {
            continue;
        }
        let rate = (node_value(target, e.u) - node_value(x, e.u))
            - (node_value(target, e.v) - node_value(x, e.v));
        if rate <= limits.feasibility * 1e-3 {
            continue;
        }
        let slack = (-violation(e, x)).max(0.0);
        let step = slack / rate;
        if step < 1.0 + tie {
            steps.push((step, index));
            alpha = alpha.min(step);
        }
    }
    let blocking = steps
        .into_iter()
        .filter(|&(step, _)| step <= alpha + tie)
        .map(|(_, index)| index)
        .collect();
    (alpha, blocking)
}

/// Work done across all phases. `used` is the budget spent: one per active-set
/// iteration plus one per split factorization.
#[derive(Default)]
struct Counters {
    iterations: u32,
    factorizations: u32,
    used: u32,
}

/// What every phase reads.
#[derive(Clone, Copy)]
struct Scope<'a> {
    checked: &'a Checked<'a>,
    edges: &'a [Edge],
    limits: Limits,
}

/// When a primal-dual run stops: at budget mark `until`, and with `guard` as
/// soon as its flip count fails to shrink.
#[derive(Clone, Copy)]
struct Stop {
    until: u32,
    guard: bool,
}

fn finish(
    scope: Scope<'_>,
    active: &[bool],
    x: Vec<f64>,
    total: f64,
    counters: &Counters,
) -> Result<Solution, Error> {
    let binding = scope
        .edges
        .iter()
        .zip(active)
        .filter(|(e, flag)| **flag && !e.permanent)
        .count();
    Ok(Solution {
        values: x,
        iterations: counters.iterations,
        factorizations: counters.factorizations,
        active: u32::try_from(binding).map_err(|_| Error::Shape)?,
        objective: total,
        method: METHOD,
    })
}

fn tight(edges: &[Edge], x: &[f64], limits: Limits) -> Vec<bool> {
    edges
        .iter()
        .map(|e| e.permanent || violation(e, x) >= -limits.feasibility)
        .collect()
}

/// Primal-dual iterations from the seed `active`. `Ok(Err(x))` hands the last
/// iterate on.
fn dual(
    scope: Scope<'_>,
    mut active: Vec<bool>,
    start: &[f64],
    stop: Stop,
    counters: &mut Counters,
) -> Result<Result<Solution, Vec<f64>>, Error> {
    let Scope {
        checked,
        edges,
        limits,
    } = scope;
    let nodes = checked.unknowns + 1;
    let mut x = start.to_vec();
    let mut previous = usize::MAX;
    while counters.used < stop.until {
        counters.iterations += 1;
        counters.used += 1;
        let working = groups(nodes, edges, &mut active)?;
        let solved = normal::solve(&checked.rows, &working.slots, working.columns, limits)?;
        counters.factorizations += u32::from(working.columns > 0);
        x = normal::expand(&working.slots, &solved.columns);
        let (total, gradient) = objective(&checked.rows, &x);
        let lambda = multipliers(edges, &working, &gradient);
        let tolerance = dual_tolerance(&gradient, limits);
        let violated = edges
            .iter()
            .zip(&active)
            .any(|(e, flag)| !*flag && !e.permanent && violation(e, &x) > limits.feasibility);
        // Feasible: tree multipliers may be negative only because tight cycles
        // make them non-unique. The flow certificate decides.
        if !violated {
            let held = tight(edges, &x, limits);
            if certify::certify(edges, &held, &gradient, tolerance) {
                return finish(scope, &held, x, total, counters).map(Ok);
            }
        }
        let mut flips = 0_usize;
        for ((flag, e), &multiplier) in active.iter_mut().zip(edges).zip(&lambda) {
            if e.permanent {
                continue;
            }
            if *flag && multiplier < -tolerance {
                *flag = false;
                flips += 1;
            } else if !*flag && violation(e, &x) > limits.feasibility {
                *flag = true;
                flips += 1;
            }
        }
        if flips == 0 {
            return finish(scope, &active, x, total, counters).map(Ok);
        }
        if stop.guard && flips >= previous {
            break;
        }
        previous = flips;
    }
    Ok(Err(x))
}

/// Phase 3 from a feasible `x`, within the remaining budget.
fn primal(scope: Scope<'_>, mut x: Vec<f64>, counters: &mut Counters) -> Result<Solution, Error> {
    let Scope {
        checked,
        edges,
        limits,
    } = scope;
    let nodes = checked.unknowns + 1;
    let mut active = tight(edges, &x, limits);
    while counters.used < limits.max_iterations {
        counters.iterations += 1;
        counters.used += 1;
        let working = groups(nodes, edges, &mut active)?;
        let solved = normal::solve(&checked.rows, &working.slots, working.columns, limits)?;
        counters.factorizations += u32::from(working.columns > 0);
        let target = normal::expand(&working.slots, &solved.columns);
        let (alpha, blocking) = ratio(edges, &active, &x, &target, limits);
        if alpha < 1.0 || !blocking.is_empty() {
            for (value, goal) in x.iter_mut().zip(&target) {
                *value += alpha * (goal - *value);
            }
            for index in blocking {
                if let Some(flag) = active.get_mut(index) {
                    *flag = true;
                }
            }
            continue;
        }
        x = target;
        let (total, gradient) = objective(&checked.rows, &x);
        let lambda = multipliers(edges, &working, &gradient);
        let tolerance = dual_tolerance(&gradient, limits);
        // Most negative releasable multiplier; the lowest edge index wins ties.
        let mut release: Option<(usize, f64)> = None;
        for (index, (e, &multiplier)) in edges.iter().zip(&lambda).enumerate() {
            if !e.permanent
                && multiplier < -tolerance
                && release.is_none_or(|(_, most)| multiplier < most)
            {
                release = Some((index, multiplier));
            }
        }
        let held = tight(edges, &x, limits);
        match release {
            Some(_) if certify::certify(edges, &held, &gradient, tolerance) => {
                return finish(scope, &held, x, total, counters);
            }
            Some((index, _)) => {
                if let Some(flag) = active.get_mut(index) {
                    *flag = false;
                }
            }
            None => return finish(scope, &active, x, total, counters),
        }
    }
    Err(Error::SolveNotConverged)
}

/// Split accuracies at which an exact polish is attempted, loosest first. The
/// first is enough for a 10k-vertex terrain; steep cones need tighter ones.
const LEVELS: [f64; 5] = [3e-3, 1e-4, 3e-6, 1e-7, 3e-9];
/// Primal-dual corrections allowed after each polish.
const POLISH: u32 = 3;

/// Phase 2: polish the split's estimate at successively tighter accuracies.
fn polish(
    scope: Scope<'_>,
    until: u32,
    last: &mut Vec<f64>,
    counters: &mut Counters,
) -> Result<Option<Solution>, Error> {
    if counters.used >= until {
        return Ok(None);
    }
    let Some(mut split) = admm::Split::new(scope.checked, scope.edges)? else {
        return Ok(None);
    };
    counters.used += 1;
    counters.factorizations += 1;
    for level in LEVELS {
        if counters.used >= until {
            break;
        }
        let refactors = (until - counters.used).saturating_sub(1).min(4);
        let (reached, used) = split.advance(level, refactors)?;
        counters.used += used;
        counters.factorizations += used;
        let stop = Stop {
            until: until.min(counters.used.saturating_add(POLISH)),
            guard: true,
        };
        match dual(
            scope,
            split.active(scope.edges, scope.limits),
            &split.values(),
            stop,
            counters,
        )? {
            Ok(solution) => return Ok(Some(solution)),
            Err(next) => *last = next,
        }
        if !reached {
            break;
        }
    }
    Ok(None)
}

/// Run the phases from the feasible `start`.
///
/// # Errors
/// [`Error::SolveUnderdetermined`] from a group system, [`Error::SolveInconsistent`]
/// if phase 3 cannot rebuild a feasible point, or [`Error::SolveNotConverged`]
/// when [`Limits::max_iterations`] is exhausted.
pub(super) fn solve(
    checked: &Checked<'_>,
    edges: &[Edge],
    start: &[f64],
    limits: Limits,
) -> Result<Solution, Error> {
    let scope = Scope {
        checked,
        edges,
        limits,
    };
    let budget = limits.max_iterations;
    let mut counters = Counters::default();
    let cold = Stop {
        until: budget.div_ceil(4),
        guard: true,
    };
    let mut last = match dual(
        scope,
        tight(edges, start, limits),
        start,
        cold,
        &mut counters,
    )? {
        Ok(solution) => return Ok(solution),
        Err(last) => last,
    };
    if let Some(solution) = polish(
        scope,
        budget.saturating_mul(3).div_ceil(4),
        &mut last,
        &mut counters,
    )? {
        return Ok(solution);
    }
    let feasible = feasible::start(checked.unknowns, edges, &last, limits)?;
    primal(scope, feasible, &mut counters)
}
