//! Foreign-memory refusal matrix, sharing the attributed fixture above.
use super::*;

fn alias_provenance(input: &mut ProvenanceRequest, pool: u32, pointer: *const u64) {
    let table = &mut input.table;
    match pool {
        0 => {
            table.sources = SliceView {
                data: pointer.cast(),
                count: 1,
            }
        }
        1 => {
            table.operations = SliceView {
                data: pointer.cast(),
                count: 1,
            }
        }
        2 => {
            table.objects = SliceView {
                data: pointer.cast(),
                count: 1,
            }
        }
        3 => {
            table.bindings = SliceView {
                data: pointer.cast(),
                count: 1,
            }
        }
        4 => {
            table.binding_refs = SliceView {
                data: pointer.cast(),
                count: 1,
            }
        }
        5 => {
            table.source_ids = SliceView {
                data: pointer.cast(),
                count: 1,
            }
        }
        6 => {
            table.operation_ids = SliceView {
                data: pointer.cast(),
                count: 1,
            }
        }
        7 => {
            table.predecessors = SliceView {
                data: pointer.cast(),
                count: 1,
            }
        }
        8 => {
            table.mappings = SliceView {
                data: pointer.cast(),
                count: 1,
            }
        }
        9 => {
            table.role_bytes = SliceView {
                data: pointer.cast(),
                count: 1,
            }
        }
        _ => {
            table.support_tables = SliceView {
                data: pointer.cast(),
                count: 1,
            }
        }
    }
}

#[test]
fn every_nested_pool_protects_unused_output_capacity_and_diagnostics() {
    let rows = rows(id::EntityCounts {
        vertices: 4,
        edges: 4,
        faces: 0,
        patches: 0,
    });
    for pool in 0..14 {
        let mut input = request(&rows);
        let mut storage = [0x5555_u64; 64];
        let before = storage;
        let pointer = storage.as_mut_ptr();
        let alias = pointer.wrapping_add(48);
        if pool < 11 {
            alias_provenance(&mut input.provenance, pool, alias);
        } else {
            match pool {
                11 => {
                    input.points = SliceView {
                        data: alias.cast(),
                        count: 3,
                    }
                }
                12 => {
                    input.constraints = SliceView {
                        data: alias.cast(),
                        count: 1,
                    }
                }
                _ => {
                    input.constraint_vertices = SliceView {
                        data: alias.cast(),
                        count: 1,
                    }
                }
            }
        }
        let mut counts = TerrainCounts::default();
        let mut output = query(&mut counts);
        // The declared capacity aliases only a distant tail, not its prefix.
        output.points = pointer.cast();
        output.point_capacity = 64;
        output.query_only = 0;
        input.provenance.table.context.schema_version = 99;
        let mut error = [0x55; 64];
        // SAFETY: all recognized ranges fit live storage. Deliberate alias and
        // malformed metadata must refuse before borrowing any pointed-to value.
        let status = unsafe {
            tapioca_terrain_triangulate(
                &raw const input,
                &raw const output,
                error.as_mut_ptr(),
                error.len(),
            )
        };
        assert_eq!(status, 1, "pool {pool}");
        assert_eq!(storage, before);
        assert_eq!(error, [0x55; 64]);
    }
}

