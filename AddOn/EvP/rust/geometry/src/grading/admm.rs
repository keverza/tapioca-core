//! Operator-splitting (ADMM) estimates of the active set.
//!
//! Minimizes `1/2 x^T P x - r^T x` over the free columns subject to one row
//! `c^T x <= u` per non-permanent edge, splitting `z = C x` (relaxation 1.6,
//! `sigma` 1e-6). `P + sigma I + rho C^T C` is factorized once per `rho`; every
//! iteration is one pair of triangular solves and a projection. `rho` adapts to
//! the residual ratio while the caller's factorization budget allows.
//!
//! The split is resumable: [`Split::advance`] continues from its state to a
//! tighter accuracy. Its estimate is never returned as a solution: the
//! dual-positive edges (dual above the slack, OSQP's polish rule) seed an exact
//! group solve that must certify KKT. As the split converges its active set
//! becomes the consistent optimal one, so polishing at successively tighter
//! accuracies settles where a cold working set on a steep mesh cycles.

use super::feasible::Edge;
use super::normal::{self, Factor, Slot};
use super::{Checked, Error, Limits};
use faer::Col;

/// Splitting iterations across all accuracies before the estimate stands as is.
const ITERATIONS: u32 = 4000;
/// Residual checks happen every this many iterations, `rho` updates every `ADAPT`.
const CHECK: u32 = 10;
const ADAPT: u32 = 50;
const RELAXATION: f64 = 1.6;
const SIGMA: f64 = 1e-6;

/// One constraint row over free columns: at most two unit entries.
struct Limit {
    edge: usize,
    entries: [(u32, f64); 2],
    count: usize,
    upper: f64,
}

impl Limit {
    fn entries(&self) -> &[(u32, f64)] {
        self.entries.get(..self.count).unwrap_or(&[])
    }
    fn apply(&self, values: &[f64]) -> f64 {
        self.entries()
            .iter()
            .map(|&(column, sign)| sign * values.get(column as usize).copied().unwrap_or(0.0))
            .sum()
    }
}

fn limits_over(edges: &[Edge], slots: &[Slot]) -> Vec<Limit> {
    let mut out = Vec::new();
    for (index, e) in edges.iter().enumerate() {
        if e.permanent {
            continue;
        }
        let mut limit = Limit {
            edge: index,
            entries: [(0, 0.0); 2],
            count: 0,
            upper: e.c,
        };
        for (node, sign) in [(e.u, 1.0), (e.v, -1.0)] {
            if node == 0 {
                continue;
            }
            match slots.get(node as usize - 1) {
                Some(Slot::Fixed(value)) => limit.upper -= sign * value,
                Some(&Slot::Free { column, .. }) => {
                    if let Some(slot) = limit.entries.get_mut(limit.count) {
                        *slot = (column, sign);
                        limit.count += 1;
                    }
                }
                None => {}
            }
        }
        if limit.count > 0 {
            out.push(limit);
        }
    }
    out
}

/// `P + sigma I + rho C^T C`, lower triangle merged in column-major order.
fn system(
    hessian: &[(u32, u32, f64)],
    rows: &[Limit],
    columns: usize,
    rho: f64,
) -> Vec<(u32, u32, f64)> {
    let mut lower: Vec<(u32, u32, f64)> = hessian.to_vec();
    for column in 0..columns {
        let column = u32::try_from(column).unwrap_or(u32::MAX);
        lower.push((column, column, SIGMA));
    }
    for limit in rows {
        for (index, &(a, sa)) in limit.entries().iter().enumerate() {
            for &(b, sb) in limit.entries().iter().skip(index) {
                lower.push((a.min(b), a.max(b), rho * sa * sb));
            }
        }
    }
    lower.sort_by_key(|&(column, row, _)| (column, row));
    lower.dedup_by(|later, earlier| {
        if (later.0, later.1) == (earlier.0, earlier.1) {
            earlier.2 += later.2;
            true
        } else {
            false
        }
    });
    lower
}

/// `C^T w` over the free columns.
fn transpose(rows: &[Limit], w: &[f64], columns: usize) -> Vec<f64> {
    let mut out = vec![0.0; columns];
    for (limit, &value) in rows.iter().zip(w) {
        for &(column, sign) in limit.entries() {
            if let Some(slot) = out.get_mut(column as usize) {
                *slot += sign * value;
            }
        }
    }
    out
}

/// Resumable split state over the problem's free columns.
pub(super) struct Split {
    slots: Vec<Slot>,
    columns: usize,
    hessian: Vec<(u32, u32, f64)>,
    linear: Vec<f64>,
    rows: Vec<Limit>,
    rho: f64,
    factor: Factor,
    values: Vec<f64>,
    auxiliary: Vec<f64>,
    duals: Vec<f64>,
    iteration: u32,
    edges: usize,
}

