//! S4.2 hybrid band export against the safe kernel on the Appendix A fixture.
use super::*;
use tapioca_geometry::terrain::design::{Band, Station};
use tapioca_geometry::terrain::edit::Effect;
use tapioca_geometry::terrain::hybrid as band_kernel;

mod locked;

const REGION: [f64; 12] = [
    -10.0, -15.0, 0.0, 50.0, -15.0, 0.0, 50.0, 15.0, 0.0, -10.0, 15.0, 0.0,
];
const HALF: TerrainSlope = TerrainSlope {
    a: 1.0,
    b: 2.0,
    kind: 3,
    reserved: 0,
};

/// 17 x 13 vertices at 5 m, offset half a metre off the band's lines.
fn base() -> (Vec<f64>, Vec<u32>) {
    let (columns, rows) = (17_u32, 13_u32);
    let mut points = Vec::new();
    for row in 0..rows {
        for column in 0..columns {
            let (x, y) = (
                -20.5 + 5.0 * f64::from(column),
                -30.5 + 5.0 * f64::from(row),
            );
            points.extend([x, y, 10.0 + 0.05 * x - 0.2 * y]);
        }
    }
    let mut triangles = Vec::new();
    for row in 0..rows - 1 {
        for column in 0..columns - 1 {
            let a = row * columns + column;
            triangles.extend([a, a + 1, a + columns + 1, a, a + columns + 1, a + columns]);
        }
    }
    (points, triangles)
}

fn station(x: f64, z: f64) -> TerrainStation {
    TerrainStation {
        position: [x, 0.0, z],
        half_left: 2.0,
        half_right: 2.0,
        cut_limit: 10.0,
        fill_limit: 10.0,
        cut: HALF,
        fill: HALF,
        ..TerrainStation::default()
    }
}

fn counts_in(points: &[f64], triangles: &[u32]) -> id::EntityCounts {
    let count = |n: usize| u64::try_from(n).unwrap_or(u64::MAX);
    id::EntityCounts {
        vertices: count(points.len() / 3 + REGION.len() / 3),
        edges: count(REGION.len() / 3),
        faces: count(triangles.len() / 3),
        patches: 0,
    }
}

fn request<'a>(
    points: &'a [f64],
    triangles: &'a [u32],
    stations: &'a [TerrainStation],
    rows: &'a [id::MappingRecord],
) -> TerrainHybridRequest {
    TerrainHybridRequest {
        points: view(points),
        triangles: view(triangles),
        region: view(&REGION),
        stations: view(stations),
        tolerance: 1e-6,
        effect: 3,
        reserved: 0,
        provenance: provenance(rows, counts_in(points, triangles), 7),
    }
}

#[derive(Clone, Debug, PartialEq)]
struct Storage {
    counts: TerrainHybridCounts,
    site: TerrainHybridVolumes,
    context: TerrainHybridVolumes,
    points: Vec<f64>,
    base: Vec<f64>,
    owners: Vec<u32>,
    triangles: Vec<u32>,
    mappings: Vec<TerrainMapping>,
    lines: Vec<TerrainHybridLine>,
    sides: Vec<TerrainHybridSide>,
}

impl Storage {
    fn new() -> Self {
        Self {
            counts: TerrainHybridCounts::default(),
            site: TerrainHybridVolumes::default(),
            context: TerrainHybridVolumes::default(),
            points: Vec::new(),
            base: Vec::new(),
            owners: Vec::new(),
            triangles: Vec::new(),
            mappings: Vec::new(),
            lines: Vec::new(),
            sides: Vec::new(),
        }
    }

    fn size(&mut self, stations: usize) {
        let vertices = self.counts.points / 3;
        self.points = vec![77.0; self.counts.points];
        self.base = vec![77.0; vertices];
        self.owners = vec![77; vertices];
        self.triangles = vec![77; self.counts.triangles];
        self.mappings = vec![
            TerrainMapping {
                kind: 77,
                index: 77,
                values: [77; 6],
            };
            self.counts.mappings
        ];
        self.lines = vec![TerrainHybridLine::default(); self.counts.lines];
        self.sides = vec![TerrainHybridSide::default(); stations * 2];
    }

