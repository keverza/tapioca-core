use super::*;

const SESSION: [u8; 16] = [0x2A; 16];
const VERTEX_MASK: u32 = 1;
const PATCH_MASK: u32 = 1 << 3;
const ZERO_COUNTS: EntityCounts = EntityCounts {
    vertices: 0,
    edges: 0,
    faces: 0,
    patches: 0,
};
const BASE_COUNTS: EntityCounts = EntityCounts {
    vertices: 1,
    edges: 0,
    faces: 0,
    patches: 0,
};

const CONTEXT: Context = Context {
    schema_version: 1,
    session_uuid: SESSION,
    revision: 0,
    table_id: 10,
};
const SUPPORT_CONTEXT: Context = Context {
    schema_version: 1,
    session_uuid: SESSION,
    revision: 0,
    table_id: 20,
};

const SOURCE_RECORD: SourceRecord = SourceRecord {
    id: SourceId(1),
    bindings: Range {
        offset: 0,
        count: 1,
    },
};
const OPERATION_1: OperationRecord = OperationRecord {
    id: OperationId(2),
    parents: Range {
        offset: 0,
        count: 0,
    },
};
const OPERATION_2: OperationRecord = OperationRecord {
    id: OperationId(3),
    parents: Range {
        offset: 0,
        count: 1,
    },
};
const OBJECT_RECORD: ObjectRecord = ObjectRecord {
    id: ObjectId(4),
    operation: OperationId(3),
};
const PREDECESSOR_0: PredecessorRecord = PredecessorRecord {
    table_id: 20,
    domain: DOMAIN_VERTEX,
    entity_index: 0,
};
const BASE_MAPPING: MappingRecord = MappingRecord {
    domain: DOMAIN_VERTEX,
    kind: KIND_EXACT,
    output_index: 0,
    sources: Range {
        offset: 0,
        count: 1,
    },
    operations: Range {
        offset: 1,
        count: 1,
    },
    object: ObjectId(4),
    predecessors: Range {
        offset: 0,
        count: 1,
    },
    patch_key: 0,
};
const SUPPORT_TABLE: SupportTable = SupportTable {
    context: SUPPORT_CONTEXT,
    counts: BASE_COUNTS,
    historical: 0,
};

const GUID_A: [u8; 36] = *b"11111111-2222-3333-4444-555555555555";
const GUID_B: [u8; 36] = *b"12345678-9ABC-DEF0-1234-56789ABCDEF0";
const GUID_LOWERCASE: [u8; 36] = *b"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
const GUID_BAD_DASH: [u8; 36] = *b"11111111x2222-3333-4444-555555555555";
const GUID_NIL: [u8; 36] = *b"00000000-0000-0000-0000-000000000000";

const SOURCE_BINDING: BindingRecord = BindingRecord {
    logical_kind: LOGICAL_SOURCE,
    logical_id: 1,
    native_guid: GUID_A,
    metadata_uuid: [0; 16],
    metadata_present: 0,
    role: Range {
        offset: 0,
        count: 4,
    },
    revision: 0,
};
const OBJECT_BINDING_A: BindingRecord = BindingRecord {
    logical_kind: LOGICAL_OBJECT,
    logical_id: 4,
    native_guid: GUID_A,
    metadata_uuid: [0; 16],
    metadata_present: 0,
    role: Range {
        offset: 0,
        count: 4,
    },
    revision: 0,
};
const OBJECT_BINDING_B: BindingRecord = BindingRecord {
    logical_kind: LOGICAL_OBJECT,
    logical_id: 4,
    native_guid: GUID_B,
    metadata_uuid: [0; 16],
    metadata_present: 0,
    role: Range {
        offset: 0,
        count: 4,
    },
    revision: 0,
};

static BASE_SOURCES: [SourceRecord; 1] = [SOURCE_RECORD];
static BASE_OPERATIONS: [OperationRecord; 2] = [OPERATION_1, OPERATION_2];
static BASE_OBJECTS: [ObjectRecord; 1] = [OBJECT_RECORD];
static BASE_BINDINGS: [BindingRecord; 3] = [SOURCE_BINDING, OBJECT_BINDING_A, OBJECT_BINDING_B];
static BASE_BINDING_REFS: [u64; 1] = [0];
static BASE_SOURCE_IDS: [SourceId; 1] = [SourceId(1)];
static BASE_OPERATION_IDS: [OperationId; 2] = [OperationId(2), OperationId(3)];
static BASE_PREDECESSORS: [PredecessorRecord; 1] = [PREDECESSOR_0];
static BASE_MAPPINGS: [MappingRecord; 1] = [BASE_MAPPING];
static BASE_SUPPORT_TABLES: [SupportTable; 1] = [SUPPORT_TABLE];
static ROLE_BYTES: [u8; 4] = *b"role";

fn empty_table() -> TableView<'static> {
    TableView {
        context: CONTEXT,
        sources: &[],
        operations: &[],
        objects: &[],
        bindings: &[],
        binding_refs: &[],
        source_ids: &[],
        operation_ids: &[],
        predecessors: &[],
        mappings: &[],
        role_bytes: &[],
        support_tables: &[],
    }
}