#[test]
fn every_output_pair_and_descriptor_overlap_refuses_without_writes() {
    let rows = rows(id::EntityCounts {
        vertices: 4,
        edges: 4,
        faces: 0,
        patches: 0,
    });
    for left in 0..4 {
        for right in left + 1..4 {
            let input = request(&rows);
            let mut storage = [[0x5555_u64; 64]; 4];
            let before = storage;
            let mut pointers = storage.each_mut().map(|array| array.as_mut_ptr());
            let same = pointers.get(left).copied().unwrap_or(std::ptr::null_mut());
            if let Some(pointer) = pointers.get_mut(right) {
                *pointer = same;
            }
            let [count, points, triangles, mappings] = pointers;
            let output = TerrainOutput {
                counts: count.cast(),
                points: points.cast(),
                point_capacity: 1,
                triangles: triangles.cast(),
                triangle_capacity: 1,
                mappings: mappings.cast(),
                mapping_capacity: 1,
                query_only: 0,
            };
            let mut error = [0x55; 64];
            // SAFETY: live aligned storage; pairwise writable overlap is rejected
            // before either typed storage or diagnostics is accessed.
            let status = unsafe {
                tapioca_terrain_triangulate(
                    &raw const input,
                    &raw const output,
                    error.as_mut_ptr(),
                    error.len(),
                )
            };
            assert_eq!(status, 1);
            assert_eq!(storage, before);
            assert_eq!(error, [0x55; 64]);
        }
    }
    let input = request(&rows);
    let mut counts = TerrainCounts::default();
    let mut output = query(&mut counts);
    output.counts = (&raw const input).cast_mut().cast();
    let mut error = [0x55; 64];
    // SAFETY: descriptor/output alias is checked before any write through it.
    let status = unsafe {
        tapioca_terrain_triangulate(
            &raw const input,
            &raw const output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 1);
    assert_eq!(error, [0x55; 64]);
}

#[test]
fn malformed_shapes_and_error_storage_preserve_counts() {
    let rows = rows(id::EntityCounts {
        vertices: 4,
        edges: 4,
        faces: 0,
        patches: 0,
    });
    for kind in 0..8 {
        let mut input = request(&rows);
        let sentinel = TerrainCounts {
            points: 77,
            triangles: 88,
            mappings: 99,
        };
        let mut counts = sentinel;
        let mut output = query(&mut counts);
        let mut error = [0_u8; 128];
        match kind {
            0 => {
                input.points = SliceView {
                    data: std::ptr::null(),
                    count: 3,
                }
            }
            1 => {
                input.points = SliceView {
                    data: std::ptr::without_provenance(1),
                    count: 3,
                }
            }
            2 => input.points.count = usize::MAX,
            3 => output.counts = std::ptr::null_mut(),
            4 => {
                output.points = std::ptr::null_mut();
                output.point_capacity = 1;
            }
            5 => {
                output.points = std::ptr::without_provenance_mut(1);
                output.point_capacity = 1;
            }
            6 => {
                output.points = error.as_mut_ptr().cast();
                output.point_capacity = usize::MAX;
            }
            _ => {}
        }
        let (pointer, capacity) = if kind == 7 {
            (std::ptr::null_mut(), 1)
        } else {
            (error.as_mut_ptr(), error.len())
        };
        // SAFETY: invalid representations are recognized before dereference;
        // all remaining inputs/counts are live disjoint fixture allocations.
        let status = unsafe {
            tapioca_terrain_triangulate(&raw const input, &raw const output, pointer, capacity)
        };
        assert_eq!(status, 1, "kind {kind}");
        assert_eq!(counts, sentinel);
    }
}

#[test]
fn split_all_pools_protect_error_alias_with_a_malformed_output_descriptor() {
    let triangles = [0, 1, 2, 0, 2, 3];
    let counts = id::EntityCounts {
        vertices: 8,
        edges: 4,
        faces: 2,
        patches: 0,
    };
    let rows = rows(counts);
    for pool in 0..15 {
        let mut input = TerrainSplitRequest {
            points: view(&POINTS),
            triangles: view(&triangles),
            line: view(&POINTS),
            arc_angles: empty(),
            provenance: provenance(&rows, counts, 7),
        };
        let mut storage = [0x5555_u64; 64];
        let before = storage;
        let pointer = storage.as_mut_ptr();
        if pool < 11 {
            alias_provenance(&mut input.provenance, pool, pointer);
        } else {
            match pool {
                11 => {
                    input.points = SliceView {
                        data: pointer.cast(),
                        count: 3,
                    }
                }
                12 => {
                    input.triangles = SliceView {
                        data: pointer.cast(),
                        count: 3,
                    }
                }
                13 => {
                    input.line = SliceView {
                        data: pointer.cast(),
                        count: 3,
                    }
                }
                _ => {
                    input.arc_angles = SliceView {
                        data: pointer.cast(),
                        count: 1,
                    }
                }
            }
        }
        // SAFETY: known error/input aliases must beat the unreadable output
        // descriptor. No aliased pool is borrowed or interpreted as a value.
        let status = unsafe {
            tapioca_terrain_split_mesh(&raw const input, std::ptr::null(), pointer.cast(), 8)
        };
        assert_eq!(status, 1);
        assert_eq!(storage, before, "pool {pool}");
    }
}

#[test]
fn uninitialized_output_storage_is_fully_published_on_success() {
    use std::mem::MaybeUninit;
    let rows = rows(id::EntityCounts {
        vertices: 4,
        edges: 4,
        faces: 0,
        patches: 0,
    });
    let input = request(&rows);
    let mut counts = TerrainCounts::default();
    let query = query(&mut counts);
    // SAFETY: live input pools, count and error discard; no aliases.
    assert_eq!(
        // SAFETY: query borrows live immutable fixtures and writes only counts.
        unsafe {
            tapioca_terrain_triangulate(&raw const input, &raw const query, std::ptr::null_mut(), 0)
        },
        0
    );
    let mut points = vec![MaybeUninit::<f64>::uninit(); counts.points];
    let mut triangles = vec![MaybeUninit::<u32>::uninit(); counts.triangles];
    let mut mappings = vec![MaybeUninit::<TerrainMapping>::uninit(); counts.mappings];
    let mut result = MaybeUninit::<TerrainCounts>::uninit();
    let output = TerrainOutput {
        counts: result.as_mut_ptr(),
        points: points.as_mut_ptr().cast(),
        point_capacity: points.len(),
        triangles: triangles.as_mut_ptr().cast(),
        triangle_capacity: triangles.len(),
        mappings: mappings.as_mut_ptr().cast(),
        mapping_capacity: mappings.len(),
        query_only: 0,
    };
    // SAFETY: uninitialized output is valid exclusive writable storage, and the
    // successful call initializes every requested prefix and count record.
    let status = unsafe {
        tapioca_terrain_triangulate(&raw const input, &raw const output, std::ptr::null_mut(), 0)
    };
    assert_eq!(status, 0);
    if status == 0 {
        // SAFETY: success above initializes the complete counts record.
        assert_eq!(unsafe { result.assume_init() }, counts);
        for point in points {
            // SAFETY: every point scalar lies in the published prefix.
            assert!(unsafe { point.assume_init() }.is_finite());
        }
        for triangle in triangles {
            // SAFETY: every triangle index lies in the published prefix.
            assert!(unsafe { triangle.assume_init() } < 4);
        }
        for mapping in mappings {
            // SAFETY: every mapping record lies in the published prefix.
            assert!((1..=11).contains(&unsafe { mapping.assume_init() }.kind));
        }
    }
}

#[test]
#[expect(
    clippy::panic,
    reason = "Deliberate unwind between terrain preparation and publication"
)]
fn contained_panic_after_computation_leaves_counts_unchanged() {
    let rows = rows(id::EntityCounts {
        vertices: 4,
        edges: 4,
        faces: 0,
        patches: 0,
    });
    let input = request(&rows);
    let sentinel = TerrainCounts {
        points: 77,
        triangles: 88,
        mappings: 99,
    };
    let mut counts = sentinel;
    let output = query(&mut counts);
    let mut error = [0; 128];
    // SAFETY: same shared envelope and full preflight as production exports;
    // panic occurs while results are owned, before the raw publication phase.
    let status = unsafe {
        call(error.as_mut_ptr(), error.len(), || {
            let _prepared = triangulate(
                &raw const input,
                &raw const output,
                memory_range(error.as_ptr(), error.len())?,
            )?;
            panic!("terrain pre-publication fixture")
        })
    };
    assert_eq!(status, 3);
    assert_eq!(counts, sentinel);
    assert!(error.starts_with(b"Rust panic contained"));
}
