use super::*;

fn request() -> TransformBuilderRequest {
    TransformBuilderRequest {
        kind: 3,
        origin: affine::Vec3 {
            x: 10.0,
            y: 20.0,
            z: 30.0,
        },
        vector: affine::Vec3 {
            x: -2.0,
            y: 3.0,
            z: 0.5,
        },
        radians: 0.0,
    }
}
fn bits(value: &affine::Transform) -> Vec<u64> {
    value.m.iter().flatten().map(|v| v.to_bits()).collect()
}

#[test]
fn builder_pod_layout_matches_c() {
    if size_of::<usize>() == 8 {
        assert_eq!(size_of::<TransformBuilderRequest>(), 64);
    }
    assert_eq!(std::mem::offset_of!(TransformBuilderRequest, origin), 8);
    assert_eq!(std::mem::offset_of!(TransformBuilderRequest, vector), 32);
    assert_eq!(std::mem::offset_of!(TransformBuilderRequest, radians), 56);
}

#[test]
fn each_tag_publishes_complete_matrix_and_clears_error() {
    for kind in 1..=4 {
        let input = TransformBuilderRequest { kind, ..request() };
        let mut output = affine::Transform::IDENTITY;
        let mut error = [0x7f; 128];
        // SAFETY: live disjoint POD input, exclusive output and error storage.
        let status = unsafe {
            tapioca_build_transform(
                &raw const input,
                &raw mut output,
                error.as_mut_ptr(),
                error.len(),
            )
        };
        assert_eq!(status, 0);
        assert_eq!(error.first(), Some(&0));
        assert!(output.m.iter().flatten().all(|v| v.is_finite()));
    }
}

#[test]
fn unknown_tags_and_domain_arithmetic_refusals_never_publish() {
    for kind in 0..8 {
        let mut input = request();
        match kind {
            0 => input.kind = 0,
            1 => input.kind = u32::MAX,
            2 => input.origin.z = f64::NAN,
            3 => input.vector.z = 0.0,
            4 => {
                input.kind = 2;
                input.vector = affine::Vec3 {
                    x: 0.0,
                    y: 0.0,
                    z: 1e-200,
                };
            }
            5 => {
                input.kind = 4;
                input.vector.z = f64::INFINITY;
            }
            6 => {
                input.kind = 2;
                input.radians = f64::NAN;
            }
            _ => {
                input.origin.x = f64::MAX;
            }
        }
        let mut output = affine::Transform::IDENTITY;
        let before = bits(&output);
        let mut error = [0; 128];
        // SAFETY: valid disjoint storage; malformed geometry is input data only.
        let status = unsafe {
            tapioca_build_transform(
                &raw const input,
                &raw mut output,
                error.as_mut_ptr(),
                error.len(),
            )
        };
        assert_eq!(status, 1, "kind {kind}");
        assert_ne!(error.first(), Some(&0));
        assert_eq!(bits(&output), before);
    }
}

#[test]
fn unused_fields_are_ignored_and_null_zero_error_is_supported() {
    let mut input = request();
    input.kind = 1;
    input.origin.x = f64::NAN;
    input.radians = f64::INFINITY;
    let mut output = affine::Transform::IDENTITY;
    // SAFETY: live disjoint storage, discarded error buffer. Only selected fields consumed.
    let status = unsafe {
        tapioca_build_transform(&raw const input, &raw mut output, std::ptr::null_mut(), 0)
    };
    assert_eq!(status, 0);
    assert_eq!(
        output
            .m
            .first()
            .and_then(|row| row.last())
            .map(|v| v.to_bits()),
        Some((-2.0_f64).to_bits())
    );
}

#[test]
fn malformed_pointer_shapes_refuse_before_access() {
    for kind in 0..7 {
        let input = request();
        let mut output = affine::Transform::IDENTITY;
        let before = bits(&output);
        let mut source = &raw const input;
        let mut target = &raw mut output;
        let mut error = [0; 128];
        let mut message = error.as_mut_ptr();
        let mut capacity = error.len();
        match kind {
            0 => source = std::ptr::null(),
            1 => source = source.cast::<u8>().wrapping_add(1).cast(),
            2 => target = std::ptr::null_mut(),
            3 => target = target.cast::<u8>().wrapping_add(1).cast(),
            4 => source = (usize::MAX - 7) as *const TransformBuilderRequest,
            5 => message = std::ptr::null_mut(),
            _ => capacity = usize::MAX,
        }
        // SAFETY: malformed shapes are rejected without dereferencing; actual allocations remain live.
        let status = unsafe { tapioca_build_transform(source, target, message, capacity) };
        assert_eq!(status, 1);
        assert_eq!(bits(&output), before);
    }
}

#[test]
fn every_overlap_pair_and_known_overlap_with_malformed_peer_refuses_without_diagnostics() {
    for kind in 0..5 {
        let mut input = request();
        let before = input;
        let mut output = affine::Transform::IDENTITY;
        let prior = bits(&output);
        let mut source = &raw const input;
        let mut target = &raw mut output;
        let mut error = [0x7f; 128];
        let mut message = error.as_mut_ptr();
        let mut capacity = error.len();
        match kind {
            0 => target = (&raw mut input).cast(),
            1 | 3 => {
                message = (&raw mut input).cast();
                capacity = size_of::<TransformBuilderRequest>();
                if kind == 3 {
                    target = std::ptr::null_mut();
                }
            }
            _ => {
                message = (&raw mut output).cast();
                capacity = size_of::<affine::Transform>();
                if kind == 4 {
                    source = std::ptr::null();
                }
            }
        }
        // SAFETY: recognized overlap wins before any read/write, including malformed peer shapes.
        let status = unsafe { tapioca_build_transform(source, target, message, capacity) };
        assert_eq!(status, 1);
        assert_eq!(error, [0x7f; 128]);
        assert_eq!(bits(&output), prior);
        assert_eq!(input.origin, before.origin);
        assert_eq!(input.vector, before.vector);
        assert_eq!(input.kind, before.kind);
    }
}