impl Split {
    /// Factorize the split system once; `None` when nothing is free or limited.
    ///
    /// # Errors
    /// [`Error::SolveUnderdetermined`] when the split system is not positive definite.
    pub(super) fn new(checked: &Checked<'_>, edges: &[Edge]) -> Result<Option<Self>, Error> {
        let mut slots = Vec::with_capacity(checked.unknowns);
        let mut columns = 0_u32;
        for value in &checked.fixed {
            slots.push(if let Some(value) = *value {
                Slot::Fixed(value)
            } else {
                columns += 1;
                Slot::Free {
                    column: columns - 1,
                    offset: 0.0,
                }
            });
        }
        let columns = columns as usize;
        let rows = limits_over(edges, &slots);
        if columns == 0 || rows.is_empty() {
            return Ok(None);
        }
        let (hessian, linear) = normal::assemble(&checked.rows, &slots, columns);
        let rho = 0.1;
        let factor = Factor::new(&system(&hessian, &rows, columns, rho), columns)?;
        let count = rows.len();
        Ok(Some(Self {
            slots,
            columns,
            hessian,
            linear,
            rows,
            rho,
            factor,
            values: vec![0.0; columns],
            auxiliary: vec![0.0; count],
            duals: vec![0.0; count],
            iteration: 0,
            edges: edges.len(),
        }))
    }

    /// Relative primal and dual residuals at the current state.
    fn residuals(&self) -> (f64, f64) {
        let cx: Vec<f64> = self
            .rows
            .iter()
            .map(|limit| limit.apply(&self.values))
            .collect();
        let gap: Vec<f64> = cx.iter().zip(&self.auxiliary).map(|(a, b)| a - b).collect();
        let px = normal::multiply(&self.hessian, &self.values);
        let cty = transpose(&self.rows, &self.duals, self.columns);
        let gradient: Vec<f64> = px
            .iter()
            .zip(&self.linear)
            .zip(&cty)
            .map(|((px, r), c)| px - r + c)
            .collect();
        let primal_scale = normal::norm(&cx)
            .max(normal::norm(&self.auxiliary))
            .max(1.0);
        let dual_scale = normal::norm(&px)
            .max(normal::norm(&cty))
            .max(normal::norm(&self.linear))
            .max(1.0);
        (
            normal::norm(&gap) / primal_scale,
            normal::norm(&gradient) / dual_scale,
        )
    }

    /// Iterate until both relative residuals reach `accuracy`, refactorizing for
    /// `rho` at most `refactors` times. Returns `(reached, factorizations used)`.
    ///
    /// # Errors
    /// [`Error::SolveUnderdetermined`] when a refactorized system is not definite.
    pub(super) fn advance(&mut self, accuracy: f64, refactors: u32) -> Result<(bool, u32), Error> {
        let mut used = 0;
        // Reused across iterations: one rhs and one solve column, no allocation.
        let mut rhs = vec![0.0; self.columns];
        let mut solved = Col::<f64>::zeros(self.columns);
        while self.iteration < ITERATIONS {
            self.iteration += 1;
            for ((slot, x), r) in rhs.iter_mut().zip(&self.values).zip(&self.linear) {
                *slot = SIGMA * x + r;
            }
            for ((limit, z), y) in self.rows.iter().zip(&self.auxiliary).zip(&self.duals) {
                for &(column, sign) in limit.entries() {
                    if let Some(slot) = rhs.get_mut(column as usize) {
                        *slot += sign * (self.rho * z - y);
                    }
                }
            }
            self.factor.solve_into(&rhs, &mut solved);
            let tilde = solved
                .try_as_col_major()
                .map_or(&[][..], |column| column.as_slice());
            for (value, new) in self.values.iter_mut().zip(tilde) {
                *value = RELAXATION * new + (1.0 - RELAXATION) * *value;
            }
            let rho = self.rho;
            for ((limit, z), y) in self
                .rows
                .iter()
                .zip(&mut self.auxiliary)
                .zip(&mut self.duals)
            {
                let relaxed = RELAXATION * limit.apply(tilde) + (1.0 - RELAXATION) * *z;
                let next = (relaxed + *y / rho).min(limit.upper);
                *y += rho * (relaxed - next);
                *z = next;
            }
            if !self.iteration.is_multiple_of(CHECK) {
                continue;
            }
            let (primal, dual) = self.residuals();
            if primal <= accuracy && dual <= accuracy {
                return Ok((true, used));
            }
            if self.iteration.is_multiple_of(ADAPT) && used < refactors {
                let ratio = (primal / dual.max(f64::MIN_POSITIVE)).sqrt();
                if ratio.is_finite() && !(0.2..=5.0).contains(&ratio) {
                    self.rho = (self.rho * ratio).clamp(1e-6, 1e6);
                    self.factor = Factor::new(
                        &system(&self.hessian, &self.rows, self.columns, self.rho),
                        self.columns,
                    )?;
                    used += 1;
                }
            }
        }
        Ok((false, used))
    }

    /// The current active estimate: permanent edges plus every dual-positive
    /// edge whose dual exceeds its slack.
    pub(super) fn active(&self, edges: &[Edge], limits: Limits) -> Vec<bool> {
        let mut active: Vec<bool> = edges.iter().map(|e| e.permanent).collect();
        active.resize(self.edges, false);
        for ((limit, z), y) in self.rows.iter().zip(&self.auxiliary).zip(&self.duals) {
            if *y > (limit.upper - z).max(0.0) + limits.feasibility * 1e-3
                && let Some(flag) = active.get_mut(limit.edge)
            {
                *flag = true;
            }
        }
        active
    }

    /// One value per unknown at the current state.
    pub(super) fn values(&self) -> Vec<f64> {
        normal::expand(&self.slots, &self.values)
    }
}
