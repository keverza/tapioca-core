use super::*;
use crate::mesh::MeshView;

const VERTICES: [f64; 9] = [
    0.0, 0.0, 0.0, //
    1.0, 0.0, 0.0, //
    0.0, 1.0, 0.0,
];
const NORMALS: [f32; 9] = [
    0.0, 0.0, 1.0, //
    0.0, 0.0, 1.0, //
    0.0, 0.0, 1.0,
];
const TRIANGLES: [u32; 3] = [0, 1, 2];
const MATERIALS: [i32; 1] = [1];
const WIRE_EDGES: [u8; 1] = [7];
const SOURCE_BODY: [i32; 1] = [1];
const SOURCE_PGON: [i32; 1] = [4];
const SURFACE_ATTRIBUTE: [i32; 1] = [42];
const ZERO_SURFACE_ATTRIBUTE: [i32; 1] = [0];
const ZERO_MATERIAL: [i32; 1] = [0];
const NEGATIVE_MATERIAL: [i32; 1] = [-1];
const SECOND_MATERIAL: [i32; 1] = [2];

const BOUNDS: Aabb = Aabb {
    mn: [0.0, 0.0, 0.0],
    mx: [1.0, 1.0, 0.0],
};
const EMPTY_BOUNDS: Aabb = Aabb {
    mn: [f64::INFINITY; 3],
    mx: [f64::NEG_INFINITY; 3],
};
const CANONICAL_GUID: &str = "12345678-ABCD-1234-ABCD-123456789ABC";

const BASE_MESH: MeshValueView<'static> = MeshValueView {
    mesh: MeshView {
        guid: "mesh-a",
        elem_type: 1,
        vertices: &VERTICES,
        normals: &NORMALS,
        triangles: &TRIANGLES,
        tri_material: &MATERIALS,
        tri_wire_edges: &WIRE_EDGES,
        tri_source_body: &SOURCE_BODY,
        tri_source_pgon: &SOURCE_PGON,
        tri_surface_attribute: &SURFACE_ATTRIBUTE,
    },
    bounds: BOUNDS,
};
const EMPTY_MESH: MeshValueView<'static> = MeshValueView {
    mesh: MeshView {
        guid: "",
        elem_type: 0,
        vertices: &[],
        normals: &[],
        triangles: &[],
        tri_material: &[],
        tri_wire_edges: &[],
        tri_source_body: &[],
        tri_source_pgon: &[],
        tri_surface_attribute: &[],
    },
    bounds: EMPTY_BOUNDS,
};
const UNKNOWN_MESH: MeshValueView<'static> = MeshValueView {
    mesh: MeshView {
        tri_material: &ZERO_MATERIAL,
        tri_surface_attribute: &ZERO_SURFACE_ATTRIBUTE,
        ..BASE_MESH.mesh
    },
    bounds: BOUNDS,
};

const TRANSPARENCY: [MaterialTransparency; 1] = [MaterialTransparency {
    index: 1,
    transparency: 0.5,
}];
const IDENTITIES: [MaterialIdentity<'static>; 1] = [MaterialIdentity {
    index: 1,
    external_reference: 42,
    name: "material",
}];
const SURFACES: [SurfaceAttribute<'static>; 1] = [SurfaceAttribute {
    index: 42,
    name: "surface",
    guid: CANONICAL_GUID,
}];

fn empty_snapshot<'a>() -> SnapshotValueView<'a> {
    SnapshotValueView {
        id: 0,
        capture_stamp: 0,
        scope: "all",
        complete_model: false,
        meshes: &[],
        material_transparency: &[],
        material_identity: &[],
        surface_attributes: &[],
    }
}

fn plain_request(snapshot: SnapshotValueView<'_>) -> SnapshotValueRequest<'_> {
    SnapshotValueRequest {
        snapshot,
        expected_capture_stamp: 0,
        require_catalogs: false,
    }
}

fn captured_snapshot<'a>(
    meshes: &'a [MeshValueView<'a>],
    transparency: &'a [MaterialTransparency],
    identities: &'a [MaterialIdentity<'a>],
    surfaces: &'a [SurfaceAttribute<'a>],
) -> SnapshotValueView<'a> {
    SnapshotValueView {
        id: 42,
        capture_stamp: 7,
        scope: "all",
        complete_model: true,
        meshes,
        material_transparency: transparency,
        material_identity: identities,
        surface_attributes: surfaces,
    }
}

