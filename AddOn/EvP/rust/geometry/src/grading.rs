//! S3b.1 grading solve (G12): sparse least squares with fixed values, bounds and
//! difference limits, solved once in Rust.
//!
//! **Ownership.** Tapioca owns the objective, the constraints, their elimination,
//! the active set and every refusal. `faer` (pinned, vendored, no default
//! features) only factorizes the reduced normal equations: sparse Cholesky with
//! its AMD fill-reducing ordering, sequential. No `faer` type is public.
//!
//! **Problem.** Unknowns `x` (metres when they are heights) minimize
//! `sum_r weight_r * (sum_t coefficient_t * x_t - target_r)^2` over caller rows.
//! [`Fixed`] values are eliminated from the system, never penalized. Constrained
//! solves add [`Bound`]s (`lower <= x_i <= upper`, cut/fill limits) and
//! [`Difference`] limits (`min <= x_a - x_b <= max`, slope limits).
//!
//! **Constrained method.** Every constraint involves one or two unknowns, so the
//! working set is a graph. A feasible start comes from shortest paths over the
//! constraint graph (Bellman-Ford with a queue), which also proves infeasibility
//! exactly ([`Error::SolveInconsistent`]). Active constraints merge unknowns into
//! groups with exact offsets along a spanning tree, so every subproblem is the
//! same symmetric positive definite reduced system. Three phases share one
//! factorization budget: a cold primal-dual active set (settles in a few
//! factorizations with bounds), an operator-splitting estimate polished at
//! tightening accuracies (steep meshes, where tight edges close cycles), and a
//! primal active set as the exact fallback. A result is returned only at a
//! certified KKT point: no edge violated beyond [`Limits::feasibility`], and
//! non-negative multipliers on the tight edges balancing the gradient, decided
//! by a maximum flow because cyclic multipliers are not unique.
//!
//! **Measured** (`ffi/examples/grading_measure.rs`, 10k unknowns, release): an
//! unconstrained solve is one factorization (14-21 ms median); 3621 binding
//! cut/fill bounds take six (about 80 ms); a slope limit with about 7100 binding
//! edges takes seven plus the split (about 260 ms median, 284 ms p95).
//!
//! **Determinism.** Row terms, triplets, constraint scans and tie-breaks have fixed
//! orders; duplicate triplets are merged by a stable sort. Results are
//! bit-reproducible for fixed input on one machine (`faer` picks SIMD kernels by
//! CPU feature, so not across CPU generations). Worker-classified; allocates.
//!
//! **Atomicity.** A refusal returns no partial values; callers keep their previous
//! surface.

mod active;
mod admm;
mod certify;
mod feasible;
mod normal;
mod slope_limit;

#[cfg(test)]
mod tests;

pub use slope_limit::{SlopeLimitRequest, SlopeLimited, reduce_slope};

/// Recorded with every result: what factorized and in which order.
pub const METHOD: &str = "faer 0.24.4 sparse LLT, AMD ordering, sequential; reduced normal equations; \
     graph active set (primal-dual, split-polished, primal); max-flow KKT certificate";

/// One coefficient of one row.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Term {
    /// Unknown index.
    pub unknown: u32,
    /// Finite coefficient. Repeated unknowns within a row are summed.
    pub coefficient: f64,
}

/// One weighted least-squares row over `terms[first..first + count]`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Row {
    /// First term index.
    pub first: u32,
    /// Number of terms, at least one.
    pub count: u32,
    /// Finite right-hand side.
    pub target: f64,
    /// Finite positive weight.
    pub weight: f64,
}

/// An unknown whose value is given; eliminated from the system.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Fixed {
    /// Unknown index. Repeats must agree within the feasibility tolerance.
    pub unknown: u32,
    /// Finite value.
    pub value: f64,
}

/// Borrowed least-squares problem.
#[derive(Clone, Copy, Debug)]
pub struct Problem<'a> {
    /// Number of unknowns, at least one.
    pub unknowns: u32,
    /// Term pool addressed by [`Row::first`]/[`Row::count`].
    pub terms: &'a [Term],
    /// Rows in caller order.
    pub rows: &'a [Row],
    /// Fixed unknowns.
    pub fixed: &'a [Fixed],
}

/// `lower <= x_unknown <= upper`; either side may be infinite.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Bound {
    /// Unknown index.
    pub unknown: u32,
    /// Lower limit, `-inf` for none.
    pub lower: f64,
    /// Upper limit, `+inf` for none.
    pub upper: f64,
}