    fn output(&mut self, query: bool) -> TerrainHybridOutput {
        let (points, base, owners, triangles, mappings, lines, sides) = if query {
            (
                (std::ptr::null_mut(), 0),
                (std::ptr::null_mut(), 0),
                (std::ptr::null_mut(), 0),
                (std::ptr::null_mut(), 0),
                (std::ptr::null_mut(), 0),
                (std::ptr::null_mut(), 0),
                (std::ptr::null_mut(), 0),
            )
        } else {
            (
                (self.points.as_mut_ptr(), self.points.len()),
                (self.base.as_mut_ptr(), self.base.len()),
                (self.owners.as_mut_ptr(), self.owners.len()),
                (self.triangles.as_mut_ptr(), self.triangles.len()),
                (self.mappings.as_mut_ptr(), self.mappings.len()),
                (self.lines.as_mut_ptr(), self.lines.len()),
                (self.sides.as_mut_ptr(), self.sides.len()),
            )
        };
        TerrainHybridOutput {
            counts: &raw mut self.counts,
            site: &raw mut self.site,
            context: &raw mut self.context,
            points: points.0,
            point_capacity: points.1,
            base: base.0,
            base_capacity: base.1,
            owners: owners.0,
            owner_capacity: owners.1,
            triangles: triangles.0,
            triangle_capacity: triangles.1,
            mappings: mappings.0,
            mapping_capacity: mappings.1,
            lines: lines.0,
            line_capacity: lines.1,
            sides: sides.0,
            side_capacity: sides.1,
            query_only: u32::from(query),
            reserved: 0,
        }
    }
}

fn run(input: &TerrainHybridRequest, output: &TerrainHybridOutput) -> (i32, String) {
    let mut error = [0_u8; 160];
    // SAFETY: live immutable fixture pools and disjoint caller-owned outputs.
    let status = unsafe {
        tapioca_terrain_hybrid_build(
            std::ptr::from_ref(input),
            std::ptr::from_ref(output),
            error.as_mut_ptr(),
            error.len(),
        )
    };
    let length = error.iter().position(|&b| b == 0).unwrap_or(error.len());
    (
        status,
        String::from_utf8_lossy(error.get(..length).unwrap_or(&[])).into_owned(),
    )
}

#[test]
fn hybrid_layouts_are_the_documented_c_records() {
    assert_eq!(size_of::<TerrainStation>(), 152);
    assert_eq!(size_of::<TerrainHybridVolumes>(), 40);
    assert_eq!(size_of::<TerrainHybridLine>(), 16);
    assert_eq!(size_of::<TerrainHybridSide>(), 24);
    if size_of::<usize>() == 8 {
        assert_eq!(size_of::<TerrainHybridCounts>(), 32);
        assert_eq!(size_of::<TerrainHybridRequest>(), 360);
        assert_eq!(std::mem::offset_of!(TerrainHybridRequest, provenance), 80);
        assert_eq!(size_of::<TerrainHybridOutput>(), 144);
        assert_eq!(std::mem::offset_of!(TerrainHybridOutput, query_only), 136);
    }
}

#[test]
fn query_then_fill_publishes_the_kernel_build_exactly() {
    let (points, triangles) = base();
    let stations = [station(0.0, 10.0), station(40.0, 12.0)];
    let rows = rows(counts_in(&points, &triangles));
    let input = request(&points, &triangles, &stations, &rows);
    let mut storage = Storage::new();
    let query = storage.output(true);
    assert_eq!(run(&input, &query), (0, String::new()));
    // Query mode publishes counts only.
    assert_eq!(storage.site, TerrainHybridVolumes::default());
    storage.size(stations.len());
    let fill = storage.output(false);
    assert_eq!(run(&input, &fill), (0, String::new()));

    let kernel_stations: Vec<Station> = stations.iter().map(hybrid_station).collect();
    let built = band_kernel::build(&band_kernel::BuildRequest {
        points: &points,
        triangles: &triangles,
        region: &REGION,
        band: Band {
            stations: &kernel_stations,
            effect: Effect::Both,
        },
        tolerance: 1e-6,
    });
    assert!(built.is_ok());
    let Ok(built) = built else { return };
    let encoded = hybrid::encode(built);
    assert!(encoded.is_ok());
    let Ok(encoded) = encoded else { return };
    assert_eq!(storage.points, encoded.points);
    assert_eq!(storage.base, encoded.base);
    assert_eq!(storage.owners, encoded.owners);
    assert_eq!(storage.triangles, encoded.triangles);
    assert_eq!(storage.mappings, encoded.mappings);
    assert_eq!(storage.lines, encoded.lines);
    assert_eq!(storage.sides, encoded.sides);
    assert_eq!(storage.site, encoded.site);
    assert_eq!(storage.context, encoded.context);

    // Edge L, Daylight L0, Edge R, Daylight R0, End first, End last.
    let kinds: Vec<[u32; 3]> = storage
        .lines
        .iter()
        .map(|l| [l.kind, l.side, l.root])
        .collect();
    assert_eq!(
        kinds,
        vec![
            [1, 1, 0],
            [3, 1, 0],
            [1, 2, 0],
            [3, 2, 0],
            [4, 0, 0],
            [4, 0, 1]
        ]
    );
    // Daylight 4/3 m on both sides of both stations.
    for side in &storage.sides {
        assert_eq!(side.roots, 1);
        assert!((side.first - 4.0 / 3.0).abs() < 1e-9);
    }
    assert_eq!(storage.context.cut, 0.0);
    assert_eq!(storage.context.fill, 0.0);
    assert!(storage.site.fill >= 80.0 / 3.0 - 1e-9 && storage.site.cut >= 80.0 / 3.0 - 1e-9);
    // Every output face has one region face row; the ring closes.
    let faces = storage.mappings.iter().filter(|r| r.kind == 12).count();
    assert_eq!(faces * 3, storage.triangles.len());
    assert!(storage.mappings.iter().filter(|r| r.kind == 17).count() >= 4);
    // Band vertices carry owner 0, outside vertices none.
    let vertices = storage.points.as_chunks::<3>().0;
    for (&[x, y, z], (&owner, &b)) in vertices
        .iter()
        .zip(storage.owners.iter().zip(&storage.base))
    {
        if !(-10.0..=50.0).contains(&x) || y.abs() > 15.0 {
            assert_eq!((owner, z.to_bits()), (u32::MAX, b.to_bits()));
        }
    }
}

