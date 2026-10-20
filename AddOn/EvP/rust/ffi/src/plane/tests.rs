use super::*;
use tapioca_geometry::affine::Vec3;

const POINTS: [f64; 6] = [1.0, 2.0, 9.0, 3.0, 4.0, -7.0];
fn request() -> PlaneRequest {
    PlaneRequest {
        plane: plane::Plane {
            origin: Vec3 {
                x: -0.0,
                y: 0.0,
                z: 5.0,
            },
            normal: Vec3 {
                x: 0.0,
                y: 0.0,
                z: 7.0,
            },
            u: Vec3 {
                x: 0.0,
                y: 0.0,
                z: 0.0,
            },
        },
        points: SliceView {
            data: POINTS.as_ptr(),
            count: POINTS.len(),
        },
    }
}
fn marker() -> plane::PlaneFrame {
    plane::PlaneFrame {
        plane: request().plane,
        u: Vec3 {
            x: 77.0,
            y: 77.0,
            z: 77.0,
        },
        v: Vec3 {
            x: 88.0,
            y: 88.0,
            z: 88.0,
        },
    }
}
fn bits(frame: &plane::PlaneFrame) -> Vec<u64> {
    [
        frame.plane.origin,
        frame.plane.normal,
        frame.plane.u,
        frame.u,
        frame.v,
    ]
    .into_iter()
    .flat_map(|v| [v.x, v.y, v.z])
    .map(f64::to_bits)
    .collect()
}

#[test]
fn x64_layout_matches_c_header() {
    assert_eq!(size_of::<plane::Plane>(), 72);
    assert_eq!(size_of::<plane::PlaneFrame>(), 120);
    if size_of::<usize>() == 8 {
        assert_eq!(size_of::<PlaneRequest>(), 88);
        assert_eq!(size_of::<PlaneOutput>(), 24);
    }
    assert_eq!(std::mem::offset_of!(plane::Plane, u), 48);
    assert_eq!(std::mem::offset_of!(PlaneRequest, points), 72);
}

