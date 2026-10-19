use super::*;

fn slice<T>(values: &[T]) -> SliceView<T> {
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

fn mesh(vertices: &[f64], triangles: &[u32]) -> MeshView {
    MeshView {
        guid: slice(b"synthetic"),
        elem_type: 0,
        vertices: slice(vertices),
        normals: empty(),
        triangles: slice(triangles),
        tri_material: empty(),
        tri_wire_edges: empty(),
        tri_source_body: empty(),
        tri_source_pgon: empty(),
        tri_surface_attribute: empty(),
    }
}

fn check(input: &MeshView) -> (i32, [u8; 256]) {
    let mut error = [0; 256];
    // SAFETY: tests retain all live input storage; malformed null/count or
    // misaligned representations are intentionally rejected before access.
    let status = unsafe { tapioca_validate_mesh(input, error.as_mut_ptr(), error.len()) };
    (status, error)
}

#[test]
fn c_layout_matches_x64_header() {
    // Supported Windows x64/Linux x64 ABI smoke tests assert the C layout too.
    if size_of::<usize>() == 8 {
        assert_eq!(size_of::<SliceView<u8>>(), 16);
        assert_eq!(size_of::<MeshView>(), 152);
        assert_eq!(std::mem::offset_of!(MeshView, vertices), 24);
        assert_eq!(size_of::<SnapshotView>(), 56);
        assert_eq!(std::mem::offset_of!(SnapshotView, meshes), 40);
    }
}

#[test]
fn empty_views_with_null_arrays_are_valid() {
    assert_eq!(check(&mesh(&[], &[])).0, 0);
    let input = SnapshotView {
        id: 0,
        capture_stamp: 0,
        scope: empty(),
        complete_model: 0,
        meshes: empty(),
    };
    // SAFETY: local initialized descriptor; all null arrays have zero length.
    let status = unsafe { tapioca_validate_snapshot(&raw const input, std::ptr::null_mut(), 0) };
    assert_eq!(status, 0);
}

#[test]
fn null_and_misaligned_descriptors_are_rejected() {
    let mut error = [0; 128];
    // SAFETY: null descriptor is rejected before forming a reference.
    let status =
        unsafe { tapioca_validate_mesh(std::ptr::null(), error.as_mut_ptr(), error.len()) };
    assert_eq!(status, 1);
    let input = mesh(&[], &[]);
    let misaligned = (&raw const input).cast::<u8>().wrapping_add(1).cast();
    // SAFETY: misaligned descriptor is rejected before access.
    let status = unsafe { tapioca_validate_mesh(misaligned, error.as_mut_ptr(), error.len()) };
    assert_eq!(status, 1);
}

#[test]
fn malformed_array_pointers_and_lengths_are_rejected() {
    let mut input = mesh(&[], &[]);
    input.vertices = SliceView {
        data: std::ptr::null(),
        count: 3,
    };
    assert_eq!(check(&input).0, 1);
    input.vertices = SliceView {
        data: std::ptr::NonNull::<f64>::dangling().as_ptr(),
        count: usize::MAX,
    };
    assert_eq!(check(&input).0, 1);
    let values = [0.0_f64; 2];
    input.vertices = SliceView {
        data: values.as_ptr().cast::<u8>().wrapping_add(1).cast(),
        count: 1,
    };
    assert_eq!(check(&input).0, 1);
}

#[test]
fn invalid_utf8_is_not_exposed_as_a_rust_string() {
    let mut input = mesh(&[], &[]);
    input.guid = slice(&[0xff]);
    let (status, error) = check(&input);
    assert_eq!(status, 1);
    assert!(error.starts_with(b"InvalidUtf8:"));
}

#[test]
fn mesh_failures_have_stable_codes_and_preserve_input() {
    let vertices = [
        1_000_000.0,
        2_000_000.0,
        0.0,
        1_000_001.0,
        2_000_000.0,
        0.0,
        1_000_000.0,
        2_000_001.0,
        0.0,
    ];
    let triangles = [0, 1, 2];
    let before = vertices.map(f64::to_bits);
    let mut input = mesh(&vertices, &triangles);
    input.tri_source_body = slice(&[1]);
    input.tri_source_pgon = slice(&[3]);
    assert_eq!(check(&input).0, 0);
    assert_eq!(vertices.map(f64::to_bits), before);
    let bad_indices = [0, 1, 3];
    input.triangles = slice(&bad_indices);
    let (status, error) = check(&input);
    assert_eq!(status, 1);
    assert!(error.starts_with(b"MeshIndexOutOfBounds:"));
    assert_eq!(vertices.map(f64::to_bits), before);
    assert_eq!(triangles, [0, 1, 2]);
}

#[test]
fn overlapping_input_and_error_storage_is_untouched() {
    let mut vertices = [1.0_f64, 2.0, 3.0];
    let error = vertices.as_mut_ptr().cast::<u8>();
    let input = mesh(&vertices, &[]);
    let before = vertices.map(f64::to_bits);
    // SAFETY: intentional overlap is recognized before forming an input borrow;
    // live storage remains exclusively owned by this test and no write occurs.
    let status = unsafe { tapioca_validate_mesh(&raw const input, error, 8) };
    assert_eq!(status, 1);
    assert_eq!(vertices.map(f64::to_bits), before);
}

#[test]
fn source_sidecars_are_paired_not_authored_ids() {
    let input = mesh(&[0.0; 9], &[0, 1, 2]);
    let mut partial = input;
    partial.tri_source_body = slice(&[1]);
    let (status, error) = check(&partial);
    assert_eq!(status, 1);
    assert!(error.starts_with(b"MeshSourceSidecarInvalid:"));
}

#[test]
fn snapshot_boolean_and_duplicate_guids_refuse() {
    let inputs = [mesh(&[], &[]); 2];
    let mut snapshot = SnapshotView {
        id: 1,
        capture_stamp: 0,
        scope: slice(b"selection"),
        complete_model: 2,
        meshes: slice(&inputs),
    };
    let mut error = [0; 256];
    // SAFETY: every descriptor/array is initialized local storage, independent of error.
    let status =
        unsafe { tapioca_validate_snapshot(&raw const snapshot, error.as_mut_ptr(), error.len()) };
    assert_eq!(status, 1);
    assert!(error.starts_with(b"InvalidBoolean:"));
    snapshot.complete_model = 0;
    // SAFETY: same retained immutable inputs; only the byte boolean changed.
    let status =
        unsafe { tapioca_validate_snapshot(&raw const snapshot, error.as_mut_ptr(), error.len()) };
    assert_eq!(status, 2);
    assert!(error.starts_with(b"SnapshotDuplicateGuid:"));
}

#[test]
fn invalid_error_buffer_prevents_input_access() {
    // SAFETY: invalid error shape is rejected before the also-null input is read.
    let status = unsafe { tapioca_validate_mesh(std::ptr::null(), std::ptr::null_mut(), 1) };
    assert_eq!(status, 1);
    // SAFETY: both invalid shapes are rejected before dereference.
    let status = unsafe { tapioca_validate_snapshot(std::ptr::null(), std::ptr::null_mut(), 1) };
    assert_eq!(status, 1);
}