fn strict_request<'a>(
    meshes: &'a [MeshValueView<'a>],
    transparency: &'a [MaterialTransparency],
    identities: &'a [MaterialIdentity<'a>],
    surfaces: &'a [SurfaceAttribute<'a>],
) -> SnapshotValueRequest<'a> {
    let mut request = plain_request(captured_snapshot(
        meshes,
        transparency,
        identities,
        surfaces,
    ));
    request.expected_capture_stamp = 7;
    request.require_catalogs = true;
    request
}

fn same_aabb(left: Aabb, right: Aabb) -> bool {
    left.mn.map(f64::to_bits) == right.mn.map(f64::to_bits)
        && left.mx.map(f64::to_bits) == right.mx.map(f64::to_bits)
}

fn same_transparency(left: &[MaterialTransparency], right: &[MaterialTransparency]) -> bool {
    left.len() == right.len()
        && left.iter().zip(right).all(|(left, right)| {
            left.index == right.index && left.transparency.to_bits() == right.transparency.to_bits()
        })
}

fn same_identities(left: &[MaterialIdentity<'_>], right: &[MaterialIdentity<'_>]) -> bool {
    left.len() == right.len()
        && left.iter().zip(right).all(|(left, right)| {
            left.index == right.index
                && left.external_reference == right.external_reference
                && left.name == right.name
        })
}

fn same_surfaces(left: &[SurfaceAttribute<'_>], right: &[SurfaceAttribute<'_>]) -> bool {
    left.len() == right.len()
        && left.iter().zip(right).all(|(left, right)| {
            left.index == right.index && left.name == right.name && left.guid == right.guid
        })
}

#[test]
fn empty_synthetic_values_and_empty_envelopes_are_valid() {
    assert_eq!(
        validate_snapshot_value(&plain_request(empty_snapshot())),
        Ok(())
    );
    assert_eq!(validate_mesh_value(&EMPTY_MESH), Ok(()));

    let finite_empty = MeshValueView {
        bounds: Aabb {
            mn: [0.0; 3],
            mx: [0.0; 3],
        },
        ..EMPTY_MESH
    };
    assert_eq!(validate_mesh_value(&finite_empty), Ok(()));

    let mixed = Aabb {
        mn: [f64::INFINITY, 0.0, 0.0],
        mx: [f64::NEG_INFINITY, 0.0, 0.0],
    };
    let nan = Aabb {
        mn: [f64::NAN; 3],
        mx: [f64::NEG_INFINITY; 3],
    };
    for bounds in [mixed, nan] {
        let value = MeshValueView {
            bounds,
            ..EMPTY_MESH
        };
        assert_eq!(validate_mesh_value(&value), Err(Error::BoundsInvalid));
    }
}

#[test]
fn complete_captured_value_with_sorted_catalogs_is_valid() {
    let meshes = [BASE_MESH];
    let request = strict_request(&meshes, &TRANSPARENCY, &IDENTITIES, &SURFACES);
    assert_eq!(validate_snapshot_value(&request), Ok(()));
    assert_eq!(validate_mesh_value(&BASE_MESH), Ok(()));

    let mut selection = strict_request(&meshes, &TRANSPARENCY, &IDENTITIES, &SURFACES);
    selection.snapshot.scope = "selection";
    selection.snapshot.complete_model = false;
    assert_eq!(validate_snapshot_value(&selection), Ok(()));
}

