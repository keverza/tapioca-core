use super::*;
use crate::identity::ProvenanceView;
use tapioca_geometry::identity as id;

const POINTS: [f64; 9] = [
    -0.0,
    6_000_000.0,
    120.0,
    700_002.0,
    6_000_000.0,
    120.0,
    700_002.0,
    6_000_003.0,
    120.0,
];
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
fn mappings() -> Vec<id::MappingRecord> {
    [(1, 3), (2, 3), (3, 1)]
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
fn request(rows: &[id::MappingRecord]) -> AttributedPolygonRequest {
    AttributedPolygonRequest {
        points: view(&POINTS),
        provenance: ProvenanceRequest {
            expected_session: [1; 16],
            expected_revision: 0,
            counts: id::EntityCounts {
                vertices: 3,
                edges: 3,
                faces: 1,
                patches: 0,
            },
            domains: 7,
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
        },
    }
}

#[test]
fn layout_and_successful_prefix_preserve_coordinate_bits() {
    if size_of::<usize>() == 8 {
        assert_eq!(size_of::<AttributedPolygonRequest>(), 296);
        assert_eq!(
            std::mem::offset_of!(AttributedPolygonRequest, provenance),
            16
        );
    }
    let rows = mappings();
    let input = request(&rows);
    let mut output = [99.0; 12];
    let mut error = [0x7f; 256];
    // SAFETY: live immutable descriptors/pools, disjoint writable local output/error.
    let status = unsafe {
        tapioca_copy_attributed_polygon(
            &raw const input,
            output.as_mut_ptr(),
            output.len(),
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 0);
    assert_eq!(error.first(), Some(&0));
    let mut expected = [99.0; 12];
    for (out, value) in expected.iter_mut().zip(POINTS) {
        *out = value;
    }
    assert_eq!(output.map(f64::to_bits), expected.map(f64::to_bits));
    assert_eq!(input.provenance.table.context.table_id, 100);
}

#[test]
fn empty_null_arrays_and_uninitialized_output_are_supported() {
    let rows = mappings();
    let mut input = request(&rows);
    let mut output = std::mem::MaybeUninit::<[f64; 9]>::uninit();
    // SAFETY: real exclusive uninitialized output is written only on success.
    let status = unsafe {
        tapioca_copy_attributed_polygon(
            &raw const input,
            output.as_mut_ptr().cast(),
            9,
            std::ptr::null_mut(),
            0,
        )
    };
    assert_eq!(status, 0);
    // SAFETY: successful call initialized all nine scalars.
    let output = unsafe { output.assume_init() };
    assert_eq!(output.map(f64::to_bits), POINTS.map(f64::to_bits));
    input.points = empty();
    input.provenance.counts = id::EntityCounts {
        vertices: 0,
        edges: 0,
        faces: 0,
        patches: 0,
    };
    input.provenance.table.mappings = empty();
    // SAFETY: empty coordinate output is null/zero; required immutable table retained.
    let status = unsafe {
        tapioca_copy_attributed_polygon(
            &raw const input,
            std::ptr::null_mut(),
            0,
            std::ptr::null_mut(),
            0,
        )
    };
    assert_eq!(status, 0);
}

#[test]
fn numeric_shape_and_structural_refusals_preserve_all_capacity() {
    let bad = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, f64::NAN, 0.0];
    let rows = mappings();
    for kind in 0..9 {
        let mut input = request(&rows);
        let mut output = [99.0; 12];
        let mut error = [0; 128];
        match kind {
            0 => input.points.count = 8,
            1 => input.points = view(&bad),
            2 => input.provenance.counts.edges = 0,
            3 => input.provenance.table.mappings = empty(),
            4 => input.provenance.expected_revision = 1,
            5 => input.provenance.require_object = 2,
            6 => input.provenance.table.objects = empty(),
            7 => input.provenance.domains = 5,
            _ => input.provenance.table.context.schema_version = 99,
        }
        // SAFETY: all descriptors/arrays live and disjoint; invalid values are checked.
        let status = unsafe {
            tapioca_copy_attributed_polygon(
                &raw const input,
                output.as_mut_ptr(),
                output.len(),
                error.as_mut_ptr(),
                error.len(),
            )
        };
        assert_eq!(
            status,
            if matches!(kind, 0 | 1 | 5) { 1 } else { 2 },
            "kind {kind}"
        );
        assert_ne!(error.first(), Some(&0));
        assert_eq!(output.map(f64::to_bits), [99.0; 12].map(f64::to_bits));
    }
}

#[test]
fn malformed_memory_and_capacity_refuse_before_publication() {
    let rows = mappings();
    for kind in 0..10 {
        let mut input = request(&rows);
        let mut output = [99.0; 12];
        let mut error = [0; 128];
        let mut source = &raw const input;
        let mut target = output.as_mut_ptr();
        let mut capacity = output.len();
        match kind {
            0 => source = std::ptr::null(),
            1 => source = source.cast::<u8>().wrapping_add(1).cast(),
            2 => target = std::ptr::null_mut(),
            3 => target = target.cast::<u8>().wrapping_add(1).cast(),
            4 => capacity = usize::MAX,
            5 => capacity = 8,
            6 => input.points.data = std::ptr::null(),
            7 => input.points.count = usize::MAX,
            8 => input.provenance.table.mappings.data = std::ptr::null(),
            _ => input.provenance.table.sources.count = usize::MAX,
        }
        // SAFETY: malformed pointer/range shapes rejected before access, other pools live.
        let status = unsafe {
            tapioca_copy_attributed_polygon(
                source,
                target,
                capacity,
                error.as_mut_ptr(),
                error.len(),
            )
        };
        assert_eq!(status, 1, "kind {kind}");
        assert_eq!(output.map(f64::to_bits), [99.0; 12].map(f64::to_bits));
    }
}

fn alias_pool(input: &mut AttributedPolygonRequest, index: usize, data: *const u64) {
    let table = &mut input.provenance.table;
    match index {
        0 => {
            input.points = SliceView {
                data: data.cast(),
                count: 9,
            }
        }
        1 => {
            table.sources = SliceView {
                data: data.cast(),
                count: 1,
            }
        }
        2 => {
            table.operations = SliceView {
                data: data.cast(),
                count: 1,
            }
        }
        3 => {
            table.objects = SliceView {
                data: data.cast(),
                count: 1,
            }
        }
        4 => {
            table.bindings = SliceView {
                data: data.cast(),
                count: 1,
            }
        }
        5 => table.binding_refs = SliceView { data, count: 1 },
        6 => {
            table.source_ids = SliceView {
                data: data.cast(),
                count: 1,
            }
        }
        7 => {
            table.operation_ids = SliceView {
                data: data.cast(),
                count: 1,
            }
        }
        8 => {
            table.predecessors = SliceView {
                data: data.cast(),
                count: 1,
            }
        }
        9 => {
            table.mappings = SliceView {
                data: data.cast(),
                count: 1,
            }
        }
        10 => {
            table.role_bytes = SliceView {
                data: data.cast(),
                count: 1,
            }
        }
        _ => {
            table.support_tables = SliceView {
                data: data.cast(),
                count: 1,
            }
        }
    }
}

#[test]
fn every_point_and_provenance_pool_is_disjoint_from_output_and_error() {
    let rows = mappings();
    for pool in 0..12 {
        for variant in 0..4 {
            let error_alias = variant != 0;
            let mut input = request(&rows);
            let mut backing = [77_u64; 128];
            let mut output = [99.0; 12];
            let mut error = [0x7f; 128];
            alias_pool(&mut input, pool, backing.as_ptr());
            // Competing malformed pool must not trigger a diagnostic write over a
            // known later alias; for sources alias use malformed points instead.
            if pool == 0 {
                input.provenance.table.sources.data = std::ptr::null();
            } else {
                input.points.data = std::ptr::null();
            }
            let mut target = if error_alias {
                output.as_mut_ptr()
            } else {
                backing.as_mut_ptr().cast()
            };
            let message = if error_alias {
                backing.as_mut_ptr().cast()
            } else {
                error.as_mut_ptr()
            };
            if variant == 2 {
                target = std::ptr::null_mut();
            }
            let capacity = if variant == 3 { usize::MAX } else { 12 };
            // SAFETY: recognized alias wins before foreign borrow/write. Backing
            // is large/aligned for each declared POD, never interpreted as one.
            let status = unsafe {
                tapioca_copy_attributed_polygon(&raw const input, target, capacity, message, 128)
            };
            assert_eq!(status, 1, "pool {pool}, error {error_alias}");
            assert_eq!(backing, [77; 128]);
            assert_eq!(error, [0x7f; 128]);
            assert_eq!(output.map(f64::to_bits), [99.0; 12].map(f64::to_bits));
        }
    }
}

#[test]
fn descriptor_unused_capacity_and_malformed_error_overlap_refuse() {
    let rows = mappings();
    for kind in 0..5 {
        let mut input = request(&rows);
        let mut output = [99.0; 12];
        let mut error = [0x7f; 128];
        let mut target = output.as_mut_ptr();
        let mut message = error.as_mut_ptr();
        let mut capacity = error.len();
        match kind {
            0 => target = (&raw mut input).cast(),
            1 => {
                message = (&raw mut input).cast();
                capacity = size_of_val(&input);
            }
            2 => {
                message = output.as_mut_ptr().wrapping_add(11).cast();
                capacity = 8;
                input.points.data = std::ptr::null();
            }
            3 => message = std::ptr::null_mut(),
            _ => capacity = usize::MAX,
        }
        // SAFETY: recognized overlap/malformed error rejected before accessing it.
        let status = unsafe {
            tapioca_copy_attributed_polygon(&raw const input, target, 12, message, capacity)
        };
        assert_eq!(status, 1);
        assert_eq!(error, [0x7f; 128]);
        assert_eq!(output.map(f64::to_bits), [99.0; 12].map(f64::to_bits));
        assert_eq!(input.provenance.domains, 7);
    }
}

#[test]
fn input_overlapping_only_unused_output_capacity_still_refuses() {
    let rows = mappings();
    let mut input = request(&rows);
    let mut storage = [99.0; 24];
    let mut error = [0x7f; 128];
    input.points = SliceView {
        data: storage.as_ptr().wrapping_add(9),
        count: 9,
    };
    // SAFETY: both ranges fit live storage; only the unused output suffix
    // intersects input, recognized and refused before creating either borrow.
    let status = unsafe {
        tapioca_copy_attributed_polygon(
            &raw const input,
            storage.as_mut_ptr(),
            12,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 1);
    assert_eq!(storage.map(f64::to_bits), [99.0; 24].map(f64::to_bits));
    assert_eq!(error, [0x7f; 128]);
}
