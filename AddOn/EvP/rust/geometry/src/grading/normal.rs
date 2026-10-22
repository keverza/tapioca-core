//! Reduced normal equations and their sparse Cholesky factorization.
//!
//! Each unknown is a [`Slot`]: a fixed value, or a free column plus a constant
//! offset (an active-set group shares one column). Rows are rewritten over
//! columns, constants move to the right-hand side, and `N = A^T W A` is
//! assembled lower-triangular, merged by a stable sort and factorized by `faer`.
//! A solve is refined against the assembled `N` until the relative residual
//! meets [`Limits::residual`].

use super::{Error, Limits, Term};
use faer::linalg::solvers::Solve;
use faer::sparse::{SparseColMat, Triplet};
use faer::{Col, Side};

/// Where one unknown's value comes from.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(super) enum Slot {
    /// A given value.
    Fixed(f64),
    /// `x = y[column] + offset`.
    Free { column: u32, offset: f64 },
}

/// Free column values and the number of factorized solves taken.
pub(super) struct Solved {
    pub(super) columns: Vec<f64>,
    pub(super) solves: u32,
}

/// Values of every unknown for column values `columns`.
pub(super) fn expand(slots: &[Slot], columns: &[f64]) -> Vec<f64> {
    slots
        .iter()
        .map(|slot| match *slot {
            Slot::Fixed(value) => value,
            Slot::Free { column, offset } => {
                columns.get(column as usize).copied().unwrap_or(0.0) + offset
            }
        })
        .collect()
}

/// One row over free columns: merged `(column, coefficient)` entries, the
/// right-hand side with constants moved over, and its weight.
fn reduce(terms: &[Term], target: f64, slots: &[Slot], entries: &mut Vec<(u32, f64)>) -> f64 {
    entries.clear();
    let mut rhs = target;
    for term in terms {
        match slots.get(term.unknown as usize) {
            Some(Slot::Fixed(value)) => rhs -= term.coefficient * value,
            Some(&Slot::Free { column, offset }) => {
                rhs -= term.coefficient * offset;
                entries.push((column, term.coefficient));
            }
            None => {}
        }
    }
    entries.sort_by_key(|&(column, _)| column);
    entries.dedup_by(|later, earlier| {
        if later.0 == earlier.0 {
            earlier.1 += later.1;
            true
        } else {
            false
        }
    });
    entries.retain(|&(_, coefficient)| coefficient != 0.0);
    rhs
}

/// Lower triangle of `N` (merged, column-major sorted) and the right-hand side.
pub(super) fn assemble(
    rows: &[(&[Term], f64, f64)],
    slots: &[Slot],
    columns: usize,
) -> (Vec<(u32, u32, f64)>, Vec<f64>) {
    let mut lower = Vec::new();
    let mut rhs = vec![0.0; columns];
    let mut entries = Vec::new();
    for &(terms, target, weight) in rows {
        let reduced = reduce(terms, target, slots, &mut entries);
        for (index, &(column, coefficient)) in entries.iter().enumerate() {
            if let Some(slot) = rhs.get_mut(column as usize) {
                *slot += weight * coefficient * reduced;
            }
            for &(row, other) in entries.iter().skip(index) {
                lower.push((column, row, weight * coefficient * other));
            }
        }
    }
    // Stable: equal (column, row) keys keep row order, so the sums are fixed.
    lower.sort_by_key(|&(column, row, _)| (column, row));
    lower.dedup_by(|later, earlier| {
        if (later.0, later.1) == (earlier.0, earlier.1) {
            earlier.2 += later.2;
            true
        } else {
            false
        }
    });
    (lower, rhs)
}

/// `N * y` from the stored lower triangle.
pub(super) fn multiply(lower: &[(u32, u32, f64)], y: &[f64]) -> Vec<f64> {
    let mut product = vec![0.0; y.len()];
    for &(column, row, value) in lower {
        let (c, r) = (column as usize, row as usize);
        let (yc, yr) = (
            y.get(c).copied().unwrap_or(0.0),
            y.get(r).copied().unwrap_or(0.0),
        );
        if let Some(slot) = product.get_mut(r) {
            *slot += value * yc;
        }
        if r != c
            && let Some(slot) = product.get_mut(c)
        {
            *slot += value * yr;
        }
    }
    product
}