#[test]
fn bounds_are_finite_ordered_all_axes_and_contain_every_vertex() {
    let inverted_y = Aabb {
        mn: [0.0, 2.0, 0.0],
        mx: [1.0, 1.0, 0.0],
    };
    let inverted_z = Aabb {
        mn: [0.0, 0.0, 1.0],
        mx: [1.0, 1.0, 0.0],
    };
    let nan = Aabb {
        mn: [0.0, 0.0, 0.0],
        mx: [1.0, f64::NAN, 0.0],
    };
    let infinite = Aabb {
        mn: [0.0, 0.0, 0.0],
        mx: [f64::INFINITY, 1.0, 0.0],
    };
    for bounds in [inverted_y, inverted_z, nan, infinite] {
        let value = MeshValueView {
            bounds,
            ..BASE_MESH
        };
        assert_eq!(validate_mesh_value(&value), Err(Error::BoundsInvalid));
    }

    let excluded = MeshValueView {
        bounds: Aabb {
            mn: [0.5, 0.0, 0.0],
            mx: [1.0, 1.0, 0.0],
        },
        ..BASE_MESH
    };
    assert_eq!(
        validate_mesh_value(&excluded),
        Err(Error::BoundsNotContaining)
    );

    let unused_vertices = [
        0.0, 0.0, 0.0, //
        1.0, 0.0, 0.0, //
        0.0, 1.0, 0.0, //
        9.0, 9.0, 9.0,
    ];
    let unused = MeshValueView {
        mesh: MeshView {
            vertices: &unused_vertices,
            normals: &[],
            ..BASE_MESH.mesh
        },
        bounds: BOUNDS,
    };
    assert_eq!(
        validate_mesh_value(&unused),
        Err(Error::BoundsNotContaining)
    );

    let no_triangles = MeshValueView {
        mesh: MeshView {
            triangles: &[],
            tri_material: &[],
            tri_wire_edges: &[],
            tri_source_body: &[],
            tri_source_pgon: &[],
            tri_surface_attribute: &[],
            ..BASE_MESH.mesh
        },
        bounds: BOUNDS,
    };
    assert_eq!(validate_mesh_value(&no_triangles), Ok(()));
}

#[test]
fn large_survey_coordinates_are_contained_without_narrowing() {
    const EASTING: f64 = 6_512_345.678_901_234;
    const NORTHING: f64 = -2_345_678.123_456_789;
    const HEIGHT: f64 = 1_234.567_890_123;
    let vertices = [
        EASTING,
        NORTHING,
        HEIGHT,
        EASTING + 1.0,
        NORTHING + 2.0,
        HEIGHT + 3.0,
        EASTING + 4.0,
        NORTHING + 5.0,
        HEIGHT + 6.0,
    ];
    let large = Aabb {
        mn: [EASTING, NORTHING, HEIGHT],
        mx: [EASTING + 4.0, NORTHING + 5.0, HEIGHT + 6.0],
    };
    let tight = Aabb {
        mn: [EASTING, NORTHING, HEIGHT],
        mx: [EASTING, NORTHING, HEIGHT],
    };
    let mesh = MeshView {
        vertices: &vertices,
        normals: &[],
        triangles: &[],
        tri_material: &[],
        tri_wire_edges: &[],
        tri_source_body: &[],
        tri_source_pgon: &[],
        tri_surface_attribute: &[],
        ..BASE_MESH.mesh
    };
    let before = vertices.map(f64::to_bits);
    assert_eq!(
        validate_mesh_value(&MeshValueView {
            mesh,
            bounds: large
        }),
        Ok(())
    );
    assert_eq!(
        validate_mesh_value(&MeshValueView {
            mesh,
            bounds: tight
        }),
        Err(Error::BoundsNotContaining)
    );
    assert_eq!(vertices.map(f64::to_bits), before);
}

#[test]
fn wire_masks_and_surface_attribute_values_are_checked() {
    let wire = [8_u8];
    let bad_wire = MeshValueView {
        mesh: MeshView {
            tri_wire_edges: &wire,
            ..BASE_MESH.mesh
        },
        ..BASE_MESH
    };
    assert_eq!(validate_mesh_value(&bad_wire), Err(Error::WireEdgeInvalid));

    let negative_surface = [-1_i32];
    let bad_surface = MeshValueView {
        mesh: MeshView {
            tri_surface_attribute: &negative_surface,
            ..BASE_MESH.mesh
        },
        ..BASE_MESH
    };
    assert_eq!(
        validate_mesh_value(&bad_surface),
        Err(Error::SurfaceAttributeInvalid)
    );

    let unknown_surface = MeshValueView {
        mesh: MeshView {
            tri_surface_attribute: &ZERO_SURFACE_ATTRIBUTE,
            ..BASE_MESH.mesh
        },
        ..BASE_MESH
    };
    assert_eq!(validate_mesh_value(&unknown_surface), Ok(()));

    let out_of_range = [0_u32, 1, 3];
    let geometry = MeshValueView {
        mesh: MeshView {
            triangles: &out_of_range,
            ..BASE_MESH.mesh
        },
        ..BASE_MESH
    };
    assert_eq!(
        validate_mesh_value(&geometry),
        Err(Error::MeshGeometryInvalid)
    );
}

