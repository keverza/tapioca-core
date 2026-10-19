use super::*;

fn empty<T>() -> SliceView<T> {
    SliceView {
        data: std::ptr::null(),
        count: 0,
    }
}
fn slice<T>(values: &[T]) -> SliceView<T> {
    SliceView {
        data: values.as_ptr(),
        count: values.len(),
    }
}
fn mesh() -> MeshValueView {
    MeshValueView {
        mesh: MeshView {
            guid: empty(),
            elem_type: 0,
            vertices: empty(),
            normals: empty(),
            triangles: empty(),
            tri_material: empty(),
            tri_wire_edges: empty(),
            tri_source_body: empty(),
            tri_source_pgon: empty(),
            tri_surface_attribute: empty(),
        },
        bounds: value::Aabb {
            mn: [f64::INFINITY; 3],
            mx: [f64::NEG_INFINITY; 3],
        },
    }
}
fn request() -> SnapshotValueRequest {
    SnapshotValueRequest {
        snapshot: SnapshotValueView {
            id: 0,
            capture_stamp: 0,
            scope: slice(b"all"),
            complete_model: 0,
            meshes: empty(),
            material_transparency: empty(),
            material_identity: empty(),
            surface_attributes: empty(),
        },
        expected_capture_stamp: 0,
        require_catalogs: 0,
    }
}
fn validate(input: &SnapshotValueRequest) -> (i32, [u8; 256]) {
    let mut error = [0; 256];
    // SAFETY: all fixtures retain live immutable arrays for the call and the local error allocation is disjoint.
    let status = unsafe { tapioca_validate_snapshot_value(input, error.as_mut_ptr(), error.len()) };
    (status, error)
}

#[test]
fn complete_value_x64_layouts_match_c_and_cpp() {
    if size_of::<usize>() == 8 {
        assert_eq!(size_of::<value::Aabb>(), 48);
        assert_eq!(size_of::<MeshValueView>(), 200);
        assert_eq!(size_of::<value::MaterialTransparency>(), 16);
        assert_eq!(size_of::<MaterialIdentity>(), 24);
        assert_eq!(size_of::<SurfaceAttribute>(), 40);
        assert_eq!(size_of::<SnapshotValueView>(), 104);
        assert_eq!(size_of::<SnapshotValueRequest>(), 120);
        assert_eq!(std::mem::offset_of!(MeshValueView, bounds), 152);
        assert_eq!(std::mem::offset_of!(SnapshotValueView, meshes), 40);
    }
}

#[test]
fn complete_value_empty_synthetic_snapshot_is_valid() {
    let input = request();
    let (status, error) = validate(&input);
    assert_eq!(status, 0);
    assert_eq!(error.first(), Some(&0));
    let input = mesh();
    let mut error = [0; 256];
    // SAFETY: initialized empty mesh and disjoint writable error allocation.
    let status =
        unsafe { tapioca_validate_mesh_value(&raw const input, error.as_mut_ptr(), error.len()) };
    assert_eq!(status, 0);
}

#[test]
fn complete_value_transport_checks_nested_catalogs_and_flags() {
    let mut input = request();
    input.require_catalogs = 2;
    assert_eq!(validate(&input).0, 1);
    input.require_catalogs = 0;
    input.snapshot.complete_model = 2;
    assert_eq!(validate(&input).0, 1);
    input.snapshot.complete_model = 0;
    input.snapshot.material_identity = SliceView {
        data: std::ptr::null(),
        count: 1,
    };
    assert_eq!(validate(&input).0, 1);
    input.snapshot.material_identity = empty();
    input.snapshot.surface_attributes = SliceView {
        data: std::ptr::NonNull::<SurfaceAttribute>::dangling().as_ptr(),
        count: usize::MAX,
    };
    assert_eq!(validate(&input).0, 1);
}

#[test]
fn complete_value_catalog_text_is_utf8_checked_even_when_unreferenced() {
    let bad_name = [0xff];
    let materials = [MaterialIdentity {
        index: 1,
        external_reference: 0,
        name: slice(&bad_name),
    }];
    let mut input = request();
    input.snapshot.material_identity = slice(&materials);
    let (status, error) = validate(&input);
    assert_eq!(status, 1);
    assert!(error.starts_with(b"InvalidUtf8:"));
}

#[test]
fn complete_value_detected_text_output_overlap_does_not_write() {
    let mut storage = [b'a'; 64];
    let materials = [MaterialIdentity {
        index: 1,
        external_reference: 0,
        name: slice(&storage),
    }];
    let mut input = request();
    input.snapshot.material_identity = slice(&materials);
    // SAFETY: recognized input/error overlap is rejected without writing or retaining the immutable borrow.
    let status = unsafe {
        tapioca_validate_snapshot_value(&raw const input, storage.as_mut_ptr(), storage.len())
    };
    assert_eq!(status, 1);
    assert_eq!(storage, [b'a'; 64]);
}

#[test]
fn complete_value_retained_bounds_are_checked_without_repair() {
    let vertices = [0.0, 0.0, 0.0];
    let mut input = mesh();
    input.mesh.vertices = slice(&vertices);
    input.bounds = value::Aabb {
        mn: [1.0; 3],
        mx: [2.0; 3],
    };
    let before = input.bounds;
    let mut error = [0; 256];
    // SAFETY: initialized live geometry and descriptor, disjoint output.
    let status =
        unsafe { tapioca_validate_mesh_value(&raw const input, error.as_mut_ptr(), error.len()) };
    assert_eq!(status, 1);
    assert_eq!(input.bounds, before);
    assert_eq!(vertices, [0.0; 3]);
}

#[test]
fn complete_value_capture_and_completeness_conflicts_report_state() {
    let mut input = request();
    input.expected_capture_stamp = 7;
    assert_eq!(validate(&input).0, 2);
    input.expected_capture_stamp = 0;
    input.snapshot.scope = slice(b"selection");
    input.snapshot.complete_model = 1;
    let (status, error) = validate(&input);
    assert_eq!(status, 2);
    assert!(error.starts_with(b"CompleteModelScope:"));
}
