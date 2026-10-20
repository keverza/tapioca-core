use super::*;
use tapioca_geometry::affine::Vec3;

fn request() -> ArcRequest {
    ArcRequest {
        centre: Vec3 {
            x: 0.0,
            y: 0.0,
            z: 0.0,
        },
        normal: Vec3 {
            x: 0.0,
            y: 0.0,
            z: 1.0,
        },
        radius: 2.0,
        start_radians: 0.0,
        sweep_radians: std::f64::consts::PI,
        segments: 2,
    }
}

#[test]
fn layout_matches_c_x64_header() {
    assert_eq!(std::mem::offset_of!(ArcRequest, normal), 24);
    assert_eq!(std::mem::offset_of!(ArcRequest, radius), 48);
    assert_eq!(std::mem::offset_of!(ArcRequest, segments), 72);
    if size_of::<usize>() == 8 {
        assert_eq!(size_of::<ArcRequest>(), 80);
    }
}

#[test]
fn publishes_exact_prefix_and_clears_error() -> Result<(), tapioca_geometry::arc::Error> {
    let input = request();
    let mut output = [99.0; 12];
    let mut error = [0xff; 128];
    // SAFETY: live disjoint initialized request and exclusive outputs.
    let status = unsafe {
        tapioca_make_arc(
            &raw const input,
            output.as_mut_ptr(),
            output.len(),
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 0);
    assert_eq!(error.first(), Some(&0));
    assert_eq!(
        output.get(..9),
        Some(tapioca_geometry::arc::make_arc(&input)?.as_slice())
    );
    assert_eq!(
        output
            .get(9..)
            .ok_or(tapioca_geometry::arc::Error::Segments)?
            .iter()
            .map(|v| v.to_bits())
            .collect::<Vec<_>>(),
        vec![99.0_f64.to_bits(); 3]
    );
    Ok(())
}

#[test]
fn invalid_domain_and_late_arithmetic_refusals_preserve_all_output() {
    for kind in 0..6 {
        let mut input = request();
        match kind {
            0 => input.segments = 4097,
            1 => input.radius = 0.0,
            2 => input.centre.z = f64::NAN,
            3 => input.normal.z = 1e-200,
            4 => {
                input.start_radians = f64::MAX;
                input.sweep_radians = f64::MAX;
            }
            _ => {
                input.centre.y = f64::MAX;
                input.radius = f64::MAX;
            }
        }
        let mut output = [99.0; 12];
        let mut error = [0; 128];
        // SAFETY: valid disjoint storage, malformed geometry is data only.
        let status = unsafe {
            tapioca_make_arc(
                &raw const input,
                output.as_mut_ptr(),
                output.len(),
                error.as_mut_ptr(),
                error.len(),
            )
        };
        assert_eq!(status, 1);
        assert_ne!(error.first(), Some(&0));
        assert_eq!(output.map(f64::to_bits), [99.0; 12].map(f64::to_bits));
    }
}

#[test]
fn malformed_pointers_capacities_and_error_storage_refuse_before_access() {
    for kind in 0..9 {
        let input = request();
        let mut output = [99.0; 12];
        let mut error = [0x7f; 128];
        let mut source = &raw const input;
        let mut destination = output.as_mut_ptr();
        let mut count = output.len();
        let mut message = error.as_mut_ptr();
        match kind {
            0 => source = std::ptr::null(),
            1 => source = source.cast::<u8>().wrapping_add(1).cast(),
            2 => destination = std::ptr::null_mut(),
            3 => destination = destination.cast::<u8>().wrapping_add(1).cast(),
            4 => count = usize::MAX,
            5 => count = 8,
            6 => {
                destination = std::ptr::null_mut();
                count = 0;
            }
            7 => message = std::ptr::null_mut(),
            _ => source = (usize::MAX - 7) as *const ArcRequest,
        }
        // SAFETY: bad raw shapes are rejected before any dereference. Real
        // allocations remain live/disjoint; no validity is inferred from shape.
        let status = unsafe { tapioca_make_arc(source, destination, count, message, error.len()) };
        assert_eq!(status, 1, "kind {kind}");
        assert_eq!(output.map(f64::to_bits), [99.0; 12].map(f64::to_bits));
    }
}

#[test]
fn all_request_output_error_overlaps_refuse_without_even_error_writes() {
    for kind in 0..5 {
        let mut input = request();
        let before = input;
        let mut output = [99.0; 12];
        let mut error = [0x7f; 128];
        let mut destination = output.as_mut_ptr();
        let mut message = error.as_mut_ptr();
        let mut message_count = error.len();
        match kind {
            0 => destination = (&raw mut input).cast(),
            1 => {
                message = (&raw mut input).cast();
                message_count = size_of::<ArcRequest>();
            }
            2 => {
                message = output.as_mut_ptr().cast();
                message_count = size_of_val(&output);
            }
            3 => {
                // Even untouched capacity after the nine-scalar prefix
                // may not alias error storage.
                message = output.as_mut_ptr().wrapping_add(10).cast();
                message_count = size_of::<f64>();
            }
            _ => destination = (&raw mut input.normal).cast(),
        }
        // SAFETY: recognized overlapping ranges refuse before borrow or write.
        let status = unsafe {
            tapioca_make_arc(
                &raw const input,
                destination,
                output.len(),
                message,
                message_count,
            )
        };
        assert_eq!(status, 1);
        assert_eq!(error, [0x7f; 128]);
        assert_eq!(input.centre, before.centre);
        assert_eq!(input.normal, before.normal);
        assert_eq!(input.radius.to_bits(), before.radius.to_bits());
        assert_eq!(output.map(f64::to_bits), [99.0; 12].map(f64::to_bits));
    }
}

#[test]
fn discarded_and_truncated_errors_retain_status() {
    let mut input = request();
    let mut output = [99.0; 12];
    assert_eq!(
        // SAFETY: live disjoint storage, error explicitly discarded.
        unsafe {
            tapioca_make_arc(
                &raw const input,
                output.as_mut_ptr(),
                output.len(),
                std::ptr::null_mut(),
                0,
            )
        },
        0
    );
    input.radius = 0.0;
    let mut error = [0xff; 2];
    assert_eq!(
        // SAFETY: live disjoint storage, valid short error buffer.
        unsafe {
            tapioca_make_arc(
                &raw const input,
                output.as_mut_ptr(),
                output.len(),
                error.as_mut_ptr(),
                error.len(),
            )
        },
        1
    );
    assert_eq!(error.last(), Some(&0));
}

#[test]
fn known_error_overlap_wins_over_another_malformed_region() {
    let mut input = request();
    let mut output = [99.0; 12];
    // SAFETY: recognized error/output overlap must refuse before reporting the
    // null request shape error. No foreign memory is accessed in this case.
    let status = unsafe {
        tapioca_make_arc(
            std::ptr::null(),
            output.as_mut_ptr(),
            output.len(),
            output.as_mut_ptr().cast(),
            size_of_val(&output),
        )
    };
    assert_eq!(status, 1);
    assert_eq!(output.map(f64::to_bits), [99.0; 12].map(f64::to_bits));
    let before = input;
    // SAFETY: known error/request overlap wins over null nonempty output;
    // live request remains untouched, including its immutable centre/normal.
    let status = unsafe {
        tapioca_make_arc(
            &raw const input,
            std::ptr::null_mut(),
            9,
            (&raw mut input).cast(),
            size_of::<ArcRequest>(),
        )
    };
    assert_eq!(status, 1);
    assert_eq!(input.centre, before.centre);
    assert_eq!(input.normal, before.normal);
    assert_eq!(input.radius.to_bits(), before.radius.to_bits());
}
