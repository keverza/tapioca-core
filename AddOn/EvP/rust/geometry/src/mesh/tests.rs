use super::*;

const BASE_VERTICES: [f64; 9] = [
    0.0, 0.0, 0.0, //
    1.0, 0.0, 0.0, //
    0.0, 1.0, 0.0,
];
const BASE_NORMALS: [f32; 9] = [
    0.0, 0.0, 1.0, //
    0.0, 0.0, 1.0, //
    0.0, 0.0, 1.0,
];
const BASE_TRIANGLES: [u32; 3] = [0, 1, 2];
const BASE_MATERIALS: [i32; 1] = [7];
const BASE_WIRE_EDGES: [u8; 1] = [0x7];
const BASE_SOURCE_BODY: [i32; 1] = [1];
const BASE_SOURCE_PGON: [i32; 1] = [2];
const BASE_SURFACE_ATTRIBUTE: [i32; 1] = [3];

fn base_mesh() -> MeshView<'static> {
    MeshView {
        guid: "synthetic mesh / 17",
        elem_type: 14,
        vertices: &BASE_VERTICES,
        normals: &BASE_NORMALS,
        triangles: &BASE_TRIANGLES,
        tri_material: &BASE_MATERIALS,
        tri_wire_edges: &BASE_WIRE_EDGES,
        tri_source_body: &BASE_SOURCE_BODY,
        tri_source_pgon: &BASE_SOURCE_PGON,
        tri_surface_attribute: &BASE_SURFACE_ATTRIBUTE,
    }
}

fn assert_valid(mesh: &MeshView<'_>) {
    assert_eq!(validate_mesh(mesh), Ok(()));
}

fn assert_mesh_error(mesh: &MeshView<'_>, expected: ValidationError) {
    assert_eq!(validate_mesh(mesh), Err(expected));
}

#[test]
fn base_mesh_and_noncanonical_synthetic_guid_are_valid() {
    assert_valid(&base_mesh());
    assert_valid(&MeshView {
        guid: "not a canonical guid // with spaces",
        ..base_mesh()
    });
}

#[test]
fn empty_mesh_is_valid_but_sidecars_must_then_be_empty() {
    let empty = MeshView {
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
    };
    assert_valid(&empty);
    assert_mesh_error(
        &MeshView {
            normals: &BASE_NORMALS,
            ..empty
        },
        ValidationError::MeshSidecarLength,
    );
    assert_mesh_error(
        &MeshView {
            tri_material: &BASE_MATERIALS,
            ..empty
        },
        ValidationError::MeshSidecarLength,
    );
}

#[test]
fn optional_sidecars_may_all_be_empty() {
    assert_valid(&MeshView {
        normals: &[],
        tri_material: &[],
        tri_wire_edges: &[],
        tri_source_body: &[],
        tri_source_pgon: &[],
        tri_surface_attribute: &[],
        ..base_mesh()
    });
}

#[test]
fn positions_and_triangles_must_be_whole_triples() {
    let vertices = [0.0_f64; 8];
    assert_mesh_error(
        &MeshView {
            vertices: &vertices,
            ..base_mesh()
        },
        ValidationError::MeshArrayLength,
    );
    let vertices = [0.0_f64; 10];
    assert_mesh_error(
        &MeshView {
            vertices: &vertices,
            ..base_mesh()
        },
        ValidationError::MeshArrayLength,
    );
    let triangles = [0_u32; 2];
    assert_mesh_error(
        &MeshView {
            triangles: &triangles,
            ..base_mesh()
        },
        ValidationError::MeshArrayLength,
    );
    let triangles = [0_u32; 4];
    assert_mesh_error(
        &MeshView {
            triangles: &triangles,
            ..base_mesh()
        },
        ValidationError::MeshArrayLength,
    );
}

#[test]
fn nan_and_infinite_positions_or_normals_are_rejected() {
    let positions = [0.0, 0.0, 0.0, f64::NAN, 0.0, 0.0, 0.0, 1.0, 0.0];
    assert_mesh_error(
        &MeshView {
            vertices: &positions,
            ..base_mesh()
        },
        ValidationError::MeshNonFinite,
    );
    let positions = [0.0, 0.0, 0.0, f64::INFINITY, 0.0, 0.0, 0.0, 1.0, 0.0];
    assert_mesh_error(
        &MeshView {
            vertices: &positions,
            ..base_mesh()
        },
        ValidationError::MeshNonFinite,
    );
    let normals = [0.0, 0.0, 1.0, f32::NEG_INFINITY, 0.0, 1.0, 0.0, 0.0, 1.0];
    assert_mesh_error(
        &MeshView {
            normals: &normals,
            ..base_mesh()
        },
        ValidationError::MeshNonFinite,
    );
    let normals = [0.0, 0.0, 1.0, f32::NAN, 0.0, 1.0, 0.0, 0.0, 1.0];
    assert_mesh_error(
        &MeshView {
            normals: &normals,
            ..base_mesh()
        },
        ValidationError::MeshNonFinite,
    );
}