#[test]
fn zero_material_pool_is_unknown_but_negative_is_invalid() {
    let negative = MeshValueView {
        mesh: MeshView {
            tri_material: &NEGATIVE_MATERIAL,
            ..UNKNOWN_MESH.mesh
        },
        ..UNKNOWN_MESH
    };
    assert_eq!(
        validate_mesh_value(&negative),
        Err(Error::MaterialIndexInvalid)
    );

    let meshes = [UNKNOWN_MESH];
    let request = strict_request(&meshes, &[], &[], &[]);
    assert_eq!(validate_snapshot_value(&request), Ok(()));
}

#[test]
fn required_catalogs_need_only_referenced_material_identity() {
    let referenced = MeshValueView {
        mesh: MeshView {
            tri_material: &MATERIALS,
            ..UNKNOWN_MESH.mesh
        },
        ..UNKNOWN_MESH
    };
    let referenced_meshes = [referenced];

    let missing = strict_request(&referenced_meshes, &[], &[], &[]);
    assert_eq!(
        validate_snapshot_value(&missing),
        Err(Error::MissingCatalog)
    );

    let sparse = strict_request(&referenced_meshes, &[], &IDENTITIES, &[]);
    assert_eq!(validate_snapshot_value(&sparse), Ok(()));

    let with_transparency = strict_request(&referenced_meshes, &TRANSPARENCY, &IDENTITIES, &[]);
    assert_eq!(validate_snapshot_value(&with_transparency), Ok(()));

    let meshes = [UNKNOWN_MESH];
    let unreferenced = [
        MaterialIdentity {
            index: 1,
            external_reference: 0,
            name: "",
        },
        MaterialIdentity {
            index: 2,
            external_reference: 99,
            name: "unreferenced",
        },
    ];
    let unreferenced_ok = strict_request(&meshes, &TRANSPARENCY, &unreferenced, &[]);
    assert_eq!(validate_snapshot_value(&unreferenced_ok), Ok(()));
}

#[test]
fn surface_attributes_are_zero_unknown_and_positive_catalogued() {
    let meshes = [BASE_MESH];
    let identity_without_surface = [MaterialIdentity {
        index: 1,
        external_reference: 0,
        name: "material",
    }];
    let missing = strict_request(&meshes, &TRANSPARENCY, &identity_without_surface, &[]);
    assert_eq!(
        validate_snapshot_value(&missing),
        Err(Error::MissingCatalog)
    );

    let unknown_meshes = [UNKNOWN_MESH];
    let unknown = strict_request(&unknown_meshes, &TRANSPARENCY, &IDENTITIES, &[]);
    assert_eq!(validate_snapshot_value(&unknown), Ok(()));

    let out_of_range = [43_i32];
    let missing_surface = MeshValueView {
        mesh: MeshView {
            tri_surface_attribute: &out_of_range,
            ..UNKNOWN_MESH.mesh
        },
        ..UNKNOWN_MESH
    };
    let surface_meshes = [missing_surface];
    let request = strict_request(&surface_meshes, &TRANSPARENCY, &IDENTITIES, &SURFACES);
    assert_eq!(
        validate_snapshot_value(&request),
        Err(Error::MissingCatalog)
    );
}

