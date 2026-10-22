//! Grading ABI v1: weighted sparse least squares with fixed values, bounds and
//! difference limits over caller-owned POD pools. Values are written into one
//! caller-owned array with a summary; nothing is retained or allocated across C.

use crate::borrow::SliceView;
use crate::boundary::{Failure, call, memory_range};
use crate::pools::{self, Range};
use tapioca_geometry::grading as kernel;

#[cfg(test)]
mod tests;

/// One coefficient of one row.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct GradingTerm {
    /// Finite coefficient.
    pub coefficient: f64,
    /// Unknown index.
    pub unknown: u32,
    /// Zero.
    pub reserved: u32,
}

/// One weighted row over `terms[first..first + count]`.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct GradingRow {
    /// Finite right-hand side.
    pub target: f64,
    /// Finite positive weight.
    pub weight: f64,
    /// First term index.
    pub first: u32,
    /// Term count, at least one.
    pub count: u32,
}

/// A given value, eliminated from the system.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct GradingFixed {
    /// Finite value.
    pub value: f64,
    /// Unknown index.
    pub unknown: u32,
    /// Zero.
    pub reserved: u32,
}

/// `lower <= x <= upper`; infinities for an open side.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct GradingBound {
    /// Lower limit.
    pub lower: f64,
    /// Upper limit.
    pub upper: f64,
    /// Unknown index.
    pub unknown: u32,
    /// Zero.
    pub reserved: u32,
}

/// `min <= x_a - x_b <= max`; infinities for an open side.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct GradingDifference {
    /// Lower limit.
    pub min: f64,
    /// Upper limit.
    pub max: f64,
    /// Minuend unknown.
    pub a: u32,
    /// Subtrahend unknown.
    pub b: u32,
}

/// The problem, its limits and mode.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct GradingRequest {
    /// Term pool.
    pub terms: SliceView<GradingTerm>,
    /// Rows over the term pool.
    pub rows: SliceView<GradingRow>,
    /// Fixed unknowns.
    pub fixed: SliceView<GradingFixed>,
    /// Bounds (constrained mode only).
    pub bounds: SliceView<GradingBound>,
    /// Difference limits (constrained mode only).
    pub differences: SliceView<GradingDifference>,
    /// Empty, or one start value per unknown (constrained mode only).
    pub start: SliceView<f64>,
    /// Absolute agreement in the unknowns' units.
    pub feasibility: f64,
    /// Relative normal-equation residual.
    pub residual: f64,
    /// Number of unknowns.
    pub unknowns: u32,
    /// Factorized-solve budget.
    pub max_iterations: u32,
    /// 0 unconstrained (`bounds`, `differences`, `start` empty), 1 constrained.
    pub constrained: u32,
    /// Zero.
    pub reserved: u32,
}

/// What the solve did.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct GradingSummary {
    /// `sum weight * residual^2`.
    pub objective: f64,
    /// Iterations taken.
    pub iterations: u32,
    /// Factorizations performed.
    pub factorizations: u32,
    /// Inequality limits held.
    pub active: u32,
    /// Zero.
    pub reserved: u32,
}

/// Caller-owned outputs: the summary and at least `unknowns` values.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct GradingOutput {
    /// Required. Unchanged on refusal.
    pub summary: *mut GradingSummary,
    /// One value per unknown; unchanged on refusal.
    pub values: *mut f64,
    /// Capacity of `values`, at least `unknowns`.
    pub value_capacity: usize,
}

fn copy<T: Copy, U>(items: &[T], convert: impl Fn(T) -> U) -> Vec<U> {
    items.iter().copied().map(convert).collect()
}