#[test]
fn triangle_indices_must_stay_below_the_vertex_count() {
    assert_valid(&base_mesh());
    let triangles = [0, 1, 2];
    assert_valid(&MeshView {
        triangles: &triangles,
        ..base_mesh()
    });
    for triangles in [[0, 1, 3], [3, 0, 0], [u32::MAX, 0, 0]] {
        assert_mesh_error(
            &MeshView {
                triangles: &triangles,
                ..base_mesh()
            },
            ValidationError::MeshIndexOutOfBounds,
        );
    }
    let vertices = [
        0.0, 0.0, 0.0, //
        1.0, 0.0, 0.0, //
        0.0, 1.0, 0.0, //
        0.0, 0.0, 1.0,
    ];
    let triangles = [0, 1, 3];
    assert_valid(&MeshView {
        vertices: &vertices,
        normals: &[],
        triangles: &triangles,
        ..base_mesh()
    });
}

#[test]
fn normals_length_mismatch_is_a_sidecar_length_error() {
    let normals = [0.0_f32; 3];
    assert_mesh_error(
        &MeshView {
            normals: &normals,
            ..base_mesh()
        },
        ValidationError::MeshSidecarLength,
    );
    let normals = [0.0_f32; 12];
    assert_mesh_error(
        &MeshView {
            normals: &normals,
            ..base_mesh()
        },
        ValidationError::MeshSidecarLength,
    );
}

#[test]
fn every_per_triangle_sidecar_length_is_checked() {
    let wrong_i32 = [0_i32; 2];
    let wrong_u8 = [0_u8; 2];
    assert_mesh_error(
        &MeshView {
            tri_material: &wrong_i32,
            ..base_mesh()
        },
        ValidationError::MeshSidecarLength,
    );
    assert_mesh_error(
        &MeshView {
            tri_wire_edges: &wrong_u8,
            ..base_mesh()
        },
        ValidationError::MeshSidecarLength,
    );
    let source_body = [1_i32; 2];
    let source_pgon = [1_i32; 2];
    assert_mesh_error(
        &MeshView {
            tri_source_body: &source_body,
            tri_source_pgon: &source_pgon,
            ..base_mesh()
        },
        ValidationError::MeshSidecarLength,
    );
    let source_body = [1_i32; 1];
    let source_pgon = [1_i32; 2];
    assert_mesh_error(
        &MeshView {
            tri_source_body: &source_body,
            tri_source_pgon: &source_pgon,
            ..base_mesh()
        },
        ValidationError::MeshSidecarLength,
    );
    assert_mesh_error(
        &MeshView {
            tri_surface_attribute: &wrong_i32,
            ..base_mesh()
        },
        ValidationError::MeshSidecarLength,
    );
}

#[test]
fn source_body_and_polygon_must_be_joint_positive_one_based_sidecars() {
    assert_valid(&MeshView {
        tri_source_body: &[],
        tri_source_pgon: &[],
        ..base_mesh()
    });
    assert_mesh_error(
        &MeshView {
            tri_source_pgon: &[],
            ..base_mesh()
        },
        ValidationError::MeshSourceSidecarInvalid,
    );
    assert_mesh_error(
        &MeshView {
            tri_source_body: &[],
            ..base_mesh()
        },
        ValidationError::MeshSourceSidecarInvalid,
    );
    for body in [[0_i32; 1], [-1_i32; 1]] {
        assert_mesh_error(
            &MeshView {
                tri_source_body: &body,
                ..base_mesh()
            },
            ValidationError::MeshSourceSidecarInvalid,
        );
    }
    for pgon in [[0_i32; 1], [-2_i32; 1]] {
        assert_mesh_error(
            &MeshView {
                tri_source_pgon: &pgon,
                ..base_mesh()
            },
            ValidationError::MeshSourceSidecarInvalid,
        );
    }
    let positive_body = [9_i32; 1];
    let positive_pgon = [4_i32; 1];
    assert_valid(&MeshView {
        tri_source_body: &positive_body,
        tri_source_pgon: &positive_pgon,
        ..base_mesh()
    });
}