#[test]
fn external_references_are_opaque_signed_pool_identity() {
    let unknown_meshes = [UNKNOWN_MESH];
    for reference in [i32::MIN, -42, -1, 0, 1, 42, i32::MAX] {
        let identity = [MaterialIdentity {
            index: 1,
            external_reference: reference,
            name: "material",
        }];
        let request = strict_request(&unknown_meshes, &TRANSPARENCY, &identity, &[]);
        assert_eq!(validate_snapshot_value(&request), Ok(()));
    }

    let ambiguous = [
        MaterialIdentity {
            index: 1,
            external_reference: 42,
            name: "first",
        },
        MaterialIdentity {
            index: 2,
            external_reference: 42,
            name: "second",
        },
    ];
    let first = MeshValueView {
        mesh: MeshView {
            guid: "mesh-one",
            tri_material: &MATERIALS,
            ..UNKNOWN_MESH.mesh
        },
        ..UNKNOWN_MESH
    };
    let second = MeshValueView {
        mesh: MeshView {
            guid: "mesh-two",
            tri_material: &SECOND_MATERIAL,
            ..UNKNOWN_MESH.mesh
        },
        ..UNKNOWN_MESH
    };
    let meshes = [first, second];
    let request = strict_request(&meshes, &[], &ambiguous, &[]);
    assert_eq!(validate_snapshot_value(&request), Ok(()));
}

#[test]
fn referenced_surface_but_unknown_material_never_infers_mapping() {
    let identity_without_surface = [MaterialIdentity {
        index: 1,
        external_reference: 42,
        name: "material",
    }];
    let unknown_meshes = [UNKNOWN_MESH];
    let request = strict_request(
        &unknown_meshes,
        &TRANSPARENCY,
        &identity_without_surface,
        &[],
    );
    assert_eq!(validate_snapshot_value(&request), Ok(()));

    let meshes = [BASE_MESH];
    let request = strict_request(&meshes, &TRANSPARENCY, &identity_without_surface, &[]);
    assert_eq!(
        validate_snapshot_value(&request),
        Err(Error::MissingCatalog)
    );
}

#[test]
fn all_catalog_rows_are_checked_even_when_unreferenced() {
    for value in [-0.01, 1.01, f64::NAN, f64::INFINITY] {
        let transparency = [MaterialTransparency {
            index: 1,
            transparency: value,
        }];
        let request = plain_request(captured_snapshot(&[], &transparency, &[], &[]));
        assert_eq!(
            validate_snapshot_value(&request),
            Err(Error::TransparencyInvalid)
        );
    }

    let duplicate_transparency = [
        MaterialTransparency {
            index: 1,
            transparency: 0.5,
        },
        MaterialTransparency {
            index: 1,
            transparency: 0.25,
        },
    ];
    let request = plain_request(captured_snapshot(&[], &duplicate_transparency, &[], &[]));
    let error = validate_snapshot_value(&request);
    assert_eq!(error, Err(Error::DuplicateCatalogs));
    assert!(Error::DuplicateCatalogs.is_state_error());

    for index in [-1, 0] {
        let invalid_material = [MaterialTransparency {
            index,
            transparency: 0.5,
        }];
        let request = plain_request(captured_snapshot(&[], &invalid_material, &[], &[]));
        assert_eq!(
            validate_snapshot_value(&request),
            Err(Error::CatalogIndexInvalid)
        );

        let invalid_identity = [MaterialIdentity {
            index,
            external_reference: 0,
            name: "invalid",
        }];
        let request = plain_request(captured_snapshot(&[], &[], &invalid_identity, &[]));
        assert_eq!(
            validate_snapshot_value(&request),
            Err(Error::CatalogIndexInvalid)
        );
    }

    let duplicate_identity = [
        MaterialIdentity {
            index: 1,
            external_reference: 0,
            name: "one",
        },
        MaterialIdentity {
            index: 1,
            external_reference: 1,
            name: "two",
        },
    ];
    let request = plain_request(captured_snapshot(&[], &[], &duplicate_identity, &[]));
    assert_eq!(
        validate_snapshot_value(&request),
        Err(Error::DuplicateCatalogs)
    );

    let duplicate_surface = [
        SurfaceAttribute {
            index: 42,
            name: "one",
            guid: CANONICAL_GUID,
        },
        SurfaceAttribute {
            index: 42,
            name: "two",
            guid: CANONICAL_GUID,
        },
    ];
    let request = plain_request(captured_snapshot(&[], &[], &[], &duplicate_surface));
    assert_eq!(
        validate_snapshot_value(&request),
        Err(Error::DuplicateCatalogs)
    );

    for index in [0, -1] {
        let surfaces = [SurfaceAttribute {
            index,
            name: "surface",
            guid: CANONICAL_GUID,
        }];
        let request = plain_request(captured_snapshot(&[], &[], &[], &surfaces));
        assert_eq!(
            validate_snapshot_value(&request),
            Err(Error::CatalogIndexInvalid)
        );
    }
}

