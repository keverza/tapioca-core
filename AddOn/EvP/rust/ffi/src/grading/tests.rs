use super::*;

fn view<T>(values: &[T]) -> SliceView<T> {
    SliceView {
        data: values.as_ptr(),
        count: values.len(),
    }
}

fn empty<T>() -> SliceView<T> {
    SliceView {
        data: std::ptr::null(),
        count: 0,
    }
}

const SENTINEL: f64 = -7.25;

/// Chain 0-1-2-3-4 smoothness rows with both ends fixed: a linear ramp.
struct Chain {
    terms: Vec<GradingTerm>,
    rows: Vec<GradingRow>,
    fixed: Vec<GradingFixed>,
}

fn chain() -> Chain {
    let mut terms = Vec::new();
    let mut rows = Vec::new();
    for i in 0..4_u32 {
        terms.push(GradingTerm {
            coefficient: 1.0,
            unknown: i,
            reserved: 0,
        });
        terms.push(GradingTerm {
            coefficient: -1.0,
            unknown: i + 1,
            reserved: 0,
        });
        rows.push(GradingRow {
            target: 0.0,
            weight: 1.0,
            first: i * 2,
            count: 2,
        });
    }
    let fixed = vec![
        GradingFixed {
            value: 0.0,
            unknown: 0,
            reserved: 0,
        },
        GradingFixed {
            value: 4.0,
            unknown: 4,
            reserved: 0,
        },
    ];
    Chain { terms, rows, fixed }
}

fn request(chain: &Chain) -> GradingRequest {
    GradingRequest {
        terms: view(&chain.terms),
        rows: view(&chain.rows),
        fixed: view(&chain.fixed),
        bounds: empty(),
        differences: empty(),
        start: empty(),
        feasibility: 1e-9,
        residual: 1e-10,
        unknowns: 5,
        max_iterations: 50,
        constrained: 0,
        reserved: 0,
    }
}

