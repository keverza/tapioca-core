use super::*;

fn request(values: &[f64], mode: u32) -> AffineRequest {
    AffineRequest {
        transform: affine::Transform {
            m: [
                [1.0, 0.0, 0.0, 10.0],
                [0.0, 1.0, 0.0, 0.0],
                [0.0, 0.0, 1.0, 0.0],
            ],
        },
        mode,
        values: SliceView {
            data: values.as_ptr(),
            count: values.len(),
        },
    }
}
fn bits(values: &[f64]) -> Vec<u64> {
    values.iter().copied().map(f64::to_bits).collect()
}

#[test]
fn affine_x64_layout_matches_c_header() {
    assert_eq!(size_of::<affine::Transform>(), 96);
    if size_of::<usize>() == 8 {
        assert_eq!(size_of::<AffineRequest>(), 120);
        assert_eq!(std::mem::offset_of!(AffineRequest, values), 104);
        assert_eq!(size_of::<AffineOutput>(), 24);
    }
}

#[test]
fn affine_commits_only_requested_prefix_and_orientation() {
    let input = [1.0, 2.0, 3.0];
    let request = request(&input, 1);
    let mut values = [99.0; 6];
    let mut flipped = 99;
    let output = AffineOutput {
        data: values.as_mut_ptr(),
        capacity: values.len(),
        flips_orientation: &raw mut flipped,
    };
    let mut error = [0xff; 256];
    // SAFETY: live initialized inputs, aligned writable disjoint output allocations.
    let status = unsafe {
        tapioca_transform_vectors(
            &raw const request,
            &raw const output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 0);
    assert_eq!(flipped, 0);
    assert_eq!(bits(&values), bits(&[11.0, 2.0, 3.0, 99.0, 99.0, 99.0]));
    assert_eq!(bits(&input), bits(&[1.0, 2.0, 3.0]));
    assert_eq!(error.first(), Some(&0));
}

#[test]
fn affine_failures_leave_coordinates_and_report_unchanged() {
    for (input, mode, capacity) in [
        (vec![1.0, 2.0, 3.0], 99, 6),
        (vec![1.0, 2.0], 1, 6),
        (vec![1.0, 2.0, 3.0], 1, 2),
        (vec![1.0, 2.0, 3.0, 4.0, f64::NAN, 6.0], 1, 6),
    ] {
        let request = request(&input, mode);
        let before = bits(&input);
        let mut values = [99.0; 6];
        let mut flipped = 99;
        let output = AffineOutput {
            data: values.as_mut_ptr(),
            capacity,
            flips_orientation: &raw mut flipped,
        };
        let mut error = [0; 256];
        // SAFETY: each fixture retains its live arrays; malformed shape/capacity/mode are refused before writes.
        let status = unsafe {
            tapioca_transform_vectors(
                &raw const request,
                &raw const output,
                error.as_mut_ptr(),
                error.len(),
            )
        };
        assert_eq!(status, 1);
        assert_eq!(flipped, 99);
        assert_eq!(bits(&values), bits(&[99.0; 6]));
        assert_eq!(bits(&input), before);
    }
}

#[test]
fn affine_rejects_output_input_and_output_report_overlap_without_writes() {
    let mut values = [1.0, 2.0, 3.0];
    let request = request(&values, 1);
    let mut flipped = 99;
    let mut output = AffineOutput {
        data: values.as_mut_ptr(),
        capacity: 3,
        flips_orientation: &raw mut flipped,
    };
    let mut error = [0x7f; 256];
    // SAFETY: recognized coordinate/input overlap is rejected before creating a slice or writing.
    let status = unsafe {
        tapioca_transform_vectors(
            &raw const request,
            &raw const output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 1);
    assert_eq!(flipped, 99);
    assert_eq!(bits(&values), bits(&[1.0, 2.0, 3.0]));
    assert_eq!(error, [0x7f; 256]);

    let empty = self::request(&[], 1);
    output.flips_orientation = values.as_mut_ptr().cast();
    // SAFETY: recognized coordinate/report overlap writes neither region.
    let status = unsafe {
        tapioca_transform_vectors(
            &raw const empty,
            &raw const output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 1);
    assert_eq!(bits(&values), bits(&[1.0, 2.0, 3.0]));
}

#[test]
fn affine_empty_coordinates_still_require_a_disjoint_report() {
    let input = request(&[], 1);
    let mut report = [99_u32; 64];
    let mut output = AffineOutput {
        data: std::ptr::null_mut(),
        capacity: 0,
        flips_orientation: report.as_mut_ptr(),
    };
    // SAFETY: recognized report/error overlap returns without touching either output.
    let status = unsafe {
        tapioca_transform_vectors(
            &raw const input,
            &raw const output,
            report.as_mut_ptr().cast(),
            256,
        )
    };
    assert_eq!(status, 1);
    assert_eq!(report, [99; 64]);
    let mut error = [0; 256];
    output.flips_orientation = std::ptr::null_mut();
    // SAFETY: explicit null required-report shape is rejected before dereference.
    let status = unsafe {
        tapioca_transform_vectors(
            &raw const input,
            &raw const output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 1);
    output.flips_orientation = report.as_mut_ptr();
    // SAFETY: valid empty coordinate storage and one exclusive aligned report.
    let status = unsafe {
        tapioca_transform_vectors(
            &raw const input,
            &raw const output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 0);
    assert_eq!(report.first(), Some(&0));
}

#[test]
fn affine_foreign_shape_overflow_refuses_before_access() {
    let mut input = request(&[], 1);
    input.values = SliceView {
        data: std::ptr::NonNull::<f64>::dangling().as_ptr(),
        count: usize::MAX,
    };
    let mut flipped = 99;
    let output = AffineOutput {
        data: std::ptr::null_mut(),
        capacity: 0,
        flips_orientation: &raw mut flipped,
    };
    let mut error = [0; 256];
    // SAFETY: deliberately overflowing shape is refused before constructing an input slice.
    let status = unsafe {
        tapioca_transform_vectors(
            &raw const input,
            &raw const output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 1);
    assert_eq!(flipped, 99);
}