#[test]
fn successful_prefix_and_raw_plane_roundtrip_publish_together() {
    let input = request();
    let mut frame = marker();
    let mut points = [99.0; 9];
    let output = PlaneOutput {
        frame: &raw mut frame,
        points: points.as_mut_ptr(),
        scalar_capacity: points.len(),
    };
    let mut error = [0x7f; 128];
    // SAFETY: live disjoint initialized descriptors/inputs and exclusive output regions.
    let status = unsafe {
        tapioca_project_to_plane(
            &raw const input,
            &raw const output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 0);
    assert_eq!(error.first(), Some(&0));
    assert_eq!(
        points.map(f64::to_bits),
        [1.0, 2.0, 5.0, 3.0, 4.0, 5.0, 99.0, 99.0, 99.0].map(f64::to_bits)
    );
    assert_eq!(frame.plane.origin.x.to_bits(), (-0.0_f64).to_bits());
    assert_eq!(frame.plane, input.plane);
}

#[test]
fn frame_only_allows_null_zero_points_and_discarded_error() {
    let mut input = request();
    input.points = SliceView {
        data: std::ptr::null(),
        count: 0,
    };
    let mut frame = marker();
    let output = PlaneOutput {
        frame: &raw mut frame,
        points: std::ptr::null_mut(),
        scalar_capacity: 0,
    };
    // SAFETY: valid null/zero arrays/error with required live disjoint frame/records.
    let status = unsafe {
        tapioca_project_to_plane(&raw const input, &raw const output, std::ptr::null_mut(), 0)
    };
    assert_eq!(status, 0);
    assert_eq!(
        frame.u,
        Vec3 {
            x: 1.0,
            y: 0.0,
            z: 0.0
        }
    );
}

#[test]
fn malformed_shapes_nonfinite_and_late_arithmetic_never_publish() {
    let late = [1.0, 2.0, 3.0, f64::MAX, f64::MAX, f64::MAX];
    for kind in 0..9 {
        let mut input = request();
        let mut frame = marker();
        let before = bits(&frame);
        let mut points = [99.0; 9];
        let mut output = PlaneOutput {
            frame: &raw mut frame,
            points: points.as_mut_ptr(),
            scalar_capacity: points.len(),
        };
        match kind {
            0 => input.points.count = 5,
            1 => input.points.data = std::ptr::null(),
            2 => input.points.count = usize::MAX,
            3 => output.frame = std::ptr::null_mut(),
            4 => output.scalar_capacity = 5,
            5 => output.points = points.as_mut_ptr().cast::<u8>().wrapping_add(1).cast(),
            6 => input.plane.u.x = f64::NAN,
            7 => input.plane.normal.z = 1e200,
            _ => {
                input.points.data = late.as_ptr();
                input.plane.normal = Vec3 {
                    x: 1.0,
                    y: 1.0,
                    z: 1.0,
                };
            }
        }
        let mut error = [0; 128];
        // SAFETY: malformed pointer shapes rejected before access; real storage remains live/disjoint.
        let status = unsafe {
            tapioca_project_to_plane(
                &raw const input,
                &raw const output,
                error.as_mut_ptr(),
                error.len(),
            )
        };
        assert_eq!(status, 1, "kind {kind}");
        assert_ne!(error.first(), Some(&0));
        assert_eq!(bits(&frame), before);
        assert_eq!(points.map(f64::to_bits), [99.0; 9].map(f64::to_bits));
    }
}

#[test]
fn output_input_descriptor_and_error_overlaps_refuse_without_writes() {
    for kind in 0..9 {
        let mut source = POINTS;
        let mut input = request();
        input.points.data = source.as_ptr();
        let mut frame = marker();
        let before = bits(&frame);
        let mut points = [99.0; 9];
        let mut output = PlaneOutput {
            frame: &raw mut frame,
            points: points.as_mut_ptr(),
            scalar_capacity: points.len(),
        };
        let mut error = [0x7f; 128];
        let mut message = error.as_mut_ptr();
        let mut capacity = error.len();
        match kind {
            0 => output.points = (&raw mut frame).cast(),
            1 => output.points = source.as_mut_ptr(),
            2 => output.frame = (&raw mut input).cast(),
            3 => output.points = (&raw mut output).cast(),
            4 => {
                message = (&raw mut input).cast();
                capacity = size_of::<PlaneRequest>();
            }
            5 => {
                message = (&raw mut output).cast();
                capacity = size_of::<PlaneOutput>();
            }
            6 => {
                message = source.as_mut_ptr().cast();
                capacity = size_of_val(&source);
                output.scalar_capacity = 0;
            }
            7 => {
                message = (&raw mut frame).cast();
                capacity = size_of::<plane::PlaneFrame>();
                input.points.data = std::ptr::null();
            }
            _ => {
                message = points.as_mut_ptr().wrapping_add(8).cast();
                capacity = size_of::<f64>();
            }
        }
        // SAFETY: recognized overlap refuses before borrow/write, including unused capacity and malformed peer.
        let status = unsafe {
            tapioca_project_to_plane(&raw const input, &raw const output, message, capacity)
        };
        assert_eq!(status, 1, "kind {kind}");
        assert_eq!(error, [0x7f; 128]);
        assert_eq!(bits(&frame), before);
        assert_eq!(points.map(f64::to_bits), [99.0; 9].map(f64::to_bits));
        assert_eq!(source.map(f64::to_bits), POINTS.map(f64::to_bits));
    }
}

#[test]
fn malformed_descriptors_and_error_storage_refuse_before_access() {
    for kind in 0..6 {
        let input = request();
        let mut frame = marker();
        let before = bits(&frame);
        let mut points = [99.0; 9];
        let output = PlaneOutput {
            frame: &raw mut frame,
            points: points.as_mut_ptr(),
            scalar_capacity: points.len(),
        };
        let mut source = &raw const input;
        let mut target = &raw const output;
        let mut error = [0x7f; 128];
        let mut message = error.as_mut_ptr();
        let mut capacity = error.len();
        match kind {
            0 => source = std::ptr::null(),
            1 => source = source.cast::<u8>().wrapping_add(1).cast(),
            2 => target = std::ptr::null(),
            3 => target = target.cast::<u8>().wrapping_add(1).cast(),
            4 => message = std::ptr::null_mut(),
            _ => capacity = usize::MAX,
        }
        // SAFETY: malformed raw descriptors/error shape are rejected before access.
        let status = unsafe { tapioca_project_to_plane(source, target, message, capacity) };
        assert_eq!(status, 1);
        assert_eq!(bits(&frame), before);
        assert_eq!(points.map(f64::to_bits), [99.0; 9].map(f64::to_bits));
    }
}
