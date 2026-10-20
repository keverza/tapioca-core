use super::*;

const POINTS: [f64; 6] = [0.0, 0.0, 0.0, 0.0, 0.0, 8.0];
const PARAMETERS: [f64; 3] = [0.0, 0.5, 1.0];

fn request() -> PolylineRequest {
    PolylineRequest {
        points: SliceView {
            data: POINTS.as_ptr(),
            count: POINTS.len(),
        },
        parameters: SliceView {
            data: PARAMETERS.as_ptr(),
            count: PARAMETERS.len(),
        },
    }
}

struct Storage {
    cumulative: [f64; 4],
    points: [f64; 12],
    tangents: [f64; 12],
    total: f64,
}
impl Storage {
    fn new() -> Self {
        Self {
            cumulative: [99.0; 4],
            points: [99.0; 12],
            tangents: [99.0; 12],
            total: 99.0,
        }
    }
    fn output(&mut self) -> PolylineOutput {
        PolylineOutput {
            cumulative: self.cumulative.as_mut_ptr(),
            cumulative_capacity: self.cumulative.len(),
            points: self.points.as_mut_ptr(),
            tangents: self.tangents.as_mut_ptr(),
            sample_scalar_capacity: self.points.len(),
            total_length: &raw mut self.total,
        }
    }
    fn unchanged(&self) {
        for value in self
            .cumulative
            .iter()
            .chain(&self.points)
            .chain(&self.tangents)
            .chain([&self.total])
        {
            assert_eq!(value.to_bits(), 99.0_f64.to_bits());
        }
    }
}

#[test]
fn x64_layout_matches_c_header() {
    if size_of::<usize>() == 8 {
        assert_eq!(size_of::<PolylineRequest>(), 32);
        assert_eq!(std::mem::offset_of!(PolylineRequest, parameters), 16);
        assert_eq!(size_of::<PolylineOutput>(), 48);
        assert_eq!(std::mem::offset_of!(PolylineOutput, total_length), 40);
    }
}

