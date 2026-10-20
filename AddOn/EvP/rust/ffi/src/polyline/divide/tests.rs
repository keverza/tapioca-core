use super::*;

const POINTS: [f64; 6] = [0.0, 0.0, 0.0, 0.0, 0.0, 8.0];
fn request() -> DividePolylineRequest {
    DividePolylineRequest {
        points: SliceView {
            data: POINTS.as_ptr(),
            count: POINTS.len(),
        },
        count: 2,
        include_ends: 1,
    }
}

#[test]
fn layout_prefix_and_uninitialized_output() {
    if size_of::<usize>() == 8 {
        assert_eq!(size_of::<DividePolylineRequest>(), 24);
        assert_eq!(std::mem::offset_of!(DividePolylineRequest, count), 16);
        assert_eq!(
            std::mem::offset_of!(DividePolylineRequest, include_ends),
            20
        );
    }
    let input = request();
    let mut output = [99.0; 12];
    let mut error = [0x7f; 128];
    // SAFETY: live immutable POD/points, disjoint exclusive writable outputs.
    let status = unsafe {
        tapioca_divide_polyline(
            &raw const input,
            output.as_mut_ptr(),
            12,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 0);
    assert_eq!(error.first(), Some(&0));
    assert_eq!(
        output.map(f64::to_bits),
        [
            0.0, 0.0, 0.0, 0.0, 0.0, 4.0, 0.0, 0.0, 8.0, 99.0, 99.0, 99.0
        ]
        .map(f64::to_bits)
    );
    let mut storage = std::mem::MaybeUninit::<[f64; 9]>::uninit();
    // SAFETY: full aligned exclusive output is initialized on successful publication.
    let status = unsafe {
        tapioca_divide_polyline(
            &raw const input,
            storage.as_mut_ptr().cast(),
            9,
            std::ptr::null_mut(),
            0,
        )
    };
    assert_eq!(status, 0);
    // SAFETY: successful call initialized all nine f64 scalars.
    let initialized = unsafe { storage.assume_init() };
    assert_eq!(
        initialized.map(f64::to_bits),
        [0.0, 0.0, 0.0, 0.0, 0.0, 4.0, 0.0, 0.0, 8.0].map(f64::to_bits)
    );
}

#[test]
fn empty_division_accepts_null_output_but_still_checks_finite_input() {
    let mut input = request();
    input.count = 1;
    input.include_ends = 0;
    // SAFETY: empty output is null/zero; immutable input remains valid.
    let status = unsafe {
        tapioca_divide_polyline(
            &raw const input,
            std::ptr::null_mut(),
            0,
            std::ptr::null_mut(),
            0,
        )
    };
    assert_eq!(status, 0);
    let bad = [0.0, 0.0, 0.0, f64::NAN, 0.0, 0.0];
    input.points.data = bad.as_ptr();
    // SAFETY: all input storage live; invalid numeric input is representable POD.
    let status = unsafe {
        tapioca_divide_polyline(
            &raw const input,
            std::ptr::null_mut(),
            0,
            std::ptr::null_mut(),
            0,
        )
    };
    assert_eq!(status, 1);
}

#[test]
fn counts_flags_shapes_numeric_range_and_capacity_refuse_atomically() {
    let nonfinite = [0.0, 0.0, 0.0, f64::INFINITY, 0.0, 0.0];
    let overflow = [0.0, 0.0, 0.0, 1e200, 0.0, 0.0];
    for kind in 0..10 {
        let mut input = request();
        let mut output = [99.0; 12];
        let mut error = [0; 128];
        let mut capacity = 12;
        match kind {
            0 => input.count = -1,
            1 => input.count = 0,
            2 => input.count = 100_001,
            3 => input.count = i32::MIN,
            4 => input.include_ends = 2,
            5 => input.points.count = 5,
            6 => input.points.data = nonfinite.as_ptr(),
            7 => input.points.data = overflow.as_ptr(),
            8 => capacity = 8,
            _ => input.points.count = 3,
        }
        // SAFETY: live disjoint storage, all malformed scalar values checked at ingress.
        let status = unsafe {
            tapioca_divide_polyline(
                &raw const input,
                output.as_mut_ptr(),
                capacity,
                error.as_mut_ptr(),
                error.len(),
            )
        };
        assert_eq!(status, 1, "kind {kind}");
        assert_ne!(error.first(), Some(&0));
        assert_eq!(output.map(f64::to_bits), [99.0; 12].map(f64::to_bits));
    }
}

#[test]
fn malformed_descriptor_point_output_and_error_shapes_refuse() {
    for kind in 0..10 {
        let mut input = request();
        let mut output = [99.0; 12];
        let mut error = [0x7f; 128];
        let mut source = &raw const input;
        let mut target = output.as_mut_ptr();
        let mut capacity = 12;
        let mut message = error.as_mut_ptr();
        let mut message_capacity = 128;
        match kind {
            0 => source = std::ptr::null(),
            1 => source = source.cast::<u8>().wrapping_add(1).cast(),
            2 => input.points.data = std::ptr::null(),
            3 => input.points.count = usize::MAX,
            4 => target = std::ptr::null_mut(),
            5 => target = target.cast::<u8>().wrapping_add(1).cast(),
            6 => capacity = usize::MAX,
            7 => message = std::ptr::null_mut(),
            8 => message_capacity = usize::MAX,
            _ => input.points.data = POINTS.as_ptr().cast::<u8>().wrapping_add(1).cast(),
        }
        // SAFETY: malformed foreign shapes are rejected before dereference; other storage live.
        let status =
            unsafe { tapioca_divide_polyline(source, target, capacity, message, message_capacity) };
        assert_eq!(status, 1, "kind {kind}");
        assert_eq!(output.map(f64::to_bits), [99.0; 12].map(f64::to_bits));
    }
}

#[test]
fn all_descriptor_point_output_error_aliases_refuse_without_writes() {
    for kind in 0..8 {
        let mut input = request();
        let mut output = [99.0; 24];
        let mut error = [0x7f; 128];
        let mut target = output.as_mut_ptr();
        let mut capacity = 24;
        let mut message = error.as_mut_ptr();
        let mut message_capacity = 128;
        match kind {
            0 => {
                target = (&raw mut input).cast();
                capacity = 3;
            }
            1 => input.points.data = output.as_ptr(),
            2 => {
                input.points.data = output.as_ptr().wrapping_add(18);
            }
            3 => {
                message = (&raw mut input).cast();
                message_capacity = size_of_val(&input);
                target = std::ptr::null_mut();
            }
            4 => {
                input.points.data = output.as_ptr();
                message = output.as_mut_ptr().cast();
                target = std::ptr::null_mut();
            }
            5 => {
                input.points.data = output.as_ptr();
                message = output.as_mut_ptr().cast();
                capacity = usize::MAX;
            }
            6 => {
                message = output.as_mut_ptr().cast();
                input.points.data = std::ptr::null();
            }
            _ => {
                message = output.as_mut_ptr().cast();
                input.points.count = usize::MAX;
            }
        }
        let before = (
            input.points.data,
            input.points.count,
            input.count,
            input.include_ends,
        );
        // SAFETY: recognized overlaps win before borrow/write, including invalid
        // peer shapes and the unused output suffix. Output backs all declared pools.
        let status = unsafe {
            tapioca_divide_polyline(
                &raw const input,
                target,
                capacity,
                message,
                message_capacity,
            )
        };
        assert_eq!(status, 1, "kind {kind}");
        assert_eq!(
            (
                input.points.data,
                input.points.count,
                input.count,
                input.include_ends
            ),
            before
        );
        assert_eq!(output.map(f64::to_bits), [99.0; 24].map(f64::to_bits));
        assert_eq!(error, [0x7f; 128]);
    }
}