#[test]
fn degenerate_triangles_and_unused_vertices_are_accepted() {
    for triangles in [[0_u32, 0, 1], [2_u32, 2, 2]] {
        assert_valid(&MeshView {
            triangles: &triangles,
            ..base_mesh()
        });
    }
    let collinear_vertices = [
        0.0, 0.0, 0.0, //
        1.0, 0.0, 0.0, //
        2.0, 0.0, 0.0, //
        3.0, 4.0, 5.0,
    ];
    let collinear_triangle = [0_u32, 1, 2];
    assert_valid(&MeshView {
        vertices: &collinear_vertices,
        normals: &[],
        triangles: &collinear_triangle,
        ..base_mesh()
    });
}

#[test]
fn large_survey_coordinates_remain_exact_f64_without_mutation() {
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
    let bits_before = vertices.map(f64::to_bits);
    assert_valid(&MeshView {
        vertices: &vertices,
        ..base_mesh()
    });
    assert_eq!(vertices.map(f64::to_bits), bits_before);
    assert_eq!(
        vertices.first().map(|value| value.to_bits()),
        Some(EASTING.to_bits())
    );
}

fn next_seed(seed: &mut u64) -> u64 {
    *seed = seed
        .wrapping_mul(6_364_136_223_846_793_005)
        .wrapping_add(1_442_695_040_888_963_407);
    *seed
}

#[test]
fn seeded_repeated_validation_is_deterministic_and_non_mutating() {
    let mut seed = 0x05ee_d0f1_1e9a_0001_u64;
    for _ in 0..256 {
        let vertices: [f64; 9] = std::array::from_fn(|_| {
            let numerator = u32::try_from(next_seed(&mut seed) % 1_000_000).unwrap_or(0);
            f64::from(numerator) / 1_000.0
        });
        let normals: [f32; 9] = std::array::from_fn(|_| {
            let numerator = u16::try_from(next_seed(&mut seed) % 2_000).unwrap_or(0);
            f32::from(numerator) / 1_000.0
        });
        let mesh = MeshView {
            vertices: &vertices,
            normals: &normals,
            ..base_mesh()
        };
        let vertices_before = vertices.map(f64::to_bits);
        let normals_before = normals.map(f32::to_bits);
        assert_valid(&mesh);
        assert_valid(&mesh);
        assert_eq!(vertices.map(f64::to_bits), vertices_before);
        assert_eq!(normals.map(f32::to_bits), normals_before);
    }
}

#[test]
fn snapshot_rejects_duplicate_nonempty_guids_even_when_complete() {
    let meshes = [
        MeshView {
            guid: "duplicate",
            ..base_mesh()
        },
        MeshView {
            guid: "duplicate",
            ..base_mesh()
        },
    ];
    let snapshot = SnapshotView {
        scope: "all",
        complete_model: true,
        meshes: &meshes,
        id: 1,
        capture_stamp: 1,
    };
    assert_eq!(
        validate_snapshot(&snapshot),
        Err(ValidationError::SnapshotDuplicateGuid)
    );
}

#[test]
fn snapshot_allows_blank_and_repeated_anonymous_guids() {
    let meshes = [
        MeshView {
            guid: "",
            ..base_mesh()
        },
        MeshView {
            guid: "",
            ..base_mesh()
        },
        MeshView {
            guid: "named",
            ..base_mesh()
        },
    ];
    let snapshot = SnapshotView {
        scope: "selection",
        complete_model: false,
        meshes: &meshes,
        id: 2,
        capture_stamp: 3,
    };
    assert_eq!(validate_snapshot(&snapshot), Ok(()));
}

#[test]
fn snapshot_reports_the_first_invalid_mesh() {
    let invalid_triangles = [0_u32, 1, 3];
    let meshes = [
        base_mesh(),
        MeshView {
            guid: "invalid",
            triangles: &invalid_triangles,
            ..base_mesh()
        },
    ];
    let snapshot = SnapshotView {
        scope: "all",
        complete_model: false,
        meshes: &meshes,
        id: 4,
        capture_stamp: 5,
    };
    assert_eq!(
        validate_snapshot(&snapshot),
        Err(ValidationError::MeshIndexOutOfBounds)
    );
}

#[test]
fn error_messages_expose_stable_code_prefixes() {
    let cases = [
        (ValidationError::MeshArrayLength, "MeshArrayLength:"),
        (ValidationError::MeshNonFinite, "MeshNonFinite:"),
        (
            ValidationError::MeshIndexOutOfBounds,
            "MeshIndexOutOfBounds:",
        ),
        (ValidationError::MeshSidecarLength, "MeshSidecarLength:"),
        (
            ValidationError::MeshSourceSidecarInvalid,
            "MeshSourceSidecarInvalid:",
        ),
        (
            ValidationError::SnapshotDuplicateGuid,
            "SnapshotDuplicateGuid:",
        ),
    ];
    for (error, prefix) in cases {
        assert!(error.message().starts_with(prefix));
        assert_eq!(error.to_string(), error.message());
    }
}