fn base_table() -> TableView<'static> {
    TableView {
        sources: &BASE_SOURCES,
        operations: &BASE_OPERATIONS,
        objects: &BASE_OBJECTS,
        bindings: &BASE_BINDINGS,
        binding_refs: &BASE_BINDING_REFS,
        source_ids: &BASE_SOURCE_IDS,
        operation_ids: &BASE_OPERATION_IDS,
        predecessors: &BASE_PREDECESSORS,
        mappings: &BASE_MAPPINGS,
        role_bytes: &ROLE_BYTES,
        support_tables: &BASE_SUPPORT_TABLES,
        ..empty_table()
    }
}

fn validate_with(
    table: TableView<'_>,
    counts: EntityCounts,
    domains: u32,
    expected_revision: u64,
    require_object: bool,
) -> Result<(), Error> {
    validate_provenance(&ValidationRequest {
        expected_session: SESSION,
        expected_revision,
        counts,
        domains,
        require_object,
        table,
    })
}

fn assert_valid(table: TableView<'_>) {
    assert_eq!(
        validate_with(table, BASE_COUNTS, VERTEX_MASK, 0, true),
        Ok(())
    );
}

fn assert_valid_with(
    table: TableView<'_>,
    counts: EntityCounts,
    domains: u32,
    require_object: bool,
) {
    assert_eq!(
        validate_with(table, counts, domains, 0, require_object),
        Ok(())
    );
}

fn assert_error(table: TableView<'_>, expected: Error) {
    assert_eq!(
        validate_with(table, BASE_COUNTS, VERTEX_MASK, 0, true),
        Err(expected)
    );
}

#[test]
fn revision_zero_table_with_two_object_guids_is_valid() {
    assert_valid(base_table());
}

#[test]
fn empty_geometry_is_valid_with_zero_counts_and_rejects_an_empty_domain_mask() {
    assert_valid_with(empty_table(), ZERO_COUNTS, VERTEX_MASK, true);
    assert_eq!(
        validate_with(empty_table(), ZERO_COUNTS, 0, 0, true),
        Err(Error::ProvenanceConflict)
    );
    assert_valid_with(empty_table(), ZERO_COUNTS, 0b1111, true);
}

#[test]
fn validation_does_not_mutate_the_borrowed_view() {
    let table = base_table();
    let before = table;
    assert_valid(table);
    assert_eq!(table, before);
    assert_eq!(table.context, CONTEXT);
}

#[test]
fn context_rejects_unknown_schema_foreign_sessions_and_table_zero() {
    let foreign_schema = TableView {
        context: Context {
            schema_version: 2,
            ..CONTEXT
        },
        ..base_table()
    };
    assert_error(foreign_schema, Error::ProvenanceConflict);

    let nil_session = TableView {
        context: Context {
            session_uuid: [0; 16],
            ..CONTEXT
        },
        ..base_table()
    };
    assert_error(nil_session, Error::ProvenanceConflict);

    let mismatched_session = TableView {
        context: Context {
            session_uuid: [7; 16],
            ..CONTEXT
        },
        ..base_table()
    };
    assert_error(mismatched_session, Error::ProvenanceConflict);

    let nil_table = TableView {
        context: Context {
            table_id: 0,
            ..CONTEXT
        },
        ..base_table()
    };
    assert_error(nil_table, Error::ProvenanceConflict);
}

#[test]
fn context_requires_matching_nonzero_expected_session_and_revision() {
    let nil_expected = validate_provenance(&ValidationRequest {
        expected_session: [0; 16],
        expected_revision: 0,
        counts: BASE_COUNTS,
        domains: VERTEX_MASK,
        require_object: true,
        table: base_table(),
    });
    assert_eq!(nil_expected, Err(Error::ProvenanceConflict));

    let foreign_expected = validate_provenance(&ValidationRequest {
        expected_session: [8; 16],
        expected_revision: 0,
        counts: BASE_COUNTS,
        domains: VERTEX_MASK,
        require_object: true,
        table: base_table(),
    });
    assert_eq!(foreign_expected, Err(Error::ProvenanceConflict));

    let future_table = TableView {
        context: Context {
            revision: 1,
            ..CONTEXT
        },
        ..base_table()
    };
    assert_error(future_table, Error::ProvenanceStale);

    let future_expected = validate_with(base_table(), BASE_COUNTS, VERTEX_MASK, 1, true);
    assert_eq!(future_expected, Err(Error::ProvenanceStale));
}

#[test]
fn identifiers_are_nonzero_and_unique_across_all_catalogs() {
    let zero_source = TableView {
        sources: &[SourceRecord {
            id: SourceId(0),
            ..SOURCE_RECORD
        }],
        ..base_table()
    };
    assert_error(zero_source, Error::ProvenanceConflict);

    let zero_operation = TableView {
        operations: &[
            OperationRecord {
                id: OperationId(0),
                ..OPERATION_1
            },
            OPERATION_2,
        ],
        ..base_table()
    };
    assert_error(zero_operation, Error::ProvenanceConflict);

    let zero_object = TableView {
        objects: &[ObjectRecord {
            id: ObjectId(0),
            ..OBJECT_RECORD
        }],
        ..base_table()
    };
    assert_error(zero_object, Error::ProvenanceConflict);

    let cross_catalog_duplicate = TableView {
        objects: &[ObjectRecord {
            id: ObjectId(1),
            ..OBJECT_RECORD
        }],
        ..base_table()
    };
    assert_error(cross_catalog_duplicate, Error::ProvenanceConflict);

    let duplicate_sources = TableView {
        sources: &[SOURCE_RECORD, SOURCE_RECORD],
        ..base_table()
    };
    assert_error(duplicate_sources, Error::ProvenanceConflict);

    let duplicate_operations = TableView {
        operations: &[OPERATION_1, OPERATION_1],
        ..base_table()
    };
    assert_error(duplicate_operations, Error::ProvenanceConflict);

    let duplicate_objects = TableView {
        objects: &[OBJECT_RECORD, OBJECT_RECORD],
        ..base_table()
    };
    assert_error(duplicate_objects, Error::ProvenanceConflict);
}

