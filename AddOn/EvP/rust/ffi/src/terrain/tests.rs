use super::*;
use crate::identity::ProvenanceView;

mod band;
mod memory;
mod operators;

const POINTS: [f64; 12] = [
    0.0, 0.0, 0.0, 10.0, 0.0, 0.0, 10.0, 10.0, 8.0, 0.0, 10.0, 0.0,
];
const VERTICES: [u32; 4] = [0, 1, 2, 3];
const CONSTRAINTS: [TerrainConstraint; 1] = [TerrainConstraint {
    role: 1,
    offset: 0,
    count: 4,
}];
const SOURCES: [id::SourceRecord; 1] = [id::SourceRecord {
    id: id::SourceId(11),
    bindings: id::Range {
        offset: 0,
        count: 0,
    },
}];
const OPERATIONS: [id::OperationRecord; 1] = [id::OperationRecord {
    id: id::OperationId(12),
    parents: id::Range {
        offset: 0,
        count: 0,
    },
}];
const OBJECTS: [id::ObjectRecord; 1] = [id::ObjectRecord {
    id: id::ObjectId(13),
    operation: id::OperationId(12),
}];
const SOURCE_IDS: [id::SourceId; 1] = [id::SourceId(11)];
const OPERATION_IDS: [id::OperationId; 1] = [id::OperationId(12)];