#[test]
fn surface_guids_are_canonical_uppercase_dashed_non_nil() {
    let malformed = [
        "",
        "not-a-native-guid",
        "00000000-0000-0000-0000-000000000000",
        "12345678-abcd-1234-abcd-123456789abc",
        "{12345678-ABCD-1234-ABCD-123456789ABC}",
        "12345678-ABCD-1234-ABCD-123456789AB",
        "12345678-ABCD-1234-ABCD-123456789ABCX",
    ];
    for guid in malformed {
        let surfaces = [SurfaceAttribute {
            index: 42,
            name: "surface",
            guid,
        }];
        let request = plain_request(captured_snapshot(&[], &[], &[], &surfaces));
        assert_eq!(
            validate_snapshot_value(&request),
            Err(Error::SurfaceGuidInvalid)
        );
    }

    let surfaces = [SurfaceAttribute {
        index: 42,
        name: "",
        guid: CANONICAL_GUID,
    }];
    let request = plain_request(captured_snapshot(&[], &[], &[], &surfaces));
    assert_eq!(validate_snapshot_value(&request), Ok(()));
}

#[test]
fn optional_missing_catalogs_remain_unknown_but_malformed_rows_refuse() {
    let meshes = [BASE_MESH];
    let request = plain_request(captured_snapshot(&meshes, &[], &[], &[]));
    assert_eq!(validate_snapshot_value(&request), Ok(()));

    let strict = strict_request(&meshes, &[], &[], &[]);
    assert_eq!(validate_snapshot_value(&strict), Err(Error::MissingCatalog));

    let bad_transparency = [MaterialTransparency {
        index: 1,
        transparency: 1.5,
    }];
    let request = plain_request(captured_snapshot(&meshes, &bad_transparency, &[], &[]));
    assert_eq!(
        validate_snapshot_value(&request),
        Err(Error::TransparencyInvalid)
    );

    let bad_surface = [SurfaceAttribute {
        index: 42,
        name: "surface",
        guid: "bad",
    }];
    let request = plain_request(captured_snapshot(&meshes, &[], &[], &bad_surface));
    assert_eq!(
        validate_snapshot_value(&request),
        Err(Error::SurfaceGuidInvalid)
    );
}

#[test]
fn snapshot_framing_scope_and_freshness_are_explicit() {
    let mut released = plain_request(empty_snapshot());
    released.snapshot.id = 5;
    released.snapshot.capture_stamp = 6;
    assert_eq!(validate_snapshot_value(&released), Ok(()));

    let meshes = [BASE_MESH];
    for (id, stamp, expected) in [(42, 0, 7), (42, 8, 7), (0, 0, 7)] {
        let mut request = captured_snapshot(&meshes, &TRANSPARENCY, &IDENTITIES, &SURFACES);
        request.id = id;
        request.capture_stamp = stamp;
        let request = SnapshotValueRequest {
            snapshot: request,
            expected_capture_stamp: expected,
            require_catalogs: true,
        };
        let error = validate_snapshot_value(&request);
        assert_eq!(error, Err(Error::CaptureStale));
        assert!(Error::CaptureStale.is_state_error());
    }

    let mut detached_id = captured_snapshot(&meshes, &TRANSPARENCY, &IDENTITIES, &SURFACES);
    detached_id.id = 0;
    let request = SnapshotValueRequest {
        snapshot: detached_id,
        expected_capture_stamp: 7,
        require_catalogs: true,
    };
    assert_eq!(validate_snapshot_value(&request), Ok(()));

    let mut complete_stamp_zero = captured_snapshot(&meshes, &TRANSPARENCY, &IDENTITIES, &SURFACES);
    complete_stamp_zero.capture_stamp = 0;
    let request = SnapshotValueRequest {
        snapshot: complete_stamp_zero,
        expected_capture_stamp: 0,
        require_catalogs: true,
    };
    assert_eq!(validate_snapshot_value(&request), Ok(()));

    for scope in ["", "ALL", "all ", "type-filtered"] {
        let mut request = strict_request(&meshes, &TRANSPARENCY, &IDENTITIES, &SURFACES);
        request.snapshot.scope = scope;
        request.snapshot.complete_model = false;
        assert_eq!(validate_snapshot_value(&request), Err(Error::ScopeInvalid));
    }

    let mut request = strict_request(&meshes, &TRANSPARENCY, &IDENTITIES, &SURFACES);
    request.snapshot.scope = "selection";
    request.snapshot.complete_model = true;
    assert_eq!(
        validate_snapshot_value(&request),
        Err(Error::CompleteModelScope)
    );
}