// SAFETY: the request, nested pools, outputs and error are preflighted disjoint
// before any foreign borrow; results are owned before publication.
unsafe fn solve(
    input: *const GradingRequest,
    output: *const GradingOutput,
    error: Range,
) -> Result<(kernel::Solution, GradingOutput), Failure> {
    // SAFETY: descriptors validates both POD shapes before copying them.
    let (request, target, context) = unsafe { pools::descriptors(input, output, error)? };
    let mut inputs = vec![memory_range(input, 1), memory_range(output, 1)];
    if let Some(r) = &request {
        inputs.extend([
            memory_range(r.terms.data, r.terms.count),
            memory_range(r.rows.data, r.rows.count),
            memory_range(r.fixed.data, r.fixed.count),
            memory_range(r.bounds.data, r.bounds.count),
            memory_range(r.differences.data, r.differences.count),
            memory_range(r.start.data, r.start.count),
        ]);
    }
    let outputs = target.map_or_else(Vec::new, |t| {
        vec![
            memory_range(t.summary.cast_const(), 1),
            memory_range(t.values.cast_const(), t.value_capacity),
        ]
    });
    pools::preflight(&inputs, &outputs, error)?;
    let request = request.ok_or(Failure::Argument("GradingInputDescriptor"))?;
    let target = target.ok_or(Failure::Argument("GradingOutputDescriptor"))?;
    if request.constrained > 1
        || (request.constrained == 0
            && (request.bounds.count != 0
                || request.differences.count != 0
                || request.start.count != 0))
    {
        return Err(Failure::Argument(
            "GradingMode: constraints and a start need constrained = 1",
        ));
    }
    if target.value_capacity < request.unknowns as usize {
        return Err(Failure::Argument("GradingOutputTooSmall"));
    }
    // SAFETY: every pool was preflighted against outputs, descriptors and error.
    let (terms, rows, fixed, bounds, differences, start) = unsafe {
        (
            context.slice(request.terms)?,
            context.slice(request.rows)?,
            context.slice(request.fixed)?,
            context.slice(request.bounds)?,
            context.slice(request.differences)?,
            context.slice(request.start)?,
        )
    };
    let terms = copy(terms, |t| kernel::Term {
        unknown: t.unknown,
        coefficient: t.coefficient,
    });
    let rows = copy(rows, |r| kernel::Row {
        first: r.first,
        count: r.count,
        target: r.target,
        weight: r.weight,
    });
    let fixed = copy(fixed, |f| kernel::Fixed {
        unknown: f.unknown,
        value: f.value,
    });
    let problem = kernel::Problem {
        unknowns: request.unknowns,
        terms: &terms,
        rows: &rows,
        fixed: &fixed,
    };
    let limits = kernel::Limits {
        max_iterations: request.max_iterations,
        feasibility: request.feasibility,
        residual: request.residual,
    };
    let solved = if request.constrained == 1 {
        let bounds = copy(bounds, |b| kernel::Bound {
            unknown: b.unknown,
            lower: b.lower,
            upper: b.upper,
        });
        let differences = copy(differences, |d| kernel::Difference {
            a: d.a,
            b: d.b,
            min: d.min,
            max: d.max,
        });
        let constraints = kernel::Constraints {
            bounds: &bounds,
            differences: &differences,
        };
        kernel::solve_constrained_ls(
            &problem,
            constraints,
            limits,
            (!start.is_empty()).then_some(start),
        )
    } else {
        kernel::solve_sparse_ls(&problem, limits)
    };
    Ok((
        solved.map_err(|error| Failure::Argument(error.message()))?,
        target,
    ))
}

// SAFETY: target ranges were preflighted disjoint and large enough.
unsafe fn publish(solution: &kernel::Solution, target: GradingOutput) {
    let summary = GradingSummary {
        objective: solution.objective,
        iterations: solution.iterations,
        factorizations: solution.factorizations,
        active: solution.active,
        reserved: 0,
    };
    // SAFETY: capacity checked before any write; raw POD copies cannot unwind.
    unsafe {
        if !solution.values.is_empty() {
            std::ptr::copy_nonoverlapping(
                solution.values.as_ptr(),
                target.values,
                solution.values.len(),
            );
        }
        target.summary.write(summary);
    }
}

/// Solve weighted sparse least squares, unconstrained or constrained.
///
/// # Safety
/// Request/output descriptors and every nested pool must be live aligned
/// initialized allocations; the summary and full value capacity must be
/// writable, exclusive, and disjoint from every input, descriptor and the error
/// buffer. Null arrays require zero count/capacity. Outputs are unchanged on any
/// refusal. Unwind is contained, not OOM, abort or invalid memory.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_grading_solve(
    input: *const GradingRequest,
    output: *const GradingOutput,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: call validates the error storage and contains panics; preflight
    // precedes computation and publication writes only checked ranges.
    unsafe {
        call(error, capacity, || {
            let (solution, target) =
                solve(input, output, memory_range(error.cast_const(), capacity)?)?;
            publish(&solution, target);
            Ok(())
        })
    }
}