fn hybrid_station(raw: &TerrainStation) -> Station {
    let slope = |s: TerrainSlope| tapioca_geometry::slope::Slope::RiseRun {
        rise: s.a,
        run: s.b,
    };
    Station {
        position: raw.position,
        half_left: raw.half_left,
        half_right: raw.half_right,
        crossfall: None,
        shoulder_left: raw.shoulder_left,
        shoulder_right: raw.shoulder_right,
        cut: slope(raw.cut),
        fill: slope(raw.fill),
        cut_limit: raw.cut_limit,
        fill_limit: raw.fill_limit,
    }
}

#[test]
fn refusals_preserve_every_output() {
    let (points, triangles) = base();
    let stations = [station(0.0, 10.0), station(40.0, 12.0)];
    let rows = rows(counts_in(&points, &triangles));
    let mut storage = Storage::new();
    assert_eq!(
        run(
            &request(&points, &triangles, &stations, &rows),
            &storage.output(true)
        )
        .0,
        0
    );
    storage.size(stations.len());
    let unmet = [
        TerrainStation {
            cut_limit: 0.5,
            fill_limit: 0.5,
            ..station(0.0, 10.0)
        },
        station(40.0, 12.0),
    ];
    let long = [station(-20.0, 9.0), station(40.0, 12.0)];
    let bad_hand = [
        TerrainStation {
            crossfall_hand: 4,
            ..station(0.0, 10.0)
        },
        station(40.0, 12.0),
    ];
    let before_counts = storage.counts;
    for case in 0..7 {
        let mut input = request(&points, &triangles, &stations, &rows);
        let mut output = storage.output(false);
        let expected = match case {
            0 => {
                input.stations = view(&unmet);
                (1, "DaylightNotReached")
            }
            1 => {
                input.stations = view(&long);
                (1, "RegionBreakline")
            }
            2 => {
                input.stations = view(&bad_hand);
                (1, "TerrainHybridHand")
            }
            3 => {
                input.effect = 4;
                (1, "TerrainHybridEffect")
            }
            4 => {
                input.provenance.domains = 5;
                (2, "ProvenanceConflict")
            }
            5 => {
                output.mapping_capacity = output.mapping_capacity.saturating_sub(1);
                (1, "TerrainOutputTooSmall")
            }
            _ => {
                output.query_only = 1;
                (1, "TerrainOutputMode")
            }
        };
        let (status, text) = run(&input, &output);
        assert_eq!(status, expected.0, "case {case}: {text}");
        assert!(text.starts_with(expected.1), "case {case}: {text}");
        assert_eq!(storage.counts, before_counts);
        assert!(
            storage
                .points
                .iter()
                .all(|v| v.to_bits() == 77.0_f64.to_bits())
        );
        assert!(storage.owners.iter().all(|&v| v == 77));
        assert_eq!(storage.site, TerrainHybridVolumes::default());
    }
}