/// `min <= x_a - x_b <= max`; either side may be infinite.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Difference {
    /// Minuend unknown.
    pub a: u32,
    /// Subtrahend unknown, distinct from `a`.
    pub b: u32,
    /// Lower limit, `-inf` for none.
    pub min: f64,
    /// Upper limit, `+inf` for none.
    pub max: f64,
}

/// Inequalities for [`solve_constrained_ls`].
#[derive(Clone, Copy, Debug, Default)]
pub struct Constraints<'a> {
    /// Per-unknown limits.
    pub bounds: &'a [Bound],
    /// Pairwise difference limits.
    pub differences: &'a [Difference],
}

/// Budgets and tolerances, each with a stated meaning.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Limits {
    /// Constrained: factorized solves across all phases (active-set iterations
    /// plus split factorizations). Unconstrained: refinement steps. At least one;
    /// exhaustion is [`Error::SolveNotConverged`].
    pub max_iterations: u32,
    /// Absolute agreement in the unknowns' units (metres for heights) for fixed
    /// repeats, constraint satisfaction and ties.
    pub feasibility: f64,
    /// Relative normal-equation residual a factorized solve must reach.
    pub residual: f64,
}

impl Default for Limits {
    fn default() -> Self {
        Self {
            max_iterations: 200,
            feasibility: 1e-9,
            residual: 1e-10,
        }
    }
}

/// A solved surface or field.
#[derive(Clone, Debug, PartialEq)]
pub struct Solution {
    /// One value per unknown.
    pub values: Vec<f64>,
    /// Active-set iterations (constrained) or factorized solves (unconstrained).
    pub iterations: u32,
    /// Sparse factorizations performed.
    pub factorizations: u32,
    /// Inequality limits held at the solution: the binding working set, or every
    /// tight limit when the flow certificate decided optimality.
    pub active: u32,
    /// `sum weight * residual^2` at the solution.
    pub objective: f64,
    /// [`METHOD`].
    pub method: &'static str,
}

/// Refusals. Nothing partial is returned.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// No unknowns, an empty or out-of-range row, or a count beyond 32 bits.
    Shape,
    /// A coefficient, target, fixed value or start value is NaN or infinite.
    NonFinite,
    /// An index names an unknown that does not exist, or a difference uses one
    /// unknown twice.
    Index,
    /// A weight is not finite and positive.
    Weight,
    /// A limit is NaN, inverted or excludes every value.
    Bounds,
    /// Budgets or tolerances are not positive and finite.
    Limits,
    /// Fixed values, bounds and difference limits contradict each other.
    SolveInconsistent,
    /// A free unknown is not determined by the rows (singular system).
    SolveUnderdetermined,
    /// The iteration budget or residual requirement was not met.
    SolveNotConverged,
}

impl Error {
    /// Static diagnostic beginning with the stable refusal code.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::Shape => "GradingShape: unknowns, rows and terms must be non-empty and in range",
            Self::NonFinite => "GradingNonFinite: coefficients, targets and values must be finite",
            Self::Index => "GradingIndex: an index names a missing unknown or repeats one",
            Self::Weight => "GradingWeight: weights must be finite and positive",
            Self::Bounds => "GradingBounds: limits must be ordered and admit a value",
            Self::Limits => "GradingLimits: iterations and tolerances must be positive and finite",
            Self::SolveInconsistent => {
                "SolveInconsistent: fixed values, bounds and difference limits contradict each other"
            }
            Self::SolveUnderdetermined => {
                "SolveUnderdetermined: a free unknown is not determined by the rows"
            }
            Self::SolveNotConverged => {
                "SolveNotConverged: iteration budget or residual requirement not met"
            }
        }
    }
}

/// Validated, index-safe view of a problem.
struct Checked<'a> {
    unknowns: usize,
    rows: Vec<(&'a [Term], f64, f64)>,
    fixed: Vec<Option<f64>>,
}

fn check_limits(limits: Limits) -> Result<(), Error> {
    let positive = |value: f64| value.is_finite() && value > 0.0;
    if limits.max_iterations == 0 || !positive(limits.feasibility) || !positive(limits.residual) {
        return Err(Error::Limits);
    }
    Ok(())
}

