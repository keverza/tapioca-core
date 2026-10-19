use super::*;

#[test]
#[expect(
    clippy::panic,
    reason = "Deliberate unwinding panic tests the exact shared export envelope before diagnostic commit"
)]
fn panic_before_publication_preserves_identity_diagnostic() {
    let input = request();
    let before = input.table.context;
    let mut output = 99;
    let mut error = [0; 256];
    // SAFETY: live immutable local descriptors and disjoint output buffers. No
    // test-only exported symbol or production panic switch is introduced.
    let status = unsafe {
        validate_call(
            &raw mut output,
            error.as_mut_ptr(),
            error.len(),
            |context| {
                let input = context.record(&raw const input)?;
                let request = context.provenance(&input)?;
                assert!(id::validate_provenance(&request).is_ok());
                panic!("deliberate identity boundary fixture");
            },
        )
    };
    assert_eq!((status, output), (3, 99));
    assert_eq!(input.table.context, before);
    assert!(error.starts_with(b"Rust panic contained"));
}

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
fn context_value() -> id::Context {
    let mut uuid = [0; 16];
    if let Some(first) = uuid.first_mut() {
        *first = 1;
    }
    id::Context {
        schema_version: 1,
        session_uuid: uuid,
        revision: 0,
        table_id: 100,
    }
}
fn request() -> ProvenanceRequest {
    let context = context_value();
    ProvenanceRequest {
        expected_session: context.session_uuid,
        expected_revision: 0,
        counts: id::EntityCounts {
            vertices: 0,
            edges: 0,
            faces: 0,
            patches: 0,
        },
        domains: 5,
        require_object: 0,
        table: ProvenanceView {
            context,
            sources: empty(),
            operations: empty(),
            objects: empty(),
            bindings: empty(),
            binding_refs: empty(),
            source_ids: empty(),
            operation_ids: empty(),
            predecessors: empty(),
            mappings: empty(),
            role_bytes: empty(),
            support_tables: empty(),
        },
    }
}
fn check(request: &ProvenanceRequest) -> (i32, u32, [u8; 256]) {
    let mut code = u32::MAX;
    let mut error = [0; 256];
    // SAFETY: tests retain all input arrays; the two local outputs are disjoint.
    let status = unsafe {
        tapioca_validate_provenance(request, &raw mut code, error.as_mut_ptr(), error.len())
    };
    (status, code, error)
}

#[test]
fn pod_layouts_match_the_x64_c_header() {
    if size_of::<usize>() == 8 {
        assert_eq!(size_of::<id::SourceId>(), 8);
        assert_eq!(size_of::<id::Context>(), 40);
        assert_eq!(size_of::<id::SourceRecord>(), 24);
        assert_eq!(size_of::<id::OperationRecord>(), 24);
        assert_eq!(size_of::<id::ObjectRecord>(), 16);
        assert_eq!(size_of::<id::BindingRecord>(), 96);
        assert_eq!(size_of::<id::MappingRecord>(), 80);
        assert_eq!(size_of::<id::PredecessorRecord>(), 24);
        assert_eq!(size_of::<id::SupportTable>(), 80);
        assert_eq!(size_of::<ProvenanceView>(), 216);
        assert_eq!(size_of::<ProvenanceRequest>(), 280);
        assert_eq!(std::mem::offset_of!(ProvenanceRequest, table), 64);
        assert_eq!(std::mem::offset_of!(id::BindingRecord, logical_id), 8);
        assert_eq!(std::mem::offset_of!(id::BindingRecord, role), 72);
    }
}

#[test]
fn empty_attributed_geometry_with_revision_zero_is_valid() {
    let (status, code, error) = check(&request());
    assert_eq!((status, code), (0, 0));
    assert_eq!(error.first(), Some(&0));
}

#[test]
fn structural_refusals_publish_machine_diagnostics() {
    let mut input = request();
    input.expected_revision = 1;
    let (status, code, error) = check(&input);
    assert_eq!((status, code), (2, 3));
    assert!(error.starts_with(b"ProvenanceStale:"));
    input.expected_revision = 0;
    input.counts.vertices = 1;
    let (status, code, error) = check(&input);
    assert_eq!((status, code), (2, 1));
    assert!(error.starts_with(b"ProvenanceUnattributable:"));
}