pub(super) fn norm(values: &[f64]) -> f64 {
    values
        .iter()
        .fold(0.0_f64, |most, value| most.max(value.abs()))
}

fn column(values: &[f64]) -> Col<f64> {
    Col::from_fn(values.len(), |i| values.get(i).copied().unwrap_or(0.0))
}

fn values(column: &Col<f64>) -> Vec<f64> {
    column.iter().copied().collect()
}

/// A sparse Cholesky factorization of a lower-triangle matrix, reusable.
pub(super) struct Factor {
    llt: faer::sparse::linalg::solvers::Llt<usize, f64>,
}

impl Factor {
    /// Factorize `lower` (merged, column-major sorted) of a `columns` square matrix.
    ///
    /// # Errors
    /// [`Error::SolveUnderdetermined`] when the matrix is not positive definite.
    pub(super) fn new(lower: &[(u32, u32, f64)], columns: usize) -> Result<Self, Error> {
        let triplets: Vec<Triplet<usize, usize, f64>> = lower
            .iter()
            .map(|&(column, row, value)| Triplet::new(row as usize, column as usize, value))
            .collect();
        let matrix = SparseColMat::<usize, f64>::try_new_from_triplets(columns, columns, &triplets)
            .map_err(|_| Error::Shape)?;
        let llt = matrix
            .sp_cholesky(Side::Lower)
            .map_err(|_| Error::SolveUnderdetermined)?;
        Ok(Self { llt })
    }

    /// `M^-1 * rhs`.
    pub(super) fn solve(&self, rhs: &[f64]) -> Vec<f64> {
        values(&self.llt.solve(column(rhs)))
    }

    /// `M^-1 * rhs` into a reused column of the same length (no allocation).
    pub(super) fn solve_into(&self, rhs: &[f64], out: &mut Col<f64>) {
        for (slot, value) in out.iter_mut().zip(rhs) {
            *slot = *value;
        }
        self.llt.solve_in_place(out.as_mut());
    }
}

/// Solve the reduced system for its free columns.
///
/// # Errors
/// [`Error::SolveUnderdetermined`] when a column carries no row or the matrix is
/// not positive definite; [`Error::SolveNotConverged`] when refinement exhausts
/// [`Limits::max_iterations`] above [`Limits::residual`].
pub(super) fn solve(
    rows: &[(&[Term], f64, f64)],
    slots: &[Slot],
    columns: usize,
    limits: Limits,
) -> Result<Solved, Error> {
    if columns == 0 {
        return Ok(Solved {
            columns: Vec::new(),
            solves: 0,
        });
    }
    let (lower, rhs) = assemble(rows, slots, columns);
    let mut diagonal = vec![0.0_f64; columns];
    for &(column, row, value) in &lower {
        if column == row
            && let Some(slot) = diagonal.get_mut(column as usize)
        {
            *slot = value;
        }
    }
    if diagonal.iter().any(|&value| value.is_nan() || value <= 0.0) {
        return Err(Error::SolveUnderdetermined);
    }
    let llt = Factor::new(&lower, columns)?;
    let mut y = llt.solve(&rhs);
    let scale = norm(&rhs)
        .max(norm(&diagonal) * norm(&y))
        .max(f64::MIN_POSITIVE);
    let mut solves = 1;
    loop {
        let product = multiply(&lower, &y);
        let residual: Vec<f64> = rhs.iter().zip(&product).map(|(b, ny)| b - ny).collect();
        if !y.iter().all(|value| value.is_finite()) {
            return Err(Error::SolveUnderdetermined);
        }
        if norm(&residual) <= limits.residual * scale {
            return Ok(Solved { columns: y, solves });
        }
        if solves >= limits.max_iterations {
            return Err(Error::SolveNotConverged);
        }
        let correction = llt.solve(&residual);
        for (value, delta) in y.iter_mut().zip(correction) {
            *value += delta;
        }
        solves += 1;
    }
}