fn check<'a>(problem: &Problem<'a>, limits: Limits) -> Result<Checked<'a>, Error> {
    check_limits(limits)?;
    let unknowns = problem.unknowns as usize;
    if unknowns == 0
        || u32::try_from(problem.terms.len()).is_err()
        || u32::try_from(problem.rows.len()).is_err()
    {
        return Err(Error::Shape);
    }
    let mut rows = Vec::with_capacity(problem.rows.len());
    for row in problem.rows {
        let start = row.first as usize;
        let end = start.checked_add(row.count as usize).ok_or(Error::Shape)?;
        let terms = problem.terms.get(start..end).ok_or(Error::Shape)?;
        if terms.is_empty() {
            return Err(Error::Shape);
        }
        if terms.iter().any(|term| term.unknown >= problem.unknowns) {
            return Err(Error::Index);
        }
        if !row.target.is_finite() || terms.iter().any(|term| !term.coefficient.is_finite()) {
            return Err(Error::NonFinite);
        }
        if !(row.weight.is_finite() && row.weight > 0.0) {
            return Err(Error::Weight);
        }
        rows.push((terms, row.target, row.weight));
    }
    let mut fixed: Vec<Option<f64>> = vec![None; unknowns];
    for given in problem.fixed {
        let slot = fixed.get_mut(given.unknown as usize).ok_or(Error::Index)?;
        if !given.value.is_finite() {
            return Err(Error::NonFinite);
        }
        match *slot {
            Some(previous) if (previous - given.value).abs() > limits.feasibility => {
                return Err(Error::SolveInconsistent);
            }
            Some(_) => {}
            None => *slot = Some(given.value),
        }
    }
    Ok(Checked {
        unknowns,
        rows,
        fixed,
    })
}

/// `sum weight * residual^2` and the gradient `sum weight * a * residual`.
fn objective(rows: &[(&[Term], f64, f64)], values: &[f64]) -> (f64, Vec<f64>) {
    let mut total = 0.0;
    let mut gradient = vec![0.0; values.len()];
    for &(terms, target, weight) in rows {
        let residual = terms
            .iter()
            .map(|term| {
                term.coefficient * values.get(term.unknown as usize).copied().unwrap_or(0.0)
            })
            .sum::<f64>()
            - target;
        total += weight * residual * residual;
        for term in terms {
            if let Some(slot) = gradient.get_mut(term.unknown as usize) {
                *slot += weight * term.coefficient * residual;
            }
        }
    }
    (total, gradient)
}

/// Unconstrained weighted sparse least squares with fixed values eliminated.
///
/// # Errors
/// See [`Error`]; nothing partial is returned.
pub fn solve_sparse_ls(problem: &Problem<'_>, limits: Limits) -> Result<Solution, Error> {
    let checked = check(problem, limits)?;
    let mut slots = Vec::with_capacity(checked.unknowns);
    let mut columns = 0_u32;
    for value in &checked.fixed {
        slots.push(if let Some(value) = *value {
            normal::Slot::Fixed(value)
        } else {
            columns += 1;
            normal::Slot::Free {
                column: columns - 1,
                offset: 0.0,
            }
        });
    }
    let solved = normal::solve(&checked.rows, &slots, columns as usize, limits)?;
    let values = normal::expand(&slots, &solved.columns);
    let (total, _) = objective(&checked.rows, &values);
    Ok(Solution {
        values,
        iterations: solved.solves,
        factorizations: u32::from(columns > 0),
        active: 0,
        objective: total,
        method: METHOD,
    })
}

/// Weighted sparse least squares subject to bounds and difference limits.
///
/// `start`, when given, must hold one finite value per unknown; the feasible
/// start is the nearest point below it that the shortest-path construction
/// reaches, so a good start (the current surface) saves iterations.
///
/// # Errors
/// See [`Error`]; nothing partial is returned.
pub fn solve_constrained_ls(
    problem: &Problem<'_>,
    constraints: Constraints<'_>,
    limits: Limits,
    start: Option<&[f64]>,
) -> Result<Solution, Error> {
    let checked = check(problem, limits)?;
    let edges = feasible::edges(&checked, constraints)?;
    let initial = match start {
        Some(values) if values.len() != checked.unknowns => return Err(Error::Shape),
        Some(values) if values.iter().any(|value| !value.is_finite()) => {
            return Err(Error::NonFinite);
        }
        Some(values) => values.to_vec(),
        None => checked
            .fixed
            .iter()
            .map(|value| value.unwrap_or(0.0))
            .collect(),
    };
    let start = feasible::start(checked.unknowns, &edges, &initial, limits)?;
    active::solve(&checked, &edges, &start, limits)
}