#[test]
fn transport_errors_do_not_commit_diagnostics() {
    let mut input = request();
    input.require_object = 2;
    assert_eq!((check(&input).0, check(&input).1), (1, u32::MAX));
    input.require_object = 0;
    input.table.sources = SliceView {
        data: std::ptr::null(),
        count: 1,
    };
    assert_eq!((check(&input).0, check(&input).1), (1, u32::MAX));
    let mut code = 99;
    // SAFETY: null descriptor and error buffer mismatch are rejected before access.
    let status = unsafe {
        tapioca_validate_provenance(std::ptr::null(), &raw mut code, std::ptr::null_mut(), 1)
    };
    assert_eq!((status, code), (1, 99));
}

#[test]
fn diagnostic_must_be_present_and_disjoint_from_input_and_error() {
    let mut input = request();
    let mut error = [0_u8; 256];
    // SAFETY: null diagnostic is a recognized malformed shape, never dereferenced.
    let status = unsafe {
        tapioca_validate_provenance(
            &raw const input,
            std::ptr::null_mut(),
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 1);
    let before = input.table.context;
    let diagnostic = &raw mut input.table.context.schema_version;
    // SAFETY: recognized output/input overlap is rejected before forming a borrow or writing.
    let status = unsafe {
        tapioca_validate_provenance(
            &raw const input,
            diagnostic,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 1);
    assert_eq!(input.table.context, before);
    let mut storage = [u32::MAX; 64];
    // SAFETY: recognized diagnostic/error overlap writes neither output.
    let status = unsafe {
        tapioca_validate_provenance(
            &raw const input,
            storage.as_mut_ptr(),
            storage.as_mut_ptr().cast(),
            256,
        )
    };
    assert_eq!(status, 1);
    assert_eq!(storage, [u32::MAX; 64]);
}

#[test]
fn flat_pool_arithmetic_is_checked_before_borrow() {
    let mut input = request();
    input.table.source_ids = SliceView {
        data: std::ptr::NonNull::<id::SourceId>::dangling().as_ptr(),
        count: usize::MAX,
    };
    let (status, code, _) = check(&input);
    assert_eq!((status, code), (1, u32::MAX));
}

fn mesh() -> MeshView {
    MeshView {
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
    }
}

#[test]
fn attributed_mesh_uses_real_array_counts_and_the_shared_geometry_gate() {
    let mut input = request();
    let mut mesh = mesh();
    let mut code = 99;
    let mut error = [0; 256];
    // SAFETY: local initialized empty descriptors and disjoint output allocations.
    let status = unsafe {
        tapioca_validate_attributed_mesh(
            &raw const mesh,
            &raw const input,
            &raw mut code,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!((status, code), (0, 0));
    input.counts.vertices = 1;
    // SAFETY: only semantic count metadata changed; allocations remain valid.
    let status = unsafe {
        tapioca_validate_attributed_mesh(
            &raw const mesh,
            &raw const input,
            &raw mut code,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!((status, code), (2, 1));
    let vertices = [0.0, 1.0];
    mesh.vertices = slice(&vertices);
    code = 99;
    // SAFETY: the two-scalar geometry allocation is live; shape is semantically invalid.
    let status = unsafe {
        tapioca_validate_attributed_mesh(
            &raw const mesh,
            &raw const input,
            &raw mut code,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!((status, code), (1, 99));
    assert!(error.starts_with(b"MeshArrayLength:"));
}

#[test]
fn diagnostic_cannot_alias_mesh_coordinate_storage() {
    let mut values = [0_u64; 3];
    let mut mesh = mesh();
    mesh.vertices = SliceView {
        data: values.as_ptr().cast(),
        count: 3,
    };
    let input = request();
    let mut error = [0; 256];
    // SAFETY: u64 zero bytes represent initialized f64 zeros. Recognized alias
    // with diagnostic output is refused before creating an immutable array borrow.
    let status = unsafe {
        tapioca_validate_attributed_mesh(
            &raw const mesh,
            &raw const input,
            values.as_mut_ptr().cast(),
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 1);
    assert_eq!(values, [0; 3]);
}
