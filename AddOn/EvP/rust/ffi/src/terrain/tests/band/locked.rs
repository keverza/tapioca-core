use super::*;
use tapioca_geometry::terrain::hybrid::Locked;

fn locked_request(
    band: &TerrainHybridRequest,
    geometry: &[f64],
    lines: &[TerrainRing],
    points: &[f64],
) -> TerrainHybridLockedRequest {
    TerrainHybridLockedRequest {
        band: *band,
        line_points: view(geometry),
        lines: view(lines),
        points: view(points),
    }
}

fn counts(
    points: &[f64],
    triangles: &[u32],
    geometry: &[f64],
    controls: &[f64],
    edges: u64,
) -> id::EntityCounts {
    let mut counts = counts_in(points, triangles);
    counts.vertices += u64::try_from((geometry.len() + controls.len()) / 3).unwrap_or(u64::MAX);
    counts.edges += edges;
    counts
}

fn run_locked(input: &TerrainHybridLockedRequest, output: &TerrainHybridOutput) -> (i32, String) {
    let mut error = [0_u8; 256];
    // SAFETY: live fixture descriptors, immutable inputs and disjoint output storage.
    let status = unsafe {
        tapioca_terrain_hybrid_locked_build(input, output, error.as_mut_ptr(), error.len())
    };
    let length = error.iter().position(|&b| b == 0).unwrap_or(error.len());
    (
        status,
        String::from_utf8_lossy(error.get(..length).unwrap_or(&[])).into_owned(),
    )
}

#[test]
fn layouts_and_query_fill_retain_every_control_contributor() {
    assert_eq!(size_of::<TerrainHybridDiagnostic>(), 80);
    if size_of::<usize>() == 8 {
        assert_eq!(size_of::<TerrainHybridLockedRequest>(), 408);
        assert_eq!(
            std::mem::offset_of!(TerrainHybridLockedRequest, line_points),
            360
        );
        assert_eq!(size_of::<TerrainHybridDiagnosticOutput>(), 32);
    }
    let (points, triangles) = base();
    let stations = [station(0.0, 10.0), station(40.0, 12.0)];
    let geometry = [
        5.0, -1.0, 10.25, 35.0, 1.0, 11.75, 5.0, 1.0, 10.25, 35.0, -1.0, 11.75,
    ];
    let lines = [
        TerrainRing {
            offset: 0,
            count: 6,
        },
        TerrainRing {
            offset: 6,
            count: 6,
        },
    ];
    let controls = [20.0, 0.0, 11.0, 20.0, 0.0, 11.0];
    let counts = counts(&points, &triangles, &geometry, &controls, 2);
    let rows = rows(counts);
    let mut band = request(&points, &triangles, &stations, &rows);
    band.provenance = provenance(&rows, counts, 7);
    let input = locked_request(&band, &geometry, &lines, &controls);
    let mut storage = Storage::new();
    assert_eq!(
        run_locked(&input, &storage.output(true)),
        (0, String::new())
    );
    storage.size(stations.len());
    assert_eq!(
        run_locked(&input, &storage.output(false)),
        (0, String::new())
    );
    let kernel_stations: Vec<_> = stations.iter().map(hybrid_station).collect();
    let built = band_kernel::build_locked(
        &band_kernel::BuildRequest {
            points: &points,
            triangles: &triangles,
            region: &REGION,
            band: Band {
                stations: &kernel_stations,
                effect: Effect::Both,
            },
            tolerance: 1e-6,
        },
        Locked {
            lines: &[&geometry[..6], &geometry[6..]],
            points: &controls,
        },
    );
    assert!(built.is_ok());
    let Ok(built) = built else { return };
    let encoded = hybrid::encode_build(built, true);
    assert!(encoded.is_ok());
    let Ok(encoded) = encoded else { return };
    assert_eq!(storage.points, encoded.points);
    assert_eq!(storage.base, encoded.base);
    assert_eq!(storage.owners, encoded.owners);
    assert_eq!(storage.triangles, encoded.triangles);
    assert_eq!(storage.mappings, encoded.mappings);
    assert_eq!(storage.lines, encoded.lines);
    assert_eq!(storage.site, encoded.site);
    let remaps: Vec<_> = storage.mappings.iter().filter(|r| r.kind == 21).collect();
    assert_eq!(remaps.len(), 2);
    assert_eq!(
        remaps.first().map(|r| r.values[0]),
        remaps.get(1).map(|r| r.values[0])
    );
    assert!(storage.mappings.iter().filter(|r| r.kind == 22).count() > 8);
    assert_eq!(storage.lines.iter().filter(|r| r.kind == 6).count(), 2);
}

