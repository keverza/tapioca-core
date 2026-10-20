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
fn composition_commits_complete_matrix_and_accepts_empty_null_input() {
    let input = request(&[], 1).transform;
    let mut output = affine::Transform { m: [[99.0; 4]; 3] };
    let mut error = [0xff; 128];
    // SAFETY: initialized input and disjoint exclusive output/error allocations.
    let status = unsafe {
        tapioca_compose_transforms(
            &raw const input,
            1,
            &raw mut output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 0);
    assert_eq!(output, input);
    assert_eq!(error.first(), Some(&0));
    // SAFETY: null/zero is valid empty input; output and error remain disjoint.
    let status = unsafe {
        tapioca_compose_transforms(
            std::ptr::null(),
            0,
            &raw mut output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 0);
    assert_eq!(output, affine::Transform::IDENTITY);
}

#[test]
fn composition_rejects_malformed_input_without_changing_output() {
    let mut invalid = affine::Transform::IDENTITY;
    invalid.m = [[f64::NAN; 4]; 3];
    let mut output = affine::Transform { m: [[99.0; 4]; 3] };
    let before = output;
    let mut error = [0; 128];
    for (data, count) in [
        (std::ptr::null(), 1),
        (&raw const invalid, 1),
        (
            std::ptr::NonNull::<affine::Transform>::dangling()
                .as_ptr()
                .cast_const(),
            usize::MAX,
        ),
    ] {
        // SAFETY: malformed shapes are refused before access; invalid is live initialized POD.
        let status = unsafe {
            tapioca_compose_transforms(
                data,
                count,
                &raw mut output,
                error.as_mut_ptr(),
                error.len(),
            )
        };
        assert_eq!(status, 1);
        assert_eq!(output, before);
        assert_ne!(error.first(), Some(&0));
    }
    let mut overflow = affine::Transform::IDENTITY;
    overflow.m = [
        [1.0, 0.0, 0.0, f64::MAX],
        [0.0, 1.0, 0.0, 0.0],
        [0.0, 0.0, 1.0, 0.0],
    ];
    let chain = [overflow, overflow];
    // SAFETY: valid disjoint arrays; arithmetic overflow refuses after borrowing.
    let status = unsafe {
        tapioca_compose_transforms(
            chain.as_ptr(),
            chain.len(),
            &raw mut output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 1);
    assert_eq!(output, before);
}

#[test]
fn composition_rejects_all_writable_overlap_without_writes() {
    let mut input = affine::Transform::IDENTITY;
    let before = input;
    let mut error = [0x7f; 128];
    // SAFETY: raw recognized input/output overlap refuses before borrowing or writing.
    let status = unsafe {
        tapioca_compose_transforms(
            &raw const input,
            1,
            &raw mut input,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 1);
    assert_eq!(input, before);
    assert_eq!(error, [0x7f; 128]);
    let mut output = before;
    // SAFETY: recognized input/error overlap refuses before a borrow or error write.
    let status = unsafe {
        tapioca_compose_transforms(
            &raw const input,
            1,
            &raw mut output,
            (&raw mut input).cast(),
            size_of::<affine::Transform>(),
        )
    };
    assert_eq!(status, 1);
    assert_eq!(input, before);
    assert_eq!(output, before);
    // SAFETY: recognized output/error overlap refuses before either write.
    let status = unsafe {
        tapioca_compose_transforms(
            std::ptr::null(),
            0,
            &raw mut output,
            (&raw mut output).cast(),
            size_of::<affine::Transform>(),
        )
    };
    assert_eq!(status, 1);
    assert_eq!(output, before);
}

#[test]
fn composition_rejects_null_or_misaligned_output_and_malformed_error() {
    let mut error = [0; 128];
    // SAFETY: explicit null output is rejected before dereference.
    let status = unsafe {
        tapioca_compose_transforms(
            std::ptr::null(),
            0,
            std::ptr::null_mut(),
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 1);
    let mut storage = [99_u64; 13];
    // SAFETY: explicit misalignment is rejected before dereference; allocation is live.
    let status = unsafe {
        tapioca_compose_transforms(
            std::ptr::null(),
            0,
            storage.as_mut_ptr().cast::<u8>().wrapping_add(1).cast(),
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 1);
    assert_eq!(storage, [99; 13]);
    let mut output = affine::Transform { m: [[99.0; 4]; 3] };
    let before = output;
    // SAFETY: null/nonzero error shape refuses before any publication.
    let status = unsafe {
        tapioca_compose_transforms(
            std::ptr::null(),
            0,
            &raw mut output,
            std::ptr::null_mut(),
            1,
        )
    };
    assert_eq!(status, 1);
    assert_eq!(output, before);
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
