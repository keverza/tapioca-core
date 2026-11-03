use super::*;

fn sphere() -> SdfEntry {
    SdfEntry {
        id: 1,
        kind: 2,
        operation: 1,
        profile: 1,
        enabled: 1,
        radius: 1.0,
        material_id: 7,
        ..SdfEntry::default()
    }
}

fn request(entries: &[SdfEntry]) -> SdfRequest {
    SdfRequest {
        entries: SliceView {
            data: entries.as_ptr(),
            count: entries.len(),
        },
        profile_points: SliceView {
            data: core::ptr::null(),
            count: 0,
        },
        cancel: SdfCancelCallback::default(),
        voxel_size: 0.25,
        solo_operation: 0,
        max_dense_points: 64_000_000,
        max_sparse_stored_points: 8_000_000,
        max_triangles: 2_000_000,
        strategy: 2,
        reserved: 0,
    }
}

fn query(counts: &mut SdfCounts, summary: &mut SdfSummary) -> SdfOutput {
    SdfOutput {
        counts,
        summary,
        query_only: 1,
        ..SdfOutput::default()
    }
}

#[test]
fn layouts_match_the_documented_x64_c_records() {
    assert_eq!(size_of::<SdfCancelCallback>(), 16);
    assert_eq!(size_of::<SdfEntry>(), 216);
    assert_eq!(size_of::<SdfRequest>(), 96);
    assert_eq!(size_of::<SdfCounts>(), 32);
    assert_eq!(size_of::<SdfSummary>(), 120);
    assert_eq!(size_of::<SdfOutput>(), 88);
}