#[test]
fn provenance_geometry_capacity_and_grading_refusals_are_atomic() {
    let (points, triangles) = base();
    let stations = [station(0.0, 10.0), station(40.0, 12.0)];
    let controls = [20.0, 0.0, 11.0];
    let counts = counts(&points, &triangles, &[], &controls, 0);
    let rows = rows(counts);
    let mut band = request(&points, &triangles, &stations, &rows);
    band.provenance = provenance(&rows, counts, 7);
    let input = locked_request(&band, &[], &[], &controls);
    let mut storage = Storage::new();
    assert_eq!(run_locked(&input, &storage.output(true)).0, 0);
    storage.size(stations.len());
    let before = storage.clone();
    let mut output = storage.output(false);
    output.mapping_capacity -= 1;
    assert_ne!(run_locked(&input, &output).0, 0);
    assert_eq!(storage, before);
    let mismatched = [20.0, 0.0, 12.0];
    let nonfinite = [20.0, 0.0, f64::NAN];
    let unused = [20.0, 0.0, 11.0];
    for mutation in 0..5 {
        let mut changed = input;
        match mutation {
            0 => changed.band.provenance.expected_revision += 1,
            1 => changed.band.provenance.counts.vertices -= 1,
            2 => changed.points = view(&mismatched),
            3 => changed.points = view(&nonfinite),
            _ => changed.line_points = view(&unused),
        }
        let error = run_locked(&changed, &storage.output(false));
        assert_ne!(error.0, 0, "{mutation}");
        if mutation == 2 {
            assert!(error.1.starts_with("LockedConflict:"));
        }
        assert_eq!(storage, before);
    }
}

#[test]
#[expect(
    clippy::cast_ptr_alignment,
    reason = "foreign output pointers are deliberately malformed/aliased; preflight must refuse before access"
)]
fn every_new_pool_and_unused_capacity_protects_inputs_and_diagnostics() {
    let (points, triangles) = base();
    let stations = [station(0.0, 10.0), station(40.0, 12.0)];
    let controls = [20.0, 0.0, 11.0];
    let geometry = [5.0, 0.0, 10.25, 35.0, 0.0, 11.75];
    let lines = [TerrainRing {
        offset: 0,
        count: 6,
    }];
    let counts = counts(&points, &triangles, &geometry, &controls, 1);
    let rows = rows(counts);
    let mut band = request(&points, &triangles, &stations, &rows);
    band.provenance = provenance(&rows, counts, 7);
    let input = locked_request(&band, &geometry, &lines, &controls);
    for address in [
        input.line_points.data.cast::<u8>(),
        input.lines.data.cast::<u8>(),
        input.points.data.cast::<u8>(),
        input.band.provenance.table.mappings.data.cast::<u8>(),
    ] {
        let mut storage = Storage::new();
        let before = storage.clone();
        let mut output = storage.output(false);
        output.points = address.cast::<f64>().cast_mut();
        output.point_capacity = 1;
        // Overlap wins even with a malformed peer, preserving all storage.
        output.base = std::ptr::null_mut();
        output.base_capacity = 1;
        assert_ne!(run_locked(&input, &output).0, 0);
        assert_eq!(storage, before);
        // SAFETY: the address is an input pool; overlap must refuse without writing.
        let status = unsafe {
            tapioca_terrain_hybrid_locked_build(
                &raw const input,
                &storage.output(true),
                address.cast_mut(),
                1,
            )
        };
        assert_ne!(status, 0);
        assert_eq!(storage, before);
    }
}

#[test]
fn a_refused_fixed_control_assesses_with_its_provenance_locators() {
    let (points, triangles) = base();
    let stations = [station(0.0, 10.0), station(40.0, 12.0)];
    let controls = [20.0, 0.0, 12.0];
    let counts = counts(&points, &triangles, &[], &controls, 0);
    let rows = rows(counts);
    let mut band = request(&points, &triangles, &stations, &rows);
    band.provenance = provenance(&rows, counts, 7);
    let input = locked_request(&band, &[], &[], &controls);
    let mut count = 99;
    let mut row = TerrainHybridDiagnostic::default();
    let mut error = [0_u8; 256];
    let query = TerrainHybridDiagnosticOutput {
        count: &raw mut count,
        rows: std::ptr::null_mut(),
        capacity: 0,
        query_only: 1,
        reserved: 0,
    };
    // SAFETY: all fixture pools and caller-owned outputs are live/disjoint.
    assert_eq!(
        // SAFETY: live disjoint fixture pools and output storage.
        unsafe {
            tapioca_terrain_hybrid_locked_assess(
                &raw const input,
                &raw const query,
                error.as_mut_ptr(),
                error.len(),
            )
        },
        0
    );
    assert_eq!(count, 1);
    let fill = TerrainHybridDiagnosticOutput {
        count: &raw mut count,
        rows: &raw mut row,
        capacity: 1,
        query_only: 0,
        reserved: 0,
    };
    // SAFETY: same live/disjoint storage, fully sized by query.
    assert_eq!(
        // SAFETY: storage was fully sized by the preceding query.
        unsafe {
            tapioca_terrain_hybrid_locked_assess(
                &raw const input,
                &raw const fill,
                error.as_mut_ptr(),
                error.len(),
            )
        },
        0
    );
    assert_eq!(
        (row.code, row.station, row.side, row.hard),
        (6, u32::MAX, 0, 1)
    );
    assert_eq!(
        (row.control_kind, row.control_index, row.other_kind),
        (2, 0, 0)
    );
    assert_eq!(row.position, controls);
    assert!((row.magnitude + 1.0).abs() < 1e-12);
    let before = row;
    count = 99;
    let small = TerrainHybridDiagnosticOutput {
        capacity: 0,
        ..fill
    };
    // SAFETY: zero capacity fails without writing the row or count.
    assert_ne!(
        // SAFETY: zero capacity must refuse before any output write.
        unsafe {
            tapioca_terrain_hybrid_locked_assess(
                &raw const input,
                &raw const small,
                error.as_mut_ptr(),
                error.len(),
            )
        },
        0
    );
    assert_eq!(count, 99);
    assert_eq!(row, before);
}