fn view<T>(values: &[T]) -> SliceView<T> {
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
fn rows(counts: id::EntityCounts) -> Vec<id::MappingRecord> {
    [(1, counts.vertices), (2, counts.edges), (3, counts.faces)]
        .into_iter()
        .flat_map(|(domain, count)| {
            (0..count).map(move |index| id::MappingRecord {
                domain,
                kind: 5,
                output_index: index,
                sources: id::Range {
                    offset: 0,
                    count: 1,
                },
                operations: id::Range {
                    offset: 0,
                    count: 1,
                },
                object: id::ObjectId(13),
                predecessors: id::Range {
                    offset: 0,
                    count: 0,
                },
                patch_key: 0,
            })
        })
        .collect()
}
fn provenance(
    rows: &[id::MappingRecord],
    counts: id::EntityCounts,
    domains: u32,
) -> ProvenanceRequest {
    ProvenanceRequest {
        expected_session: [1; 16],
        expected_revision: 0,
        counts,
        domains,
        require_object: 1,
        table: ProvenanceView {
            context: id::Context {
                schema_version: 1,
                session_uuid: [1; 16],
                revision: 0,
                table_id: 100,
            },
            sources: view(&SOURCES),
            operations: view(&OPERATIONS),
            objects: view(&OBJECTS),
            bindings: empty(),
            binding_refs: empty(),
            source_ids: view(&SOURCE_IDS),
            operation_ids: view(&OPERATION_IDS),
            predecessors: empty(),
            mappings: view(rows),
            role_bytes: empty(),
            support_tables: empty(),
        },
    }
}
fn request(rows: &[id::MappingRecord]) -> TerrainRequest {
    TerrainRequest {
        points: view(&POINTS),
        constraints: view(&CONSTRAINTS),
        constraint_vertices: view(&VERTICES),
        z_tolerance: 1e-9,
        refinement: TerrainRefinement::default(),
        provenance: provenance(
            rows,
            id::EntityCounts {
                vertices: 4,
                edges: 4,
                faces: 0,
                patches: 0,
            },
            3,
        ),
    }
}
fn query(counts: &mut TerrainCounts) -> TerrainOutput {
    TerrainOutput {
        counts,
        points: std::ptr::null_mut(),
        point_capacity: 0,
        triangles: std::ptr::null_mut(),
        triangle_capacity: 0,
        mappings: std::ptr::null_mut(),
        mapping_capacity: 0,
        query_only: 1,
    }
}

#[test]
fn layout_and_query_fill_match_safe_kernel_with_unchanged_tails() {
    assert_eq!(size_of::<TerrainMapping>(), 32);
    assert_eq!(size_of::<TerrainCounts>(), 24);
    assert_eq!(size_of::<TerrainOutput>(), 64);
    assert_eq!(size_of::<TerrainRefinement>(), 32);
    assert_eq!(size_of::<TerrainRequest>(), 368);
    assert_eq!(size_of::<TerrainSplitRequest>(), 344);
    assert_eq!(std::mem::offset_of!(TerrainRequest, provenance), 88);
    assert_eq!(std::mem::offset_of!(TerrainSplitRequest, provenance), 64);
    assert_eq!(std::mem::offset_of!(TerrainOutput, query_only), 56);
    let rows = rows(id::EntityCounts {
        vertices: 4,
        edges: 4,
        faces: 0,
        patches: 0,
    });
    let mut input = request(&rows);
    for refine in [false, true] {
        input.refinement = TerrainRefinement {
            enabled: u32::from(refine),
            has_max_area: 1,
            min_angle_degrees: 20.0,
            max_area_square_metres: 2.0,
            max_added_vertices: 10_000,
        };
        let mut counts = TerrainCounts::default();
        let mut output = query(&mut counts);
        // SAFETY: live immutable inputs and exclusively writable count; no aliases.
        assert_eq!(
            // SAFETY: live checked fixture allocations and disjoint output storage.
            unsafe {
                tapioca_terrain_triangulate(
                    &raw const input,
                    &raw const output,
                    std::ptr::null_mut(),
                    0,
                )
            },
            0
        );
        let expected = counts;
        let mut points = vec![77.0; counts.points + 3];
        let mut triangles = vec![77; counts.triangles + 3];
        let sentinel = TerrainMapping {
            kind: 77,
            index: 77,
            values: [77; 6],
        };
        let mut mappings = vec![sentinel; counts.mappings + 1];
        output.points = points.as_mut_ptr();
        output.point_capacity = points.len();
        output.triangles = triangles.as_mut_ptr();
        output.triangle_capacity = triangles.len();
        output.mappings = mappings.as_mut_ptr();
        output.mapping_capacity = mappings.len();
        output.query_only = 0;
        // SAFETY: all full-capacity arrays are initialized live disjoint storage.
        assert_eq!(
            // SAFETY: all full capacities are live, initialized and disjoint.
            unsafe {
                tapioca_terrain_triangulate(
                    &raw const input,
                    &raw const output,
                    std::ptr::null_mut(),
                    0,
                )
            },
            0
        );
        let constraints = [kernel::Constraint {
            role: kernel::Role::Boundary,
            vertices: &VERTICES,
        }];
        let safe = kernel::triangulate(&kernel::Request {
            points: &POINTS,
            constraints: &constraints,
            z_tolerance: 1e-9,
            refinement: refine.then_some(kernel::Refinement {
                min_angle_degrees: 20.0,
                max_area_square_metres: Some(2.0),
                max_added_vertices: 10_000,
            }),
        });
        assert!(safe.is_ok());
        let Ok(safe) = safe else { return };
        let encoded = result::tin(safe);
        assert!(encoded.is_ok());
        let Ok(encoded) = encoded else { return };
        assert_eq!(points.get(..counts.points), Some(encoded.points.as_slice()));
        assert_eq!(
            triangles.get(..counts.triangles),
            Some(encoded.triangles.as_slice())
        );
        assert_eq!(
            mappings.get(..counts.mappings),
            Some(encoded.mappings.as_slice())
        );
        assert_eq!(counts, expected);
        assert_eq!(points.get(counts.points..), Some([77.0; 3].as_slice()));
        assert_eq!(triangles.get(counts.triangles..), Some([77; 3].as_slice()));
        assert_eq!(mappings.last(), Some(&sentinel));
    }
}

#[test]
fn capacity_numeric_provenance_and_mode_refusals_preserve_outputs() {
    let rows = rows(id::EntityCounts {
        vertices: 4,
        edges: 4,
        faces: 0,
        patches: 0,
    });
    for kind in 0..9 {
        let mut input = request(&rows);
        let sentinel = TerrainCounts {
            points: 77,
            triangles: 88,
            mappings: 99,
        };
        let mut counts = sentinel;
        let mut points = [77.0; 12];
        let mut output = query(&mut counts);
        match kind {
            0 => {
                output.query_only = 0;
            } // Insufficient capacity, including mappings.
            1 => input.z_tolerance = f64::NAN,
            2 => input.provenance.table.mappings = empty(),
            3 => input.provenance.expected_revision = 1,
            4 => input.provenance.counts.edges = 3,
            5 => input.refinement.enabled = 2,
            6 => output.query_only = 2,
            7 => {
                output.points = points.as_mut_ptr();
                output.point_capacity = 12;
            }
            _ => input.constraint_vertices = empty(),
        }
        // SAFETY: all supplied live storage remains disjoint; shapes may be invalid.
        let status = unsafe {
            tapioca_terrain_triangulate(
                &raw const input,
                &raw const output,
                std::ptr::null_mut(),
                0,
            )
        };
        assert_ne!(status, 0, "kind {kind}");
        assert_eq!(counts, sentinel);
        assert_eq!(points, [77.0; 12]);
    }
}

#[test]
fn all_known_writable_overlaps_win_over_malformed_peers() {
    let rows = rows(id::EntityCounts {
        vertices: 4,
        edges: 4,
        faces: 0,
        patches: 0,
    });
    for kind in 0..8 {
        let mut input = request(&rows);
        let mut counts = TerrainCounts::default();
        let mut output = query(&mut counts);
        let mut buffer = [0x55_u64; 64];
        let before = buffer;
        let pointer = buffer.as_mut_ptr();
        let error = pointer.cast::<u8>();
        match kind {
            0 => output.counts = pointer.cast(),
            1 => {
                output.points = pointer.cast();
                output.point_capacity = 3;
            }
            2 => {
                output.triangles = pointer.cast();
                output.triangle_capacity = 3;
            }
            3 => {
                output.mappings = pointer.cast();
                output.mapping_capacity = 1;
            }
            4 => {
                input.points = SliceView {
                    data: pointer.cast(),
                    count: 3,
                };
            }
            5 => {
                input.provenance.table.role_bytes = SliceView {
                    data: pointer.cast(),
                    count: 8,
                };
            }
            6 => {
                input.provenance.table.sources = SliceView {
                    data: pointer.cast(),
                    count: 1,
                };
            }
            _ => {
                input.provenance.table.mappings = SliceView {
                    data: pointer.cast(),
                    count: 1,
                };
            }
        }
        // A second malformed pool must not trigger diagnostics over the known alias.
        input.constraint_vertices = SliceView {
            data: std::ptr::null(),
            count: 1,
        };
        // SAFETY: aliases are deliberately invalid and recognized before any borrow/write.
        assert_eq!(
            // SAFETY: deliberately aliased ranges must be rejected before access.
            unsafe { tapioca_terrain_triangulate(&raw const input, &raw const output, error, 8) },
            1
        );
        assert_eq!(buffer, before, "kind {kind}");
    }
}

#[test]
fn split_matches_safe_mapping_and_refuses_arcs_without_publication() {
    let triangles = [0, 1, 2, 0, 2, 3];
    let line = [2.0, 2.0, 0.0, 8.0, 2.0, 0.0, 8.0, 8.0, 0.0, 2.0, 8.0, 0.0];
    let counts_in = id::EntityCounts {
        vertices: 8,
        edges: 4,
        faces: 2,
        patches: 0,
    };
    let rows = rows(counts_in);
    let mut input = TerrainSplitRequest {
        points: view(&POINTS),
        triangles: view(&triangles),
        line: view(&line),
        arc_angles: empty(),
        provenance: provenance(&rows, counts_in, 7),
    };
    let mut counts = TerrainCounts::default();
    let mut output = query(&mut counts);
    // SAFETY: immutable disjoint pools and count storage.
    assert_eq!(
        // SAFETY: live immutable input pools and disjoint writable count record.
        unsafe {
            tapioca_terrain_split_mesh(&raw const input, &raw const output, std::ptr::null_mut(), 0)
        },
        0
    );
    let mut points = vec![0.0; counts.points];
    let mut indices = vec![0; counts.triangles];
    let mut mappings = vec![
        TerrainMapping {
            kind: 0,
            index: 0,
            values: [0; 6]
        };
        counts.mappings
    ];
    output.points = points.as_mut_ptr();
    output.point_capacity = points.len();
    output.triangles = indices.as_mut_ptr();
    output.triangle_capacity = indices.len();
    output.mappings = mappings.as_mut_ptr();
    output.mapping_capacity = mappings.len();
    output.query_only = 0;
    // SAFETY: all result arrays are live exclusively writable and preflight-disjoint.
    assert_eq!(
        // SAFETY: full output arrays are exclusively writable and disjoint.
        unsafe {
            tapioca_terrain_split_mesh(&raw const input, &raw const output, std::ptr::null_mut(), 0)
        },
        0
    );
    let safe = kernel::split_mesh(&kernel::SplitRequest {
        points: &POINTS,
        triangles: &triangles,
        line: &line,
        arc_angles: &[],
    });
    assert!(safe.is_ok());
    let Ok(safe) = safe else { return };
    let encoded = result::split(safe, 4);
    assert!(encoded.is_ok());
    let Ok(encoded) = encoded else { return };
    assert_eq!(points, encoded.points);
    assert_eq!(indices, encoded.triangles);
    assert_eq!(mappings, encoded.mappings);
    let before = counts;
    let before_points = points.clone();
    let before_mappings = mappings.clone();
    let arcs = [0.1; 4];
    input.arc_angles = view(&arcs);
    // SAFETY: same disjoint storage, malformed geometry must refuse atomically.
    assert_eq!(
        // SAFETY: same live disjoint storage; arc refusal must not publish.
        unsafe {
            tapioca_terrain_split_mesh(&raw const input, &raw const output, std::ptr::null_mut(), 0)
        },
        1
    );
    assert_eq!(counts, before);
    assert_eq!(points, before_points);
    assert_eq!(mappings, before_mappings);
}

#[test]
fn malformed_output_descriptor_must_not_write_diagnostics_over_known_input() {
    let rows = rows(id::EntityCounts {
        vertices: 4,
        edges: 4,
        faces: 0,
        patches: 0,
    });
    let mut points = POINTS;
    let before = points;
    let mut input = request(&rows);
    input.points = view(&points);
    // SAFETY: input pools are live; null descriptor and recognized error/input
    // alias must refuse without reading the descriptor or writing diagnostics.
    let status = unsafe {
        tapioca_terrain_triangulate(
            &raw const input,
            std::ptr::null(),
            points.as_mut_ptr().cast(),
            8,
        )
    };
    assert_eq!(status, 1);
    assert_eq!(points, before);
}

#[test]
fn malformed_input_descriptor_must_not_write_diagnostics_over_known_output() {
    for split in [false, true] {
        let sentinel = TerrainCounts {
            points: 77,
            triangles: 88,
            mappings: 99,
        };
        let mut counts = sentinel;
        let output = query(&mut counts);
        let error = (&raw mut counts).cast();
        // SAFETY: null input and known count/error alias must refuse before
        // reading input or writing either the count record or diagnostics.
        let status = unsafe {
            if split {
                tapioca_terrain_split_mesh(std::ptr::null(), &raw const output, error, 8)
            } else {
                tapioca_terrain_triangulate(std::ptr::null(), &raw const output, error, 8)
            }
        };
        assert_eq!(status, 1);
        assert_eq!(counts, sentinel);
    }
}