#[test]
fn catalogs_may_be_unsorted_and_preserve_identifier_lookup() {
    let operations = [OPERATION_2, OPERATION_1];
    let bindings = [OBJECT_BINDING_A, SOURCE_BINDING, OBJECT_BINDING_B];
    let binding_refs = [1];
    let table = TableView {
        operations: &operations,
        bindings: &bindings,
        binding_refs: &binding_refs,
        ..base_table()
    };
    assert_valid(table);
}

#[test]
fn mapping_rows_may_be_unsorted_when_coverage_is_complete() {
    let predecessors = [
        PREDECESSOR_0,
        PredecessorRecord {
            entity_index: 1,
            ..PREDECESSOR_0
        },
    ];
    let mappings = [
        MappingRecord {
            output_index: 1,
            predecessors: Range {
                offset: 1,
                count: 1,
            },
            ..BASE_MAPPING
        },
        BASE_MAPPING,
    ];
    let support_tables = [SupportTable {
        counts: EntityCounts {
            vertices: 2,
            ..ZERO_COUNTS
        },
        ..SUPPORT_TABLE
    }];
    let table = TableView {
        predecessors: &predecessors,
        mappings: &mappings,
        support_tables: &support_tables,
        ..base_table()
    };
    assert_valid_with(
        table,
        EntityCounts {
            vertices: 2,
            ..ZERO_COUNTS
        },
        VERTEX_MASK,
        true,
    );
}

#[test]
fn pool_identifiers_and_binding_references_must_resolve() {
    let bad_source_pool = TableView {
        source_ids: &[SourceId(99)],
        ..base_table()
    };
    assert_error(bad_source_pool, Error::ProvenanceConflict);

    let bad_operation_pool = TableView {
        operation_ids: &[OperationId(2), OperationId(99)],
        mappings: &[],
        ..base_table()
    };
    assert_eq!(
        validate_with(bad_operation_pool, ZERO_COUNTS, VERTEX_MASK, 0, true),
        Err(Error::ProvenanceConflict)
    );

    let bad_binding_ref = TableView {
        binding_refs: &[9],
        ..base_table()
    };
    assert_error(bad_binding_ref, Error::ProvenanceConflict);

    let duplicate_binding_ref = TableView {
        sources: &[SourceRecord {
            bindings: Range {
                offset: 0,
                count: 2,
            },
            ..SOURCE_RECORD
        }],
        binding_refs: &[0, 0],
        ..base_table()
    };
    assert_error(duplicate_binding_ref, Error::ProvenanceConflict);
}