fn run(input: &GradingRequest, values: &mut [f64], summary: &mut GradingSummary) -> (i32, String) {
    let output = GradingOutput {
        summary: std::ptr::from_mut(summary),
        values: values.as_mut_ptr(),
        value_capacity: values.len(),
    };
    let mut error = [0_u8; 160];
    // SAFETY: every pool is a live local; outputs and error are disjoint locals.
    let status = unsafe {
        tapioca_grading_solve(
            std::ptr::from_ref(input),
            &raw const output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    let length = error.iter().position(|&b| b == 0).unwrap_or(error.len());
    (
        status,
        String::from_utf8_lossy(error.get(..length).unwrap_or(&[])).into_owned(),
    )
}

#[test]
fn layouts_are_the_documented_c_records() {
    assert_eq!(size_of::<GradingTerm>(), 16);
    assert_eq!(size_of::<GradingRow>(), 24);
    assert_eq!(size_of::<GradingFixed>(), 16);
    assert_eq!(size_of::<GradingBound>(), 24);
    assert_eq!(size_of::<GradingDifference>(), 24);
    assert_eq!(size_of::<GradingSummary>(), 24);
    if size_of::<usize>() == 8 {
        assert_eq!(size_of::<GradingRequest>(), 128);
        assert_eq!(size_of::<GradingOutput>(), 24);
    }
}

#[test]
fn an_unconstrained_solve_publishes_the_kernel_values_and_leaves_the_tail() {
    let chain = chain();
    let mut values = [SENTINEL; 7];
    let mut summary = GradingSummary::default();
    let (status, message) = run(&request(&chain), &mut values, &mut summary);
    assert_eq!((status, message.as_str()), (0, ""));
    for (index, expected) in [0.0, 1.0, 2.0, 3.0, 4.0].iter().enumerate() {
        assert!((values.get(index).copied().unwrap_or(f64::NAN) - expected).abs() < 1e-12);
    }
    assert_eq!(values.get(5..), Some(&[SENTINEL, SENTINEL][..]));
    assert_eq!(
        (summary.iterations, summary.factorizations, summary.active),
        (1, 1, 0)
    );
    assert!(summary.objective > 0.0);
}

#[test]
fn constrained_mode_applies_bounds_and_a_start() {
    let chain = chain();
    let bounds = [GradingBound {
        lower: f64::NEG_INFINITY,
        upper: 1.5,
        unknown: 2,
        reserved: 0,
    }];
    let start = [0.0, 0.5, 1.0, 2.0, 4.0];
    let input = GradingRequest {
        bounds: view(&bounds),
        start: view(&start),
        constrained: 1,
        ..request(&chain)
    };
    let mut values = [SENTINEL; 5];
    let mut summary = GradingSummary::default();
    let (status, _) = run(&input, &mut values, &mut summary);
    assert_eq!(status, 0);
    assert!((values.get(2).copied().unwrap_or(f64::NAN) - 1.5).abs() < 1e-10);
    assert_eq!(summary.active, 1);
}

#[test]
fn refusals_leave_every_output_unchanged() {
    let chain = chain();
    let contradictory = [
        GradingFixed {
            value: 0.0,
            unknown: 0,
            reserved: 0,
        },
        GradingFixed {
            value: 1.0,
            unknown: 0,
            reserved: 0,
        },
    ];
    let bounds = [GradingBound {
        lower: 0.0,
        upper: 1.0,
        unknown: 1,
        reserved: 0,
    }];
    let cases: [(GradingRequest, &str); 3] = [
        (
            GradingRequest {
                fixed: view(&contradictory),
                ..request(&chain)
            },
            "SolveInconsistent",
        ),
        (
            GradingRequest {
                bounds: view(&bounds),
                ..request(&chain)
            },
            "GradingMode",
        ),
        (
            GradingRequest {
                constrained: 2,
                ..request(&chain)
            },
            "GradingMode",
        ),
    ];
    for (input, code) in cases {
        let mut values = [SENTINEL; 5];
        let mut summary = GradingSummary {
            objective: SENTINEL,
            ..GradingSummary::default()
        };
        let (status, message) = run(&input, &mut values, &mut summary);
        assert_eq!(status, 1, "{code}");
        assert!(message.starts_with(code), "{message}");
        assert_eq!(values, [SENTINEL; 5]);
        assert!((summary.objective - SENTINEL).abs() < f64::EPSILON);
    }
    let mut short = [SENTINEL; 4];
    let mut summary = GradingSummary::default();
    let (status, message) = run(&request(&chain), &mut short, &mut summary);
    assert_eq!((status, message.as_str()), (1, "GradingOutputTooSmall"));
    assert_eq!(short, [SENTINEL; 4]);
}

#[test]
fn aliased_or_missing_outputs_are_refused_without_writes() {
    let chain = chain();
    let mut terms = chain.terms.clone();
    let input = GradingRequest {
        terms: view(&terms),
        ..request(&chain)
    };
    let mut summary = GradingSummary::default();
    let output = GradingOutput {
        summary: &raw mut summary,
        values: terms.as_mut_ptr().cast(),
        value_capacity: 5,
    };
    let mut error = [0xAA_u8; 64];
    // SAFETY: the alias is deliberate; preflight must refuse it before any write.
    let status = unsafe {
        tapioca_grading_solve(
            &raw const input,
            &raw const output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 1);
    assert_eq!(terms, chain.terms);
    let mut values = [SENTINEL; 5];
    let missing = GradingOutput {
        summary: std::ptr::null_mut(),
        values: values.as_mut_ptr(),
        value_capacity: 5,
    };
    let base = request(&chain);
    // SAFETY: a null summary is a malformed descriptor, refused before writes.
    let status = unsafe {
        tapioca_grading_solve(
            &raw const base,
            &raw const missing,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 1);
    assert_eq!(values, [SENTINEL; 5]);
}