#[test]
fn a_crowned_station_adds_the_centreline_line() {
    let (points, triangles) = base();
    let crowned = |x: f64, z: f64| TerrainStation {
        crossfall: TerrainSlope {
            a: 2.0,
            b: 0.0,
            kind: 2,
            reserved: 0,
        },
        crossfall_hand: 3,
        ..station(x, z)
    };
    let stations = [crowned(0.0, 10.0), crowned(40.0, 12.0)];
    let rows = rows(counts_in(&points, &triangles));
    let input = request(&points, &triangles, &stations, &rows);
    let mut storage = Storage::new();
    assert_eq!(run(&input, &storage.output(true)), (0, String::new()));
    storage.size(stations.len());
    assert_eq!(run(&input, &storage.output(false)), (0, String::new()));
    let crown: Vec<_> = storage.lines.iter().filter(|l| l.kind == 5).collect();
    assert_eq!(crown.len(), 1);
    assert!(crown.iter().all(|l| l.side == 0 && l.root == 0));
    // Fill 0.576 and cut 0.7627 m3 per metre between the ends, plus the caps.
    assert!(storage.site.fill >= 40.0 * 0.576 - 1e-9);
    assert!(storage.site.cut >= 40.0 * (0.44 + 0.5 * 0.44 * 22.0 / 15.0) - 1e-9);
}

fn conflicts_run(
    input: &TerrainHybridRequest,
    count: &mut usize,
    rows: &mut [TerrainConflict],
    query: bool,
) -> (i32, String) {
    let output = TerrainConflictOutput {
        count: std::ptr::from_mut(count),
        rows: if query {
            std::ptr::null_mut()
        } else {
            rows.as_mut_ptr()
        },
        capacity: if query { 0 } else { rows.len() },
        query_only: u32::from(query),
        reserved: 0,
    };
    let mut error = [0_u8; 160];
    // SAFETY: live immutable fixture pools and disjoint caller-owned outputs.
    let status = unsafe {
        tapioca_terrain_hybrid_conflicts(
            std::ptr::from_ref(input),
            &raw const output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    let length = error.iter().position(|&b| b == 0).unwrap_or(error.len());
    (
        status,
        String::from_utf8_lossy(error.get(..length).unwrap_or(&[])).into_owned(),
    )
}

#[test]
fn conflict_layouts_and_a_refused_band_assesses_with_locations() {
    assert_eq!(size_of::<TerrainConflict>(), 48);
    assert_eq!(size_of::<TerrainClip>(), 32);
    if size_of::<usize>() == 8 {
        assert_eq!(size_of::<TerrainConflictOutput>(), 32);
        assert_eq!(size_of::<TerrainClipOutput>(), 32);
    }
    let (points, triangles) = base();
    let short = [
        TerrainStation {
            cut_limit: 0.5,
            fill_limit: 0.5,
            ..station(0.0, 10.0)
        },
        TerrainStation {
            cut_limit: 0.5,
            fill_limit: 0.5,
            ..station(40.0, 12.0)
        },
    ];
    let rows = rows(counts_in(&points, &triangles));
    let input = request(&points, &triangles, &short, &rows);
    // The build refuses ...
    let mut storage = Storage::new();
    assert!(
        run(&input, &storage.output(true))
            .1
            .starts_with("DaylightNotReached")
    );
    // ... and the assessment names all four places.
    let mut count = 0;
    assert_eq!(
        conflicts_run(&input, &mut count, &mut [], true),
        (0, String::new())
    );
    assert_eq!(count, 4);
    let mut found = vec![TerrainConflict::default(); count];
    assert_eq!(
        conflicts_run(&input, &mut count, &mut found, false),
        (0, String::new())
    );
    assert!(
        found
            .iter()
            .all(|c| c.code == 2 && c.hard == 1 && (c.magnitude - 4.0 / 3.0).abs() < 1e-9)
    );
    let sides: Vec<[u32; 2]> = found.iter().map(|c| [c.side, c.station]).collect();
    assert_eq!(sides, vec![[1, 0], [1, 1], [2, 0], [2, 1]]);
    assert!(
        found
            .first()
            .is_some_and(|c| c.position[0].abs() < 1e-12 && (c.position[1] - 2.5).abs() < 1e-12)
    );
    // Attribution still refuses with STATE and writes nothing.
    let mut wrong = input;
    wrong.provenance.domains = 5;
    let mut kept = 9;
    let (status, text) = conflicts_run(&wrong, &mut kept, &mut found, false);
    assert_eq!((status, kept), (2, 9));
    assert!(text.starts_with("ProvenanceConflict"));
}