#[test]
fn sphere_query_fill_is_complete_and_deterministic() {
    let entries = [sphere()];
    let request = request(&entries);
    let mut counts = SdfCounts::default();
    let mut summary = SdfSummary::default();
    let mut output = query(&mut counts, &mut summary);
    let mut error = [0_u8; 256];
    // SAFETY: all descriptors and arrays are live, disjoint and correctly sized.
    let status = unsafe {
        tapioca_sdf_build(
            &raw const request,
            &raw const output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 0);
    assert!(counts.positions > 0 && counts.positions.is_multiple_of(3));
    assert_eq!(counts.normals, counts.positions);
    assert!(counts.triangles.is_multiple_of(3));
    assert_eq!(counts.materials, counts.triangles / 3);
    assert_eq!(summary.triangles, counts.materials as u64);
    let expected = summary;

    let mut positions = vec![0.0; counts.positions];
    let mut normals = vec![0.0; counts.normals];
    let mut triangles = vec![0; counts.triangles];
    let mut materials = vec![0; counts.materials];
    output.positions = positions.as_mut_ptr();
    output.position_capacity = positions.len();
    output.normals = normals.as_mut_ptr();
    output.normal_capacity = normals.len();
    output.triangles = triangles.as_mut_ptr();
    output.triangle_capacity = triangles.len();
    output.materials = materials.as_mut_ptr();
    output.material_capacity = materials.len();
    output.query_only = 0;
    // SAFETY: fill storage is disjoint and has exactly the queried capacity.
    let status = unsafe {
        tapioca_sdf_build(
            &raw const request,
            &raw const output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 0);
    assert_eq!(summary, expected);
    assert!(positions.iter().all(|value| value.is_finite()));
    assert!(normals.iter().all(|value| value.is_finite()));
    assert!(
        triangles
            .iter()
            .all(|index| (*index as usize) < positions.len() / 3)
    );
    assert!(materials.iter().all(|material| *material == 7));
}

#[test]
fn first_operation_refusal_preserves_every_output() {
    let mut subtract = sphere();
    subtract.operation = 2;
    let entries = [subtract];
    let request = request(&entries);
    let mut counts = SdfCounts {
        positions: 91,
        normals: 92,
        triangles: 93,
        materials: 94,
    };
    let mut summary = SdfSummary {
        content_hash: 0x1234,
        ..SdfSummary::default()
    };
    let expected_counts = counts;
    let expected_summary = summary;
    let output = query(&mut counts, &mut summary);
    let mut error = [0_u8; 256];
    // SAFETY: descriptors are live and disjoint; refusal writes nothing.
    let status = unsafe {
        tapioca_sdf_build(
            &raw const request,
            &raw const output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 1);
    assert_eq!(counts, expected_counts);
    assert_eq!(summary, expected_summary);
    assert!(error.starts_with(b"FirstOperationNotUnion:"));
}

unsafe extern "C" fn cancel_after_three(context: *mut core::ffi::c_void) -> u8 {
    // SAFETY: the test passes one live mutable u32 for the complete call.
    let polls = unsafe { &mut *context.cast::<u32>() };
    *polls += 1;
    u8::from(*polls > 3)
}

#[test]
fn callback_cancellation_preserves_query_outputs() {
    let entries = [sphere()];
    let mut request = request(&entries);
    let mut polls = 0_u32;
    request.cancel = SdfCancelCallback {
        context: core::ptr::from_mut(&mut polls).cast(),
        is_cancelled: Some(cancel_after_three),
    };
    let mut counts = SdfCounts {
        positions: 55,
        ..SdfCounts::default()
    };
    let mut summary = SdfSummary {
        content_hash: 66,
        ..SdfSummary::default()
    };
    let output = query(&mut counts, &mut summary);
    let mut error = [0_u8; 256];
    // SAFETY: callback points to live `polls`; output descriptors are disjoint.
    let status = unsafe {
        tapioca_sdf_build(
            &raw const request,
            &raw const output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 1);
    assert_eq!(counts.positions, 55);
    assert_eq!(summary.content_hash, 66);
    assert!(error.starts_with(b"Cancelled:"));
    assert!(polls > 0);
}

#[test]
fn overlapping_fill_storage_refuses_before_evaluation_or_publication() {
    let entries = [sphere()];
    let request = request(&entries);
    let mut counts = SdfCounts {
        positions: 91,
        normals: 92,
        triangles: 93,
        materials: 94,
    };
    let expected_counts = counts;
    let mut summary = SdfSummary {
        content_hash: 0x1234,
        ..SdfSummary::default()
    };
    let expected_summary = summary;
    let positions = core::ptr::from_mut(&mut counts).cast::<f64>();
    let output = SdfOutput {
        counts: &raw mut counts,
        summary: &raw mut summary,
        positions,
        position_capacity: size_of::<SdfCounts>() / size_of::<f64>(),
        ..SdfOutput::default()
    };
    let mut error = [0x5a_u8; 256];
    // SAFETY: every allocation is live and aligned; the intentional overlap must
    // be rejected before either raw output pointer is dereferenced.
    let status = unsafe {
        tapioca_sdf_build(
            &raw const request,
            &raw const output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 1);
    assert_eq!(counts, expected_counts);
    assert_eq!(summary, expected_summary);
    assert_eq!(error, [0x5a_u8; 256]);
}

#[test]
fn short_fill_capacity_preserves_counts_summary_and_every_array() {
    let entries = [sphere()];
    let request = request(&entries);
    let mut counts = SdfCounts::default();
    let mut summary = SdfSummary::default();
    let mut output = query(&mut counts, &mut summary);
    let mut error = [0_u8; 256];
    // SAFETY: query descriptors and error storage are live and disjoint.
    let status = unsafe {
        tapioca_sdf_build(
            &raw const request,
            &raw const output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 0);
    assert!(counts.positions > 0);
    let expected_counts = counts;
    let expected_summary = summary;
    let mut positions = vec![13.0; counts.positions];
    let mut normals = vec![14.0; counts.normals];
    let mut triangles = vec![15_u32; counts.triangles];
    let mut materials = vec![16_i32; counts.materials];
    output.positions = positions.as_mut_ptr();
    output.position_capacity = positions.len() - 1;
    output.normals = normals.as_mut_ptr();
    output.normal_capacity = normals.len();
    output.triangles = triangles.as_mut_ptr();
    output.triangle_capacity = triangles.len();
    output.materials = materials.as_mut_ptr();
    output.material_capacity = materials.len();
    output.query_only = 0;
    // SAFETY: storage is live and disjoint; the short position prefix must be
    // refused before any output is written.
    let status = unsafe {
        tapioca_sdf_build(
            &raw const request,
            &raw const output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 1);
    assert!(error.starts_with(b"SdfOutputTooSmall"));
    assert_eq!(counts, expected_counts);
    assert_eq!(summary, expected_summary);
    assert!(
        positions
            .iter()
            .all(|value| value.to_bits() == 13.0_f64.to_bits())
    );
    assert!(
        normals
            .iter()
            .all(|value| value.to_bits() == 14.0_f32.to_bits())
    );
    assert!(triangles.iter().all(|value| *value == 15));
    assert!(materials.iter().all(|value| *value == 16));
}