#[test]
fn duplicate_nonempty_guids_are_state_errors_but_blank_guids_may_repeat() {
    let first = MeshValueView {
        mesh: MeshView {
            guid: "duplicate",
            ..BASE_MESH.mesh
        },
        ..BASE_MESH
    };
    let meshes = [first, first];
    let request = plain_request(captured_snapshot(&meshes, &[], &[], &[]));
    let error = validate_snapshot_value(&request);
    assert_eq!(error, Err(Error::SnapshotDuplicateGuid));
    assert!(Error::SnapshotDuplicateGuid.is_state_error());

    let blank = MeshValueView {
        mesh: MeshView {
            guid: "",
            ..BASE_MESH.mesh
        },
        ..BASE_MESH
    };
    let blank_meshes = [blank, blank];
    let request = plain_request(captured_snapshot(&blank_meshes, &[], &[], &[]));
    assert_eq!(validate_snapshot_value(&request), Ok(()));
}

#[test]
fn validation_is_deterministic_shuffled_and_non_mutating() {
    let meshes = [BASE_MESH];
    let transparency = [
        MaterialTransparency {
            index: 2,
            transparency: 0.25,
        },
        MaterialTransparency {
            index: 1,
            transparency: 0.5,
        },
    ];
    let identities = [
        MaterialIdentity {
            index: 2,
            external_reference: -7,
            name: "second",
        },
        MaterialIdentity {
            index: 1,
            external_reference: 42,
            name: "material",
        },
    ];
    let surfaces = [SURFACES[0]];
    let vertices_before = VERTICES.map(f64::to_bits);
    let bounds_before = BASE_MESH.bounds;

    let request = strict_request(&meshes, &transparency, &identities, &surfaces);
    assert_eq!(validate_snapshot_value(&request), Ok(()));
    assert_eq!(validate_snapshot_value(&request), Ok(()));

    let sorted_transparency = [transparency[1], transparency[0]];
    let sorted_identities = [identities[1], identities[0]];
    let sorted = strict_request(&meshes, &sorted_transparency, &sorted_identities, &surfaces);
    assert_eq!(validate_snapshot_value(&sorted), Ok(()));

    assert!(same_transparency(&transparency, &transparency));
    assert!(same_identities(&identities, &identities));
    assert!(same_surfaces(&surfaces, &surfaces));
    assert!(same_aabb(BASE_MESH.bounds, bounds_before));
    assert_eq!(VERTICES.map(f64::to_bits), vertices_before);
}

#[test]
fn error_messages_and_state_classification_are_stable() {
    let state = [
        Error::CaptureStale,
        Error::DuplicateCatalogs,
        Error::SnapshotDuplicateGuid,
        Error::CompleteModelScope,
        Error::MissingCatalog,
    ];
    for error in state {
        assert!(error.is_state_error());
        assert!(error.message().contains(':'));
        assert_eq!(error.to_string(), error.message());
    }

    let argument = [
        Error::MeshGeometryInvalid,
        Error::BoundsInvalid,
        Error::BoundsNotContaining,
        Error::WireEdgeInvalid,
        Error::MaterialIndexInvalid,
        Error::SurfaceAttributeInvalid,
        Error::TransparencyInvalid,
        Error::CatalogIndexInvalid,
        Error::SurfaceGuidInvalid,
        Error::ScopeInvalid,
    ];
    for error in argument {
        assert!(!error.is_state_error());
        assert!(error.message().contains(':'));
        assert_eq!(error.to_string(), error.message());
    }
}