#[test]
fn complete_success_writes_only_prefixes() {
    let input = request();
    let mut storage = Storage::new();
    let output = storage.output();
    let mut error = [0xff; 128];
    // SAFETY: live immutable input and descriptor storage; all writable regions disjoint.
    let status = unsafe {
        tapioca_sample_polyline(
            &raw const input,
            &raw const output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 0);
    assert_eq!(error.first(), Some(&0));
    assert_eq!(
        storage.cumulative.map(f64::to_bits),
        [0.0, 8.0, 99.0, 99.0].map(f64::to_bits)
    );
    assert_eq!(
        storage.points.map(f64::to_bits),
        [
            0.0, 0.0, 0.0, 0.0, 0.0, 4.0, 0.0, 0.0, 8.0, 99.0, 99.0, 99.0
        ]
        .map(f64::to_bits)
    );
    assert_eq!(
        storage.tangents.map(f64::to_bits),
        [
            0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 99.0, 99.0, 99.0
        ]
        .map(f64::to_bits)
    );
    assert_eq!(storage.total.to_bits(), 8.0_f64.to_bits());
}

#[test]
fn measurement_only_accepts_null_zero_sample_buffers_and_discarded_error() {
    let mut input = request();
    input.parameters = SliceView {
        data: std::ptr::null(),
        count: 0,
    };
    let mut storage = Storage::new();
    let mut output = storage.output();
    output.points = std::ptr::null_mut();
    output.tangents = std::ptr::null_mut();
    output.sample_scalar_capacity = 0;
    // SAFETY: null/zero arrays and error are valid; remaining storage is live and disjoint.
    let status = unsafe {
        tapioca_sample_polyline(&raw const input, &raw const output, std::ptr::null_mut(), 0)
    };
    assert_eq!(status, 0);
    assert_eq!(storage.total.to_bits(), 8.0_f64.to_bits());
    assert_eq!(
        storage.points.map(f64::to_bits),
        [99.0; 12].map(f64::to_bits)
    );
}

#[test]
fn malformed_shapes_capacity_and_geometry_never_publish_partial_outputs() {
    let bad_points = [0.0, 0.0, 0.0, f64::NAN, 0.0, 0.0];
    let huge_points = [0.0, 0.0, 0.0, 1.0e200, 0.0, 0.0];
    let bad_parameters = [0.0, f64::INFINITY, 1.0];
    for kind in 0..12 {
        let mut input = request();
        let mut storage = Storage::new();
        let mut output = storage.output();
        match kind {
            0 => input.points.count = 5,
            1 => input.points.data = std::ptr::null(),
            2 => input.parameters.data = std::ptr::null(),
            3 => input.parameters.count = usize::MAX,
            4 => input.points.data = POINTS.as_ptr().cast::<u8>().wrapping_add(1).cast(),
            5 => output.cumulative_capacity = 1,
            6 => output.sample_scalar_capacity = 8,
            7 => output.total_length = std::ptr::null_mut(),
            8 => {
                output.points = storage
                    .points
                    .as_mut_ptr()
                    .cast::<u8>()
                    .wrapping_add(1)
                    .cast();
            }
            9 => input.points.data = bad_points.as_ptr(),
            10 => input.points.data = huge_points.as_ptr(),
            _ => input.parameters.data = bad_parameters.as_ptr(),
        }
        let mut error = [0; 128];
        // SAFETY: malformed raw shapes are rejected before access; other arrays
        // remain live initialized inputs or exclusive disjoint writable outputs.
        let status = unsafe {
            tapioca_sample_polyline(
                &raw const input,
                &raw const output,
                error.as_mut_ptr(),
                error.len(),
            )
        };
        assert_eq!(status, 1, "kind {kind}");
        assert_ne!(error.first(), Some(&0));
        storage.unchanged();
    }
}

#[test]
fn every_output_pair_overlap_refuses_without_even_error_publication() {
    for left in 0..4 {
        for right in left + 1..4 {
            let input = request();
            let mut storage = Storage::new();
            let mut output = storage.output();
            let pointer = match left {
                0 => output.cumulative,
                1 => output.points,
                _ => output.tangents,
            };
            match right {
                1 => output.points = pointer,
                2 => output.tangents = pointer,
                _ => output.total_length = pointer,
            }
            let mut error = [0x7f; 128];
            // SAFETY: deliberately overlapping output shapes are recognized
            // before any borrow/write; no output allocation validity is inferred.
            let status = unsafe {
                tapioca_sample_polyline(
                    &raw const input,
                    &raw const output,
                    error.as_mut_ptr(),
                    error.len(),
                )
            };
            assert_eq!(status, 1);
            assert_eq!(error, [0x7f; 128]);
            storage.unchanged();
        }
    }
}

#[test]
fn output_input_descriptor_and_error_overlap_refuse_without_writes() {
    for kind in 0..6 {
        let mut points = POINTS;
        let mut input = request();
        input.points.data = points.as_ptr();
        let mut storage = Storage::new();
        let mut output = storage.output();
        let mut error = [0x7f; 128];
        let mut error_pointer = error.as_mut_ptr();
        let mut error_capacity = error.len();
        match kind {
            0 => output.points = points.as_mut_ptr(),
            1 => output.total_length = (&raw mut input).cast(),
            2 => output.total_length = (&raw mut output).cast(),
            3 => {
                error_pointer = points.as_mut_ptr().cast();
                error_capacity = size_of_val(&points);
                // Overlap must win over capacity errors, or error publication
                // would corrupt immutable input while reporting that error.
                output.cumulative_capacity = 1;
            }
            4 => {
                error_pointer = (&raw mut storage.total).cast();
                error_capacity = size_of::<f64>();
            }
            _ => {
                error_pointer = (&raw mut input).cast();
                error_capacity = size_of::<PolylineRequest>();
            }
        }
        // SAFETY: recognized overlap is rejected before foreign slice creation
        // or publication, including error/descriptor overlap before record reads.
        let status = unsafe {
            tapioca_sample_polyline(
                &raw const input,
                &raw const output,
                error_pointer,
                error_capacity,
            )
        };
        assert_eq!(status, 1);
        assert_eq!(error, [0x7f; 128]);
        assert_eq!(points.map(f64::to_bits), POINTS.map(f64::to_bits));
        storage.unchanged();
    }
}

#[test]
fn malformed_descriptors_and_error_storage_refuse_before_access() {
    let input = request();
    let mut storage = Storage::new();
    let output = storage.output();
    let mut error = [0; 128];
    for kind in 0..4 {
        let input_ptr = if kind == 0 {
            std::ptr::null()
        } else {
            &raw const input
        };
        let output_ptr = if kind == 1 {
            std::ptr::null()
        } else {
            &raw const output
        };
        let error_ptr = if kind == 2 {
            std::ptr::null_mut()
        } else {
            error.as_mut_ptr()
        };
        let input_ptr = if kind == 3 {
            input_ptr.cast::<u8>().wrapping_add(1).cast()
        } else {
            input_ptr
        };
        // SAFETY: null/misaligned shapes are rejected before dereference.
        let status =
            unsafe { tapioca_sample_polyline(input_ptr, output_ptr, error_ptr, error.len()) };
        assert_eq!(status, 1);
        storage.unchanged();
    }
}