#[test]
fn range_offsets_counts_and_slice_bounds_are_checked() {
    let overflowing_offset = TableView {
        sources: &[SourceRecord {
            bindings: Range {
                offset: u64::MAX,
                count: 1,
            },
            ..SOURCE_RECORD
        }],
        ..base_table()
    };
    assert_error(overflowing_offset, Error::ProvenanceConflict);

    let overflowing_count = TableView {
        sources: &[SourceRecord {
            bindings: Range {
                offset: 1,
                count: u64::MAX,
            },
            ..SOURCE_RECORD
        }],
        ..base_table()
    };
    assert_error(overflowing_count, Error::ProvenanceConflict);

    let bad_parent_range = TableView {
        operations: &[
            OPERATION_1,
            OperationRecord {
                parents: Range {
                    offset: 1,
                    count: u64::MAX,
                },
                ..OPERATION_2
            },
        ],
        ..base_table()
    };
    assert_error(bad_parent_range, Error::ProvenanceConflict);

    let bad_source_range = TableView {
        mappings: &[MappingRecord {
            sources: Range {
                offset: 5,
                count: 1,
            },
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_error(bad_source_range, Error::ProvenanceConflict);

    let bad_operation_range = TableView {
        mappings: &[MappingRecord {
            operations: Range {
                offset: 5,
                count: 1,
            },
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_error(bad_operation_range, Error::ProvenanceConflict);

    let bad_predecessor_range = TableView {
        mappings: &[MappingRecord {
            predecessors: Range {
                offset: 5,
                count: 1,
            },
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_error(bad_predecessor_range, Error::ProvenanceConflict);

    let bad_role_range = TableView {
        bindings: &[
            BindingRecord {
                role: Range {
                    offset: 0,
                    count: 5,
                },
                ..SOURCE_BINDING
            },
            OBJECT_BINDING_A,
            OBJECT_BINDING_B,
        ],
        ..base_table()
    };
    assert_error(bad_role_range, Error::ProvenanceConflict);
}

#[test]
fn operation_dag_rejects_self_and_mutual_cycles() {
    let self_cycle = TableView {
        operations: &[
            OperationRecord {
                parents: Range {
                    offset: 0,
                    count: 1,
                },
                ..OPERATION_1
            },
            OPERATION_2,
        ],
        ..base_table()
    };
    assert_error(self_cycle, Error::ProvenanceConflict);

    let mutual_cycle = TableView {
        operations: &[
            OperationRecord {
                parents: Range {
                    offset: 1,
                    count: 1,
                },
                ..OPERATION_1
            },
            OperationRecord {
                parents: Range {
                    offset: 0,
                    count: 1,
                },
                ..OPERATION_2
            },
        ],
        ..base_table()
    };
    assert_error(mutual_cycle, Error::ProvenanceConflict);
}

#[test]
fn long_operation_chains_use_iterative_traversal() {
    const LENGTH: usize = 20_000;
    let length = u64::try_from(LENGTH).unwrap_or(u64::MAX);
    let operation_ids: Vec<OperationId> = (1..=length).map(OperationId).collect();
    let operations: Vec<OperationRecord> = operation_ids
        .iter()
        .enumerate()
        .map(|(index, operation_id)| OperationRecord {
            id: *operation_id,
            parents: if index == 0 {
                Range {
                    offset: 0,
                    count: 0,
                }
            } else {
                Range {
                    offset: u64::try_from(index - 1).unwrap_or(u64::MAX),
                    count: 1,
                }
            },
        })
        .collect();
    let table = TableView {
        operations: &operations,
        operation_ids: &operation_ids,
        ..empty_table()
    };
    assert_valid_with(table, ZERO_COUNTS, VERTEX_MASK, false);

    let mut cyclic = operations;
    if let Some(first) = cyclic.first_mut() {
        first.parents = Range {
            offset: length - 1,
            count: 1,
        };
    }
    let cyclic_table = TableView {
        operations: &cyclic,
        operation_ids: &operation_ids,
        ..empty_table()
    };
    assert_eq!(
        validate_with(cyclic_table, ZERO_COUNTS, VERTEX_MASK, 0, false),
        Err(Error::ProvenanceConflict)
    );
}

#[test]
fn bindings_reject_unknown_kinds_missing_targets_and_bad_guids() {
    let bad_kind = TableView {
        bindings: &[
            BindingRecord {
                logical_kind: 4,
                ..SOURCE_BINDING
            },
            OBJECT_BINDING_A,
            OBJECT_BINDING_B,
        ],
        ..base_table()
    };
    assert_error(bad_kind, Error::ProvenanceConflict);

    let missing_source = TableView {
        bindings: &[
            BindingRecord {
                logical_id: 2,
                ..SOURCE_BINDING
            },
            OBJECT_BINDING_A,
            OBJECT_BINDING_B,
        ],
        ..base_table()
    };
    assert_error(missing_source, Error::ProvenanceConflict);

    let lowercase_guid = TableView {
        bindings: &[
            BindingRecord {
                native_guid: GUID_LOWERCASE,
                ..SOURCE_BINDING
            },
            OBJECT_BINDING_A,
            OBJECT_BINDING_B,
        ],
        ..base_table()
    };
    assert_error(lowercase_guid, Error::ProvenanceConflict);

    let bad_dash_guid = TableView {
        bindings: &[
            BindingRecord {
                native_guid: GUID_BAD_DASH,
                ..SOURCE_BINDING
            },
            OBJECT_BINDING_A,
            OBJECT_BINDING_B,
        ],
        ..base_table()
    };
    assert_error(bad_dash_guid, Error::ProvenanceConflict);

    let nil_guid = TableView {
        bindings: &[
            BindingRecord {
                native_guid: GUID_NIL,
                ..SOURCE_BINDING
            },
            OBJECT_BINDING_A,
            OBJECT_BINDING_B,
        ],
        ..base_table()
    };
    assert_error(nil_guid, Error::ProvenanceConflict);
}

#[test]
fn bindings_validate_metadata_presence_and_role_text() {
    let absent_with_value = TableView {
        bindings: &[
            BindingRecord {
                metadata_uuid: [7; 16],
                ..SOURCE_BINDING
            },
            OBJECT_BINDING_A,
            OBJECT_BINDING_B,
        ],
        ..base_table()
    };
    assert_error(absent_with_value, Error::ProvenanceConflict);

    let present_without_value = TableView {
        bindings: &[
            BindingRecord {
                metadata_present: 1,
                ..SOURCE_BINDING
            },
            OBJECT_BINDING_A,
            OBJECT_BINDING_B,
        ],
        ..base_table()
    };
    assert_error(present_without_value, Error::ProvenanceConflict);

    let unknown_presence = TableView {
        bindings: &[
            BindingRecord {
                metadata_present: 2,
                ..SOURCE_BINDING
            },
            OBJECT_BINDING_A,
            OBJECT_BINDING_B,
        ],
        ..base_table()
    };
    assert_error(unknown_presence, Error::ProvenanceConflict);

    let present_with_value = TableView {
        bindings: &[
            BindingRecord {
                metadata_present: 1,
                metadata_uuid: [7; 16],
                ..SOURCE_BINDING
            },
            OBJECT_BINDING_A,
            OBJECT_BINDING_B,
        ],
        ..base_table()
    };
    assert_valid(present_with_value);

    let empty_role = TableView {
        role_bytes: &[],
        bindings: &[
            BindingRecord {
                role: Range {
                    offset: 0,
                    count: 0,
                },
                ..SOURCE_BINDING
            },
            BindingRecord {
                role: Range {
                    offset: 0,
                    count: 0,
                },
                ..OBJECT_BINDING_A
            },
            BindingRecord {
                role: Range {
                    offset: 0,
                    count: 0,
                },
                ..OBJECT_BINDING_B
            },
        ],
        ..base_table()
    };
    assert_error(empty_role, Error::ProvenanceConflict);

    let invalid_utf8 = TableView {
        role_bytes: &[0xFF],
        bindings: &[
            BindingRecord {
                role: Range {
                    offset: 0,
                    count: 1,
                },
                ..SOURCE_BINDING
            },
            BindingRecord {
                role: Range {
                    offset: 0,
                    count: 1,
                },
                ..OBJECT_BINDING_A
            },
            BindingRecord {
                role: Range {
                    offset: 0,
                    count: 1,
                },
                ..OBJECT_BINDING_B
            },
        ],
        ..base_table()
    };
    assert_error(invalid_utf8, Error::ProvenanceConflict);

    let nul_role = TableView {
        role_bytes: &[0],
        bindings: &[
            BindingRecord {
                role: Range {
                    offset: 0,
                    count: 1,
                },
                ..SOURCE_BINDING
            },
            BindingRecord {
                role: Range {
                    offset: 0,
                    count: 1,
                },
                ..OBJECT_BINDING_A
            },
            BindingRecord {
                role: Range {
                    offset: 0,
                    count: 1,
                },
                ..OBJECT_BINDING_B
            },
        ],
        ..base_table()
    };
    assert_error(nul_role, Error::ProvenanceConflict);
}

#[test]
fn binding_revision_cannot_be_newer_than_the_current_revision() {
    let future_binding = TableView {
        bindings: &[
            BindingRecord {
                revision: 1,
                ..SOURCE_BINDING
            },
            OBJECT_BINDING_A,
            OBJECT_BINDING_B,
        ],
        ..base_table()
    };
    assert_error(future_binding, Error::ProvenanceStale);
}

#[test]
fn duplicate_binding_identity_is_refused_but_distinct_guids_remain_valid() {
    let duplicate = TableView {
        bindings: &[
            SOURCE_BINDING,
            SOURCE_BINDING,
            OBJECT_BINDING_A,
            OBJECT_BINDING_B,
        ],
        ..base_table()
    };
    assert_error(duplicate, Error::ProvenanceConflict);
    assert_valid(base_table());
}

#[test]
fn shared_native_guid_across_objects_is_not_a_global_conflict() {
    let objects = [
        OBJECT_RECORD,
        ObjectRecord {
            id: ObjectId(5),
            operation: OperationId(3),
        },
    ];
    let bindings = [
        SOURCE_BINDING,
        OBJECT_BINDING_A,
        OBJECT_BINDING_B,
        BindingRecord {
            logical_id: 5,
            ..OBJECT_BINDING_A
        },
    ];
    let table = TableView {
        objects: &objects,
        bindings: &bindings,
        ..base_table()
    };
    assert_valid(table);
}

#[test]
fn unknown_discriminators_are_rejected_without_invalid_rust_values() {
    let unknown_support_flag = TableView {
        support_tables: &[SupportTable {
            historical: 2,
            ..SUPPORT_TABLE
        }],
        ..base_table()
    };
    assert_error(unknown_support_flag, Error::ProvenanceConflict);

    let unknown_predecessor_domain = TableView {
        predecessors: &[PredecessorRecord {
            domain: 0,
            ..PREDECESSOR_0
        }],
        ..base_table()
    };
    assert_error(unknown_predecessor_domain, Error::ProvenanceConflict);

    let unknown_mapping_domain = TableView {
        mappings: &[MappingRecord {
            domain: 5,
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_error(unknown_mapping_domain, Error::ProvenanceConflict);

    let unknown_mapping_kind = TableView {
        mappings: &[MappingRecord {
            kind: 6,
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_error(unknown_mapping_kind, Error::ProvenanceConflict);

    let unknown_domain_mask = TableView {
        mappings: &[],
        ..base_table()
    };
    assert_eq!(
        validate_with(unknown_domain_mask, ZERO_COUNTS, 1 << 4, 0, true),
        Err(Error::ProvenanceConflict)
    );
}

#[test]
fn counts_for_undeclared_domains_must_be_zero() {
    let undeclared_counts = TableView {
        mappings: &[],
        ..base_table()
    };
    assert_eq!(
        validate_with(
            undeclared_counts,
            EntityCounts {
                faces: 1,
                ..ZERO_COUNTS
            },
            VERTEX_MASK,
            0,
            true,
        ),
        Err(Error::ProvenanceConflict)
    );
}

#[test]
fn source_binding_lists_must_match_the_source_kind_and_identifier() {
    let wrong_binding_kind = TableView {
        sources: &[SourceRecord {
            bindings: Range {
                offset: 0,
                count: 1,
            },
            ..SOURCE_RECORD
        }],
        bindings: &[OBJECT_BINDING_A],
        ..base_table()
    };
    assert_error(wrong_binding_kind, Error::ProvenanceConflict);
}

#[test]
fn support_tables_validate_revision_session_and_identity() {
    let current_mismatch = TableView {
        support_tables: &[SupportTable {
            context: Context {
                revision: 1,
                ..SUPPORT_CONTEXT
            },
            ..SUPPORT_TABLE
        }],
        ..base_table()
    };
    assert_error(current_mismatch, Error::ProvenanceStale);

    let historical_current = TableView {
        support_tables: &[SupportTable {
            historical: 1,
            ..SUPPORT_TABLE
        }],
        ..base_table()
    };
    assert_valid(historical_current);

    let historical_future = TableView {
        support_tables: &[SupportTable {
            historical: 1,
            context: Context {
                revision: 1,
                ..SUPPORT_CONTEXT
            },
            ..SUPPORT_TABLE
        }],
        ..base_table()
    };
    assert_error(historical_future, Error::ProvenanceStale);

    let foreign_session = TableView {
        support_tables: &[SupportTable {
            context: Context {
                session_uuid: [9; 16],
                ..SUPPORT_CONTEXT
            },
            ..SUPPORT_TABLE
        }],
        ..base_table()
    };
    assert_error(foreign_session, Error::ProvenanceConflict);

    let nil_session = TableView {
        support_tables: &[SupportTable {
            context: Context {
                session_uuid: [0; 16],
                ..SUPPORT_CONTEXT
            },
            ..SUPPORT_TABLE
        }],
        ..base_table()
    };
    assert_error(nil_session, Error::ProvenanceConflict);

    let wrong_schema = TableView {
        support_tables: &[SupportTable {
            context: Context {
                schema_version: 2,
                ..SUPPORT_CONTEXT
            },
            ..SUPPORT_TABLE
        }],
        ..base_table()
    };
    assert_error(wrong_schema, Error::ProvenanceConflict);

    let nil_table = TableView {
        support_tables: &[SupportTable {
            context: Context {
                table_id: 0,
                ..SUPPORT_CONTEXT
            },
            ..SUPPORT_TABLE
        }],
        ..base_table()
    };
    assert_error(nil_table, Error::ProvenanceConflict);

    let current_table_id = TableView {
        support_tables: &[SupportTable {
            context: Context {
                table_id: CONTEXT.table_id,
                ..SUPPORT_CONTEXT
            },
            ..SUPPORT_TABLE
        }],
        ..base_table()
    };
    assert_error(current_table_id, Error::ProvenanceConflict);

    let duplicate_support = TableView {
        support_tables: &[SUPPORT_TABLE, SUPPORT_TABLE],
        ..base_table()
    };
    assert_error(duplicate_support, Error::ProvenanceConflict);

    let orphan_invalid_support = TableView {
        mappings: &[],
        support_tables: &[SupportTable {
            historical: 2,
            ..SUPPORT_TABLE
        }],
        ..base_table()
    };
    assert_eq!(
        validate_with(orphan_invalid_support, ZERO_COUNTS, VERTEX_MASK, 0, true),
        Err(Error::ProvenanceConflict)
    );
}

#[test]
fn predecessor_locators_must_resolve_and_are_validated_even_when_unused() {
    let missing_table = TableView {
        predecessors: &[PredecessorRecord {
            table_id: 99,
            ..PREDECESSOR_0
        }],
        ..base_table()
    };
    assert_error(missing_table, Error::ProvenanceConflict);

    let unknown_domain = TableView {
        predecessors: &[PredecessorRecord {
            domain: 5,
            ..PREDECESSOR_0
        }],
        ..base_table()
    };
    assert_error(unknown_domain, Error::ProvenanceConflict);

    let out_of_bounds_index = TableView {
        predecessors: &[PredecessorRecord {
            entity_index: 1,
            ..PREDECESSOR_0
        }],
        ..base_table()
    };
    assert_error(out_of_bounds_index, Error::ProvenanceConflict);

    let unused_invalid = TableView {
        mappings: &[],
        predecessors: &[PredecessorRecord {
            entity_index: 1,
            ..PREDECESSOR_0
        }],
        ..base_table()
    };
    assert_eq!(
        validate_with(unused_invalid, ZERO_COUNTS, VERTEX_MASK, 0, true),
        Err(Error::ProvenanceConflict)
    );
}

#[test]
fn declared_domains_require_exactly_one_mapping_row_per_entity() {
    let missing_row = base_table();
    assert_eq!(
        validate_with(
            missing_row,
            EntityCounts {
                vertices: 2,
                ..ZERO_COUNTS
            },
            VERTEX_MASK,
            0,
            true,
        ),
        Err(Error::ProvenanceUnattributable)
    );

    let no_rows = TableView {
        mappings: &[],
        ..base_table()
    };
    assert_error(no_rows, Error::ProvenanceUnattributable);

    let out_of_range_index = TableView {
        mappings: &[MappingRecord {
            output_index: 1,
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_error(out_of_range_index, Error::ProvenanceConflict);

    let duplicate_index = TableView {
        mappings: &[BASE_MAPPING, BASE_MAPPING],
        ..base_table()
    };
    assert_error(duplicate_index, Error::ProvenanceConflict);

    let coverage_gap = TableView {
        mappings: &[
            BASE_MAPPING,
            MappingRecord {
                output_index: 2,
                ..BASE_MAPPING
            },
        ],
        ..base_table()
    };
    assert_eq!(
        validate_with(
            coverage_gap,
            EntityCounts {
                vertices: 3,
                ..ZERO_COUNTS
            },
            VERTEX_MASK,
            0,
            true,
        ),
        Err(Error::ProvenanceUnattributable)
    );
}

#[test]
fn mapping_contributors_must_be_nonempty_sorted_and_resolve() {
    let empty_sources = TableView {
        mappings: &[MappingRecord {
            sources: Range {
                offset: 0,
                count: 0,
            },
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_error(empty_sources, Error::ProvenanceUnattributable);

    let empty_operations = TableView {
        mappings: &[MappingRecord {
            operations: Range {
                offset: 0,
                count: 0,
            },
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_error(empty_operations, Error::ProvenanceUnattributable);

    let duplicate_sources = TableView {
        source_ids: &[SourceId(1), SourceId(1)],
        mappings: &[MappingRecord {
            sources: Range {
                offset: 0,
                count: 2,
            },
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_error(duplicate_sources, Error::ProvenanceConflict);

    let source_two = SourceRecord {
        id: SourceId(5),
        bindings: Range {
            offset: 0,
            count: 0,
        },
    };
    let unsorted_sources = TableView {
        sources: &[SOURCE_RECORD, source_two],
        source_ids: &[SourceId(5), SourceId(1)],
        mappings: &[MappingRecord {
            sources: Range {
                offset: 0,
                count: 2,
            },
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_error(unsorted_sources, Error::ProvenanceConflict);

    let operations = [
        OPERATION_1,
        OperationRecord {
            parents: Range {
                offset: 0,
                count: 0,
            },
            ..OPERATION_2
        },
    ];
    let unsorted_operations = TableView {
        operations: &operations,
        operation_ids: &[OperationId(3), OperationId(2)],
        mappings: &[MappingRecord {
            operations: Range {
                offset: 0,
                count: 2,
            },
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_error(unsorted_operations, Error::ProvenanceConflict);
}

#[test]
fn object_requirement_and_owner_reachability_are_enforced() {
    let absent_object = TableView {
        mappings: &[MappingRecord {
            object: ObjectId(0),
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_error(absent_object, Error::ProvenanceUnattributable);

    assert_eq!(
        validate_with(absent_object, BASE_COUNTS, VERTEX_MASK, 0, false),
        Ok(())
    );

    let missing_object = TableView {
        mappings: &[MappingRecord {
            object: ObjectId(5),
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_eq!(
        validate_with(missing_object, BASE_COUNTS, VERTEX_MASK, 0, false),
        Err(Error::ProvenanceConflict)
    );

    let unreachable_owner = TableView {
        mappings: &[MappingRecord {
            operations: Range {
                offset: 0,
                count: 1,
            },
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_error(unreachable_owner, Error::ProvenanceUnattributable);
}

#[test]
fn mapping_kinds_enforce_predecessor_cardinality_and_domains() {
    let exact_without_parent = TableView {
        mappings: &[MappingRecord {
            predecessors: Range {
                offset: 0,
                count: 0,
            },
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_error(exact_without_parent, Error::ProvenanceUnattributable);

    let two_predecessors = [
        PREDECESSOR_0,
        PredecessorRecord {
            entity_index: 1,
            ..PREDECESSOR_0
        },
    ];
    let exact_with_two = TableView {
        predecessors: &two_predecessors,
        support_tables: &[SupportTable {
            counts: EntityCounts {
                vertices: 2,
                ..ZERO_COUNTS
            },
            ..SUPPORT_TABLE
        }],
        mappings: &[MappingRecord {
            predecessors: Range {
                offset: 0,
                count: 2,
            },
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_error(exact_with_two, Error::ProvenanceConflict);

    let wrong_domain = TableView {
        predecessors: &[PredecessorRecord {
            domain: DOMAIN_EDGE,
            ..PREDECESSOR_0
        }],
        support_tables: &[SupportTable {
            counts: EntityCounts {
                edges: 1,
                ..ZERO_COUNTS
            },
            ..SUPPORT_TABLE
        }],
        ..base_table()
    };
    assert_error(wrong_domain, Error::ProvenanceConflict);

    let inherited_without_parent = TableView {
        mappings: &[MappingRecord {
            kind: KIND_INHERITED,
            predecessors: Range {
                offset: 0,
                count: 0,
            },
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_error(inherited_without_parent, Error::ProvenanceUnattributable);

    let inherited = TableView {
        mappings: &[MappingRecord {
            kind: KIND_INHERITED,
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_valid(inherited);

    let split = TableView {
        mappings: &[MappingRecord {
            kind: KIND_SPLIT,
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_valid(split);

    let merged_with_one = TableView {
        mappings: &[MappingRecord {
            kind: KIND_MERGED,
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_error(merged_with_one, Error::ProvenanceUnattributable);

    let merged = TableView {
        predecessors: &two_predecessors,
        support_tables: &[SupportTable {
            counts: EntityCounts {
                vertices: 2,
                ..ZERO_COUNTS
            },
            ..SUPPORT_TABLE
        }],
        mappings: &[MappingRecord {
            kind: KIND_MERGED,
            predecessors: Range {
                offset: 0,
                count: 2,
            },
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_valid(merged);

    let generated_without_parent = TableView {
        mappings: &[MappingRecord {
            kind: KIND_GENERATED,
            predecessors: Range {
                offset: 0,
                count: 0,
            },
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_valid(generated_without_parent);

    let generated_with_parent = TableView {
        mappings: &[MappingRecord {
            kind: KIND_GENERATED,
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_valid(generated_with_parent);

    let unknown_kind = TableView {
        mappings: &[MappingRecord {
            kind: 0,
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_error(unknown_kind, Error::ProvenanceConflict);
}

#[test]
fn duplicate_predecessor_locators_are_refused_for_every_kind() {
    let duplicate = TableView {
        predecessors: &[PREDECESSOR_0, PREDECESSOR_0],
        mappings: &[MappingRecord {
            kind: KIND_GENERATED,
            predecessors: Range {
                offset: 0,
                count: 2,
            },
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_error(duplicate, Error::ProvenanceConflict);
}

#[test]
fn mapping_domains_must_be_declared_and_patch_members_require_an_owner() {
    let undeclared_mapping = TableView {
        mappings: &[MappingRecord {
            domain: DOMAIN_FACE,
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_error(undeclared_mapping, Error::ProvenanceConflict);

    let patch_key_on_vertex = TableView {
        mappings: &[MappingRecord {
            patch_key: 7,
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_eq!(
        validate_with(patch_key_on_vertex, BASE_COUNTS, VERTEX_MASK, 0, true),
        Ok(())
    );

    let unowned_patch_member = TableView {
        mappings: &[MappingRecord {
            object: ObjectId(0),
            patch_key: 7,
            ..BASE_MAPPING
        }],
        ..base_table()
    };
    assert_eq!(
        validate_with(unowned_patch_member, BASE_COUNTS, VERTEX_MASK, 0, false),
        Err(Error::ProvenanceConflict)
    );

    let empty_mask_mapping = base_table();
    assert_eq!(
        validate_with(empty_mask_mapping, BASE_COUNTS, 0, 0, true),
        Err(Error::ProvenanceConflict)
    );
}

#[test]
fn patch_rows_require_an_object_and_nonzero_token() {
    let patch_mapping = MappingRecord {
        domain: DOMAIN_PATCH,
        kind: KIND_GENERATED,
        sources: Range {
            offset: 0,
            count: 1,
        },
        operations: Range {
            offset: 1,
            count: 1,
        },
        object: ObjectId(4),
        predecessors: Range {
            offset: 0,
            count: 0,
        },
        patch_key: 5,
        ..BASE_MAPPING
    };
    let patch_counts = EntityCounts {
        patches: 1,
        ..ZERO_COUNTS
    };
    assert_valid_with(
        TableView {
            mappings: &[patch_mapping],
            ..base_table()
        },
        patch_counts,
        PATCH_MASK,
        true,
    );

    let missing_object = TableView {
        mappings: &[MappingRecord {
            object: ObjectId(0),
            ..patch_mapping
        }],
        ..base_table()
    };
    assert_eq!(
        validate_with(missing_object, patch_counts, PATCH_MASK, 0, false),
        Err(Error::ProvenanceUnattributable)
    );

    let missing_token = TableView {
        mappings: &[MappingRecord {
            patch_key: 0,
            ..patch_mapping
        }],
        ..base_table()
    };
    assert_eq!(
        validate_with(missing_token, patch_counts, PATCH_MASK, 0, true),
        Err(Error::ProvenanceConflict)
    );
}

#[test]
fn patch_tokens_are_unique_per_object() {
    let patch_mapping = MappingRecord {
        domain: DOMAIN_PATCH,
        kind: KIND_GENERATED,
        sources: Range {
            offset: 0,
            count: 1,
        },
        operations: Range {
            offset: 1,
            count: 1,
        },
        object: ObjectId(4),
        predecessors: Range {
            offset: 0,
            count: 0,
        },
        patch_key: 9,
        ..BASE_MAPPING
    };
    let duplicate = TableView {
        mappings: &[
            patch_mapping,
            MappingRecord {
                output_index: 1,
                ..patch_mapping
            },
        ],
        ..base_table()
    };
    assert_eq!(
        validate_with(
            duplicate,
            EntityCounts {
                patches: 2,
                ..ZERO_COUNTS
            },
            PATCH_MASK,
            0,
            true,
        ),
        Err(Error::PatchKeyUnstable)
    );
}

#[test]
fn patch_tokens_are_scoped_to_the_owning_object() {
    let objects = [
        OBJECT_RECORD,
        ObjectRecord {
            id: ObjectId(5),
            operation: OperationId(3),
        },
    ];
    let bindings = [
        SOURCE_BINDING,
        OBJECT_BINDING_A,
        OBJECT_BINDING_B,
        BindingRecord {
            logical_id: 5,
            ..OBJECT_BINDING_A
        },
    ];
    let patch_mapping = MappingRecord {
        domain: DOMAIN_PATCH,
        kind: KIND_GENERATED,
        sources: Range {
            offset: 0,
            count: 1,
        },
        operations: Range {
            offset: 1,
            count: 1,
        },
        object: ObjectId(4),
        predecessors: Range {
            offset: 0,
            count: 0,
        },
        patch_key: 9,
        ..BASE_MAPPING
    };
    let table = TableView {
        objects: &objects,
        bindings: &bindings,
        mappings: &[
            patch_mapping,
            MappingRecord {
                output_index: 1,
                object: ObjectId(5),
                ..patch_mapping
            },
        ],
        ..base_table()
    };
    assert_eq!(
        validate_with(
            table,
            EntityCounts {
                patches: 2,
                ..ZERO_COUNTS
            },
            PATCH_MASK,
            0,
            true,
        ),
        Ok(())
    );
}

#[test]
fn native_source_guid_absence_is_valid() {
    let sources = [SourceRecord {
        id: SourceId(1),
        bindings: Range {
            offset: 0,
            count: 0,
        },
    }];
    let bindings = [OBJECT_BINDING_A, OBJECT_BINDING_B];
    let table = TableView {
        sources: &sources,
        bindings: &bindings,
        binding_refs: &[],
        ..base_table()
    };
    assert_valid(table);
}
