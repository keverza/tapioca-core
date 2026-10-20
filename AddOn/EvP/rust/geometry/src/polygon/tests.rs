use super::*;
use crate::identity as id;

const SESSION: [u8; 16] = [1; 16];
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

fn rows(count: usize) -> Vec<id::MappingRecord> {
    [
        (id::DOMAIN_VERTEX, count),
        (id::DOMAIN_EDGE, count),
        (id::DOMAIN_FACE, usize::from(count != 0)),
    ]
    .into_iter()
    .flat_map(|(domain, count)| {
        (0..count).map(move |index| id::MappingRecord {
            domain,
            kind: id::KIND_GENERATED,
            output_index: index as u64,
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
fn request(count: u64, rows: &[id::MappingRecord]) -> id::ValidationRequest<'_> {
    id::ValidationRequest {
        expected_session: SESSION,
        expected_revision: 0,
        counts: id::EntityCounts {
            vertices: count,
            edges: count,
            faces: u64::from(count != 0),
            patches: 0,
        },
        domains: 7,
        require_object: true,
        table: id::TableView {
            context: id::Context {
                schema_version: 1,
                session_uuid: SESSION,
                revision: 0,
                table_id: 100,
            },
            sources: &SOURCES,
            operations: &OPERATIONS,
            objects: &OBJECTS,
            bindings: &[],
            binding_refs: &[],
            source_ids: &SOURCE_IDS,
            operation_ids: &OPERATION_IDS,
            predecessors: &[],
            mappings: rows,
            role_bytes: &[],
            support_tables: &[],
        },
    }
}
fn run(points: &[f64], request: &id::ValidationRequest<'_>) -> Result<Vec<f64>, Error> {
    copy_attributed_polygon(&PolygonView {
        points,
        provenance: request,
    })
}

#[test]
fn exact_survey_bits_winding_duplicates_and_explicit_closure_survive() -> Result<(), Error> {
    for points in [
        vec![
            -0.0,
            6_000_000.0,
            120.0,
            700_002.0,
            6_000_000.0,
            120.0,
            700_002.0,
            6_000_003.0,
            120.0,
        ],
        vec![0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, -0.0, 0.0, 0.0],
        vec![1.0; 9],
    ] {
        let count = points.len() / 3;
        let mappings = rows(count);
        let request = request(count as u64, &mappings);
        let output = run(&points, &request)?;
        assert_eq!(
            output.iter().map(|v| v.to_bits()).collect::<Vec<_>>(),
            points.iter().map(|v| v.to_bits()).collect::<Vec<_>>()
        );
        assert_eq!(output, run(&points, &request)?);
    }
    Ok(())
}

#[test]
fn empty_value_has_no_face_or_implicit_edge() -> Result<(), Error> {
    assert_eq!(run(&[], &request(0, &[]))?, [] as [f64; 0]);
    Ok(())
}

#[test]
fn carrier_does_not_pretend_to_validate_geometry() -> Result<(), Error> {
    for points in [
        vec![f64::MAX; 9], // No arithmetic: finite coordinates copy without overflow.
        vec![0.0; 9],
        vec![0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 2.0, 0.0, 0.0],
        vec![0.0, 0.0, 0.0, 2.0, 2.0, 0.0, 0.0, 2.0, 0.0, 2.0, 0.0, 1.0],
    ] {
        let count = points.len() / 3;
        let mappings = rows(count);
        assert_eq!(run(&points, &request(count as u64, &mappings))?, points);
    }
    Ok(())
}

#[test]
fn malformed_and_nonfinite_coordinates_refuse() {
    for count in [1, 3, 6, 8, 10] {
        assert_eq!(run(&vec![0.0; count], &request(0, &[])), Err(Error::Shape));
    }
    let mappings = rows(3);
    let request = request(3, &mappings);
    for bad in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY] {
        for index in 0..9 {
            let mut points = [0.0; 9];
            if let Some(value) = points.get_mut(index) {
                *value = bad;
            }
            assert_eq!(run(&points, &request), Err(Error::NonFinite));
        }
    }
}

#[test]
fn incomplete_or_wrong_domain_attribution_refuses() {
    let mappings = rows(3);
    for kind in 0..6 {
        let mut request = request(3, &mappings);
        match kind {
            0 => request.domains = 5,
            1 => request.counts.vertices = 4,
            2 => request.counts.edges = 0,
            3 => request.counts.faces = 0,
            4 => request.counts.patches = 1,
            _ => request.table.mappings = &[],
        }
        assert_eq!(
            run(&[0.0; 9], &request),
            Err(Error::Provenance(id::Error::ProvenanceUnattributable))
        );
    }
}

#[test]
fn stale_and_conflicting_tables_are_not_carried_as_success() {
    let mappings = rows(3);
    let mut request = request(3, &mappings);
    request.expected_revision = 1;
    assert_eq!(
        run(&[0.0; 9], &request),
        Err(Error::Provenance(id::Error::ProvenanceStale))
    );
    request.expected_revision = 0;
    request.table.objects = &[];
    assert!(matches!(
        run(&[0.0; 9], &request),
        Err(Error::Provenance(_))
    ));
}
