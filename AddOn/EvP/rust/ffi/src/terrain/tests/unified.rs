//! Unified landscape-build export against the safe kernel (#112 P3).
use super::*;

const PROPERTY: [f64; 12] = [0.0, 0.0, 0.0, 8.0, 0.0, 0.0, 8.0, 8.0, 0.0, 0.0, 8.0, 0.0];
const DOMAIN: [f64; 12] = [2.0, 2.0, 0.0, 6.0, 2.0, 0.0, 6.0, 6.0, 0.0, 2.0, 6.0, 0.0];
const SLOPE: TerrainSlope = TerrainSlope {
    a: 100.0,
    b: 0.0,
    kind: 2,
    reserved: 0,
};

/// 9 x 9 vertices at 1 m on z = 0, so every ring sits on mesh edges.
fn base() -> (Vec<f64>, Vec<u32>) {
    let mut points = Vec::new();
    for row in 0..9 {
        for column in 0..9 {
            points.extend([f64::from(column), f64::from(row), 0.0]);
        }
    }
    let mut triangles = Vec::new();
    for row in 0..8 {
        for column in 0..8 {
            let a = row * 9 + column;
            triangles.extend([a, a + 1, a + 10, a, a + 10, a + 9]);
        }
    }
    (points, triangles)
}

fn counts_in(ring_vertices: usize) -> id::EntityCounts {
    id::EntityCounts {
        vertices: (81 + PROPERTY.len() / 3 + ring_vertices) as u64,
        edges: (PROPERTY.len() / 3 + ring_vertices) as u64,
        faces: 128,
        patches: 0,
    }
}

fn entry(order: u32, level: f64) -> UnifiedEntry {
    UnifiedEntry {
        object: 0x101 + u64::from(order),
        root_operation: 0x202 + u64::from(order),
        level,
        falloff: 0.0,
        cut: SLOPE,
        fill: SLOPE,
        reach: 1.0,
        surface_points: TerrainRing {
            offset: 0,
            count: 0,
        },
        surface_triangles: TerrainRing {
            offset: 0,
            count: 0,
        },
        order,
        surface: 2,
        effect: 1,
        transition: 2,
        first_part: order,
        part_count: 1,
        first_source: order,
        source_count: 1,
    }
}

/// Borrowing request over caller-owned backing arrays, so the views stay
/// valid for the whole test (mirrors the hybrid band test fixture).
#[expect(
    clippy::too_many_arguments,
    reason = "a complete plan fixture needs all of its shared pools together"
)]
fn scene<'a>(
    points: &'a [f64],
    triangles: &'a [u32],
    entries: &'a [UnifiedEntry],
    geometry: &'a [f64],
    sources: &'a [u64],
    rows: &'a [id::MappingRecord],
    controls: &'a [UnifiedControl],
    rings: &'a [TerrainRing],
    part_pool: &'a [UnifiedPart],
    part_indices: &'a [u32],
) -> UnifiedRequest {
    UnifiedRequest {
        points: view(points),
        triangles: view(triangles),
        property: view(&PROPERTY),
        geometry: view(geometry),
        rings: view(rings),
        parts: view(part_pool),
        part_indices: view(part_indices),
        stations: empty(),
        surface_indices: empty(),
        sources: view(sources),
        entries: view(entries),
        paths: empty(),
        masks: empty(),
        controls: view(controls),
        control_tolerance: 1e-3,
        revision: 7,
        plan_hash: 0xABCD,
        provenance: provenance(rows, rows_counts(rows), 7),
        outlines: empty(),
        outline_lines: empty(),
        outline_grades: empty(),
    }
}

fn rows_counts(rows: &[id::MappingRecord]) -> id::EntityCounts {
    let count = |domain: u64| {
        u64::try_from(
            rows.iter()
                .filter(|row| u64::from(row.domain) == domain)
                .count(),
        )
        .unwrap_or(u64::MAX)
    };
    id::EntityCounts {
        vertices: count(1),
        edges: count(2),
        faces: count(3),
        patches: 0,
    }
}

#[derive(Clone, Debug, PartialEq)]
struct Storage {
    counts: UnifiedCounts,
    meta: UnifiedMeta,
    site: TerrainHybridVolumes,
    context: TerrainHybridVolumes,
    points: Vec<f64>,
    base: Vec<f64>,
    owners: Vec<u32>,
    triangles: Vec<u32>,
    mappings: Vec<TerrainMapping>,
    lines: Vec<UnifiedLine>,
    locked: Vec<UnifiedLocked>,
    steps: Vec<UnifiedStep>,
}

impl Storage {
    fn new() -> Self {
        Self {
            counts: UnifiedCounts::default(),
            meta: UnifiedMeta::default(),
            site: TerrainHybridVolumes::default(),
            context: TerrainHybridVolumes::default(),
            points: Vec::new(),
            base: Vec::new(),
            owners: Vec::new(),
            triangles: Vec::new(),
            mappings: Vec::new(),
            lines: Vec::new(),
            locked: Vec::new(),
            steps: Vec::new(),
        }
    }

    fn size(&mut self) {
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
        self.lines = vec![UnifiedLine::default(); self.counts.lines];
        self.locked = vec![UnifiedLocked::default(); self.counts.locked];
        self.steps = vec![
            UnifiedStep {
                kind: 77,
                ..UnifiedStep::default()
            };
            self.counts.steps
        ];
    }

    fn query(
        counts: &mut UnifiedCounts,
        meta: &mut UnifiedMeta,
        site: &mut TerrainHybridVolumes,
        context: &mut TerrainHybridVolumes,
    ) -> UnifiedOutput {
        UnifiedOutput {
            counts,
            meta,
            site,
            context,
            points: std::ptr::null_mut(),
            point_capacity: 0,
            base: std::ptr::null_mut(),
            base_capacity: 0,
            owners: std::ptr::null_mut(),
            owner_capacity: 0,
            triangles: std::ptr::null_mut(),
            triangle_capacity: 0,
            mappings: std::ptr::null_mut(),
            mapping_capacity: 0,
            lines: std::ptr::null_mut(),
            line_capacity: 0,
            locked: std::ptr::null_mut(),
            locked_capacity: 0,
            steps: std::ptr::null_mut(),
            step_capacity: 0,
            query_only: 1,
            reserved: 0,
        }
    }

    fn fill(&mut self) -> UnifiedOutput {
        UnifiedOutput {
            counts: std::ptr::from_mut(&mut self.counts),
            meta: std::ptr::from_mut(&mut self.meta),
            site: std::ptr::from_mut(&mut self.site),
            context: std::ptr::from_mut(&mut self.context),
            points: self.points.as_mut_ptr(),
            point_capacity: self.points.len(),
            base: self.base.as_mut_ptr(),
            base_capacity: self.base.len(),
            owners: self.owners.as_mut_ptr(),
            owner_capacity: self.owners.len(),
            triangles: self.triangles.as_mut_ptr(),
            triangle_capacity: self.triangles.len(),
            mappings: self.mappings.as_mut_ptr(),
            mapping_capacity: self.mappings.len(),
            lines: self.lines.as_mut_ptr(),
            line_capacity: self.lines.len(),
            locked: self.locked.as_mut_ptr(),
            locked_capacity: self.locked.len(),
            steps: self.steps.as_mut_ptr(),
            step_capacity: self.steps.len(),
            query_only: 0,
            reserved: 0,
        }
    }
}

fn build(request: &UnifiedRequest, storage: &mut Storage) -> Result<(), (i32, String)> {
    let mut meta = UnifiedMeta::default();
    let mut site = TerrainHybridVolumes::default();
    let mut context = TerrainHybridVolumes::default();
    let mut counts = UnifiedCounts::default();
    let query_output = Storage::query(&mut counts, &mut meta, &mut site, &mut context);
    let mut buffer = [0_u8; 256];
    // SAFETY: live immutable request pools and exclusive count/meta/volume records.
    let status = unsafe {
        tapioca_terrain_unified_build(
            &raw const *request,
            &raw const query_output,
            buffer.as_mut_ptr(),
            buffer.len(),
        )
    };
    if status != 0 {
        return Err((status, message(&buffer)));
    }
    let first = counts;
    let second_output = Storage::query(&mut counts, &mut meta, &mut site, &mut context);
    // SAFETY: same live immutable request and exclusive records as the first query.
    let status = unsafe {
        tapioca_terrain_unified_build(
            &raw const *request,
            &raw const second_output,
            buffer.as_mut_ptr(),
            buffer.len(),
        )
    };
    if status != 0 {
        return Err((status, message(&buffer)));
    }
    assert_eq!(first, counts, "repeat query must be deterministic");
    let expected = counts;
    storage.counts = counts;
    storage.size();
    let fill_output = storage.fill();
    // SAFETY: request pools live and immutable; every output array is exclusively
    // writable storage sized from the deterministic query counts.
    let status = unsafe {
        tapioca_terrain_unified_build(
            &raw const *request,
            &raw const fill_output,
            buffer.as_mut_ptr(),
            buffer.len(),
        )
    };
    if status != 0 {
        return Err((status, message(&buffer)));
    }
    if storage.counts != expected {
        return Err((-1, "unified query/fill counts disagree".to_owned()));
    }
    Ok(())
}

fn message(buffer: &[u8; 256]) -> String {
    let end = buffer
        .iter()
        .position(|&byte| byte == 0)
        .unwrap_or(buffer.len());
    String::from_utf8_lossy(buffer.get(..end).unwrap_or(buffer)).into_owned()
}

#[test]
fn layouts_match_the_documented_c_records() {
    assert_eq!(size_of::<UnifiedPart>(), 8);
    assert_eq!(size_of::<UnifiedEntry>(), 136);
    assert_eq!(size_of::<UnifiedPath>(), 56);
    assert_eq!(size_of::<UnifiedMask>(), 32);
    assert_eq!(size_of::<UnifiedControl>(), 56);
    assert_eq!(size_of::<UnifiedCounts>(), 48);
    assert_eq!(size_of::<UnifiedMeta>(), 24);
    assert_eq!(size_of::<UnifiedOutline>(), 120);
    assert_eq!(size_of::<UnifiedRequest>(), 576);
    assert_eq!(size_of::<UnifiedLine>(), 32);
    assert_eq!(size_of::<UnifiedLocked>(), 16);
    assert_eq!(size_of::<UnifiedStep>(), 48);
    assert_eq!(std::mem::offset_of!(UnifiedStep, position), 8);
    assert_eq!(size_of::<UnifiedOutput>(), 168);
    assert_eq!(size_of::<PreviewCounts>(), 40);
    assert_eq!(size_of::<PreviewSummary>(), 72);
    assert_eq!(size_of::<PreviewLine>(), 32);
    assert_eq!(size_of::<PreviewOutput>(), 104);
}

#[test]
fn grade_entry_composes_heights_origins_volumes_and_meta() {
    let (points, triangles) = base();
    let entries = [entry(0, 2.0)];
    let geometry = DOMAIN.to_vec();
    let sources = [0x1001_u64];
    let counts = counts_in(4);
    let rows = rows(counts);
    let no_controls: [UnifiedControl; 0] = [];
    let ring_pool = [TerrainRing {
        offset: 0,
        count: 12,
    }];
    let part_pool = [UnifiedPart {
        first_ring: 0,
        ring_count: 1,
    }];
    let part_indices = [0_u32, 1];
    let request = scene(
        &points,
        &triangles,
        &entries,
        &geometry,
        &sources,
        &rows,
        &no_controls,
        &ring_pool,
        &part_pool,
        &part_indices,
    );
    let mut storage = Storage::new();
    let built = build(&request, &mut storage);
    assert!(built.is_ok(), "unified build refused: {:?}", built.err());
    let vertex = |x: f64, y: f64| {
        storage
            .points
            .as_chunks::<3>()
            .0
            .iter()
            .position(|&[px, py, _]| px.to_bits() == x.to_bits() && py.to_bits() == y.to_bits())
            .map(|at| u32::try_from(at).unwrap_or(u32::MAX))
    };
    let height = |at: u32| {
        storage
            .points
            .get(at as usize * 3 + 2)
            .copied()
            .unwrap_or(f64::NAN)
    };
    let inside = vertex(4.0, 4.0).unwrap_or(u32::MAX);
    assert_ne!(inside, u32::MAX, "interior vertex");
    assert_eq!(height(inside), 2.0);
    assert_eq!(storage.base.get(inside as usize), Some(&0.0));
    assert_eq!(storage.owners.get(inside as usize), Some(&0));
    let outside = vertex(0.0, 0.0).unwrap_or(u32::MAX);
    assert_ne!(outside, u32::MAX, "outside vertex");
    assert_eq!(height(outside), 0.0);
    assert_eq!(storage.owners.get(outside as usize), Some(&u32::MAX));
    // One ring of four segments gives four domain-1 breakline origins.
    assert_eq!(storage.counts.lines, 4);
    assert!(
        storage
            .lines
            .iter()
            .all(|line| line.domain == 1 && line.index == 0)
    );
    assert!(storage.lines.iter().any(|line| line.segment == 0));
    // The result carries the ring as the property-line mapping rows (17).
    assert!(storage.mappings.iter().any(|row| row.kind == 17));
    // Exact fill over the graded square dominates the boundary interpolation.
    assert!(storage.site.fill > 30.0, "site fill {}", storage.site.fill);
    assert_eq!(storage.site.cut, 0.0);
    assert_eq!(storage.meta.revision, 7);
    assert_eq!(storage.meta.plan_hash, 0xABCD);
    assert_eq!(storage.counts.locked, 0);
}

#[test]
fn adjacent_sharp_entries_refuse_atomically_with_the_hard_step_code() {
    let left: [f64; 12] = [2.0, 2.0, 0.0, 4.0, 2.0, 0.0, 4.0, 6.0, 0.0, 2.0, 6.0, 0.0];
    let right: [f64; 12] = [4.0, 2.0, 0.0, 6.0, 2.0, 0.0, 6.0, 6.0, 0.0, 4.0, 6.0, 0.0];
    let (points, triangles) = base();
    let mut geometry = left.to_vec();
    geometry.extend_from_slice(&right);
    let mut left_entry = entry(0, 1.0);
    left_entry.reach = 0.0;
    let mut right_entry = entry(1, 3.0);
    right_entry.reach = 0.0;
    let entries = [left_entry, right_entry];
    let sources = [0x1001_u64, 0x1002];
    let ring_pool = [
        TerrainRing {
            offset: 0,
            count: 12,
        },
        TerrainRing {
            offset: 12,
            count: 12,
        },
    ];
    let part_pool = [
        UnifiedPart {
            first_ring: 0,
            ring_count: 1,
        },
        UnifiedPart {
            first_ring: 1,
            ring_count: 1,
        },
    ];
    let part_indices = [0_u32, 1];
    let counts = counts_in(8);
    let rows = rows(counts);
    let no_controls: [UnifiedControl; 0] = [];
    let request = scene(
        &points,
        &triangles,
        &entries,
        &geometry,
        &sources,
        &rows,
        &no_controls,
        &ring_pool,
        &part_pool,
        &part_indices,
    );
    let mut meta = UnifiedMeta::default();
    let mut site = TerrainHybridVolumes::default();
    let mut context = TerrainHybridVolumes::default();
    let mut counts = UnifiedCounts::default();
    let before = (counts, meta, site, context);
    let query_output = Storage::query(&mut counts, &mut meta, &mut site, &mut context);
    // SAFETY: live immutable request and exclusive count/meta/volume records.
    let status = unsafe {
        tapioca_terrain_unified_build(
            &raw const request,
            &raw const query_output,
            std::ptr::null_mut(),
            0,
        )
    };
    assert_eq!(status, 1, "hard steps refuse with ERR_ARG");
    assert_eq!(
        (counts, meta, site, context),
        before,
        "refusal publishes nothing"
    );
}

#[test]
fn provenance_mismatch_refuses_with_state_and_changes_nothing() {
    let (points, triangles) = base();
    let entries = [entry(0, 2.0)];
    let geometry = DOMAIN.to_vec();
    let sources = [0x1001_u64];
    let counts = counts_in(4);
    let rows = rows(counts);
    let no_controls: [UnifiedControl; 0] = [];
    let ring_pool = [TerrainRing {
        offset: 0,
        count: 12,
    }];
    let part_pool = [UnifiedPart {
        first_ring: 0,
        ring_count: 1,
    }];
    let part_indices = [0_u32, 1];
    let mut request = scene(
        &points,
        &triangles,
        &entries,
        &geometry,
        &sources,
        &rows,
        &no_controls,
        &ring_pool,
        &part_pool,
        &part_indices,
    );
    request.provenance.counts.vertices -= 1;
    let mut meta = UnifiedMeta::default();
    let mut site = TerrainHybridVolumes::default();
    let mut context = TerrainHybridVolumes::default();
    let mut counts = UnifiedCounts::default();
    let before = (counts, meta, site, context);
    let query_output = Storage::query(&mut counts, &mut meta, &mut site, &mut context);
    // SAFETY: live immutable request and exclusive count/meta/volume records.
    let status = unsafe {
        tapioca_terrain_unified_build(
            &raw const request,
            &raw const query_output,
            std::ptr::null_mut(),
            0,
        )
    };
    assert_eq!(status, 2, "attribution refusals return ERR_STATE");
    assert_eq!(
        (counts, meta, site, context),
        before,
        "refusal publishes nothing"
    );
}

#[test]
fn point_control_locks_its_vertex_and_publishes_rows() {
    let (points, triangles) = base();
    let entries = [entry(0, 2.0)];
    let geometry = DOMAIN.to_vec();
    let sources = [0x1001_u64];
    let mut counts = counts_in(4);
    counts.vertices += 1;
    let rows = rows(counts);
    let controls = [UnifiedControl {
        point: 1,
        reserved: 0,
        source: 0x3001,
        vertex: 0x4001,
        position: [4.0, 4.0, 2.0],
        line: TerrainRing {
            offset: 0,
            count: 0,
        },
    }];
    let ring_pool = [TerrainRing {
        offset: 0,
        count: 12,
    }];
    let part_pool = [UnifiedPart {
        first_ring: 0,
        ring_count: 1,
    }];
    let part_indices = [0_u32, 1];
    let request = scene(
        &points,
        &triangles,
        &entries,
        &geometry,
        &sources,
        &rows,
        &controls,
        &ring_pool,
        &part_pool,
        &part_indices,
    );
    let mut storage = Storage::new();
    let built = build(&request, &mut storage);
    assert!(built.is_ok(), "unified build refused: {:?}", built.err());
    assert_eq!(storage.counts.locked, 1);
    let locked = storage.locked.first().copied().unwrap_or_default();
    assert_eq!(locked.control, 0);
    assert_eq!(
        storage
            .points
            .get(locked.vertex as usize * 3 + 2)
            .copied()
            .unwrap_or(f64::NAN),
        2.0
    );
    assert!(storage.mappings.iter().any(|row| row.kind == 21));
    // The control spot is not a breakline; only the four domain lines publish.
    assert_eq!(storage.counts.lines, 4);
}

#[test]
fn two_point_fixed_line_has_single_provenance_coverage() {
    let (points, triangles) = base();
    let entries = [entry(0, 2.0)];
    let mut geometry = DOMAIN.to_vec();
    geometry.extend_from_slice(&[2.0, 4.0, 2.0, 6.0, 4.0, 2.0]);
    let sources = [0x1001_u64];
    let mut counts = counts_in(4);
    counts.vertices += 2;
    counts.edges += 1;
    let rows = rows(counts);
    let controls = [UnifiedControl {
        point: 0,
        reserved: 0,
        source: 0x3001,
        vertex: 0,
        position: [0.0; 3],
        line: TerrainRing {
            offset: 12,
            count: 6,
        },
    }];
    let ring_pool = [
        TerrainRing {
            offset: 0,
            count: 12,
        },
        TerrainRing {
            offset: 12,
            count: 6,
        },
    ];
    let part_pool = [UnifiedPart {
        first_ring: 0,
        ring_count: 1,
    }];
    let part_indices = [0_u32];
    let request = scene(
        &points,
        &triangles,
        &entries,
        &geometry,
        &sources,
        &rows,
        &controls,
        &ring_pool,
        &part_pool,
        &part_indices,
    );
    let mut storage = Storage::new();
    let built = build(&request, &mut storage);
    assert!(built.is_ok(), "fixed line refused: {:?}", built.err());
    assert!(storage.counts.locked >= 2);
    assert_eq!(storage.counts.lines, 5);
    assert!(
        storage
            .lines
            .iter()
            .any(|line| { line.domain == 4 && line.index == 0 && line.segment == 1 })
    );
}

#[test]
fn nested_transitions_and_coincident_property_entry_ring_builds() {
    let (points, triangles) = base();
    // Property ring reused as the outer entry domain ("grade the whole
    // property") with a strictly nested override at a different height.
    let property_and_outer: [f64; 12] =
        [2.0, 2.0, 0.0, 7.0, 2.0, 0.0, 7.0, 7.0, 0.0, 2.0, 7.0, 0.0];
    let inner: [f64; 12] = [4.0, 4.0, 0.0, 6.0, 4.0, 0.0, 6.0, 6.0, 0.0, 4.0, 6.0, 0.0];
    let mut geometry = property_and_outer.to_vec();
    geometry.extend_from_slice(&inner);
    let ring_pool = [
        TerrainRing {
            offset: 0,
            count: 12,
        },
        TerrainRing {
            offset: 12,
            count: 12,
        },
    ];
    let part_pool = [
        UnifiedPart {
            first_ring: 0,
            ring_count: 1,
        },
        UnifiedPart {
            first_ring: 1,
            ring_count: 1,
        },
    ];
    let part_indices = [0_u32, 1];
    let entries = [entry(0, 3.0), entry(1, 1.0)];
    let sources = [0x1001_u64, 0x1002];
    // Vertices: 81 mesh + 4 property + 8 entry rings; edges: 4 + 8; faces: 128.
    let counts = id::EntityCounts {
        vertices: 81 + 4 + 8,
        edges: 4 + 8,
        faces: 128,
        patches: 0,
    };
    let rows = rows(counts);
    let no_controls: [UnifiedControl; 0] = [];
    let request = scene(
        &points,
        &triangles,
        &entries,
        &geometry,
        &sources,
        &rows,
        &no_controls,
        &ring_pool,
        &part_pool,
        &part_indices,
    );
    // The request property is the shared 8 m square; replace it with the
    // coincident outer ring by rebuilding the request field.
    let mut request = request;
    request.property = view(&property_and_outer);
    let mut storage = Storage::new();
    let built = build(&request, &mut storage);
    assert!(built.is_ok(), "unified build refused: {:?}", built.err());
    let height = |x: f64, y: f64| {
        storage
            .points
            .as_chunks::<3>()
            .0
            .iter()
            .find(|&[px, py, _]| px.to_bits() == x.to_bits() && py.to_bits() == y.to_bits())
            .map_or(f64::NAN, |&[_, _, z]| z)
    };
    assert_eq!(height(5.0, 5.0), 1.0, "inner override wins");
    assert_eq!(height(3.0, 3.0), 3.0, "annulus keeps the outer target");
    assert_eq!(height(0.0, 0.0), 0.0, "outside stays baseline");
}

/// #175: the field preview of the graded entry, through the C ABI: too-small capacities return
/// the counts and write nothing else, a second call with them writes everything, and its fill
/// is the build's to within the grid's resolution.
#[test]
fn the_field_preview_answers_in_one_call_once_its_capacities_fit() {
    let (points, triangles) = base();
    let entries = [entry(0, 2.0)];
    let geometry = DOMAIN.to_vec();
    let sources = [0x1001_u64];
    let rows = rows(counts_in(4));
    let no_controls: [UnifiedControl; 0] = [];
    let ring_pool = [TerrainRing {
        offset: 0,
        count: 12,
    }];
    let part_pool = [UnifiedPart {
        first_ring: 0,
        ring_count: 1,
    }];
    let part_indices = [0_u32, 1];
    let request = scene(
        &points,
        &triangles,
        &entries,
        &geometry,
        &sources,
        &rows,
        &no_controls,
        &ring_pool,
        &part_pool,
        &part_indices,
    );
    let mut storage = Storage::new();
    assert!(build(&request, &mut storage).is_ok());
    let mut counts = PreviewCounts::default();
    let mut summary = PreviewSummary::default();
    let mut buffer = [0_u8; 256];
    let mut output = PreviewOutput {
        step: 0.05,
        interval: 1.0,
        max_reach: 0.0,
        counts: &raw mut counts,
        summary: &raw mut summary,
        points: std::ptr::null_mut(),
        point_capacity: 0,
        triangles: std::ptr::null_mut(),
        triangle_capacity: 0,
        lines: std::ptr::null_mut(),
        line_capacity: 0,
        line_points: std::ptr::null_mut(),
        line_point_capacity: 0,
    };
    // SAFETY: every pointer is live for the call and the buffers are disjoint.
    let status = unsafe {
        tapioca_terrain_unified_preview(
            &raw const request,
            &raw const output,
            buffer.as_mut_ptr(),
            buffer.len(),
        )
    };
    assert_eq!(status, 0, "{}", message(&buffer));
    assert_eq!(counts.complete, 0);
    assert!(counts.points > 0 && counts.triangles > 0 && counts.lines > 0);
    assert_eq!(
        summary,
        PreviewSummary::default(),
        "nothing but the counts is written"
    );
    let mut surface = vec![0.0; counts.points];
    let mut indices = vec![0_u32; counts.triangles];
    let mut lines = vec![PreviewLine::default(); counts.lines];
    let mut line_points = vec![0.0; counts.line_points];
    output.points = surface.as_mut_ptr();
    output.point_capacity = surface.len();
    output.triangles = indices.as_mut_ptr();
    output.triangle_capacity = indices.len();
    output.lines = lines.as_mut_ptr();
    output.line_capacity = lines.len();
    output.line_points = line_points.as_mut_ptr();
    output.line_point_capacity = line_points.len();
    // SAFETY: as above, with every capacity the first call reported.
    let status = unsafe {
        tapioca_terrain_unified_preview(
            &raw const request,
            &raw const output,
            buffer.as_mut_ptr(),
            buffer.len(),
        )
    };
    assert_eq!(status, 0, "{}", message(&buffer));
    assert_eq!(counts.complete, 1);
    assert_eq!(summary.interval, 1.0);
    // Levels 0 (bounding the raised square) and 1 (on its transition).
    let levels: Vec<f64> = lines.iter().map(|line| line.level).collect();
    assert!(levels.contains(&0.0) && levels.contains(&1.0), "{levels:?}");
    let last = lines.last().copied().unwrap_or_default();
    assert_eq!(
        usize::try_from(last.first + last.count)
            .ok()
            .map(|points| points * 3),
        Some(line_points.len())
    );
    assert!(
        (summary.fill - storage.site.fill).abs() <= 0.03 * storage.site.fill,
        "preview fill {} vs build {}",
        summary.fill,
        storage.site.fill
    );
    assert!(summary.cut.abs() < 1e-9);
    assert!(indices.iter().all(|&v| (v as usize) < surface.len() / 3));
}

/// A level 2 x 2 m pavement at 2 m over the z = 0 grid, 3 m inside the property on every side.
const PAVED: [f64; 12] = [3.0, 3.0, 2.0, 5.0, 3.0, 2.0, 5.0, 5.0, 2.0, 3.0, 5.0, 2.0];
const PAVED_TRIANGLES: [u32; 6] = [0, 1, 2, 0, 2, 3];
const HALF: TerrainSlope = TerrainSlope {
    a: 1.0,
    b: 2.0,
    kind: 3,
    reserved: 0,
};

/// Owned pools of the outline-only unified plan over [`PAVED`] (#149 T1).
struct OutlineScene {
    points: Vec<f64>,
    triangles: Vec<u32>,
    rows: Vec<id::MappingRecord>,
    outlines: [UnifiedOutline; 1],
}

impl OutlineScene {
    /// The plan with the given plan reach, metres.
    fn new(limit: f64) -> Self {
        let (points, triangles) = base();
        let none = TerrainRing {
            offset: 0,
            count: 0,
        };
        Self {
            points,
            triangles,
            rows: rows(counts_in(0)),
            outlines: [UnifiedOutline {
                cut: HALF,
                fill: HALF,
                limit,
                rounding: 0.0,
                tolerance: 1e-6,
                surface_points: TerrainRing {
                    offset: 0,
                    count: 12,
                },
                surface_triangles: TerrainRing {
                    offset: 0,
                    count: 6,
                },
                grades: none,
                spots: none,
                order: 0,
                effect: 3,
                first_line: 0,
                line_count: 0,
            }],
        }
    }

    fn request(&self) -> UnifiedRequest {
        UnifiedRequest {
            points: view(&self.points),
            triangles: view(&self.triangles),
            property: view(&PROPERTY),
            geometry: view(&PAVED),
            rings: empty(),
            parts: empty(),
            part_indices: empty(),
            stations: empty(),
            surface_indices: view(&PAVED_TRIANGLES),
            sources: empty(),
            entries: empty(),
            paths: empty(),
            masks: empty(),
            controls: empty(),
            control_tolerance: 1e-3,
            revision: 9,
            plan_hash: 0xBEEF,
            provenance: provenance(&self.rows, rows_counts(&self.rows), 7),
            outlines: view(&self.outlines),
            outline_lines: empty(),
            outline_grades: empty(),
        }
    }
}

fn outline_build(limit: f64, storage: &mut Storage) -> Result<(), (i32, String)> {
    build(&OutlineScene::new(limit).request(), storage)
}

#[test]
fn every_witness_kind_maps_to_its_documented_row() {
    use tapioca_geometry::terrain::outline::StepWitness;
    assert_eq!(
        super::super::unified::step(&StepWitness::ReachLimit {
            vertex: 5,
            position: [1.0, 2.0, 3.5],
        }),
        UnifiedStep {
            kind: 1,
            index: 5,
            position: [1.0, 2.0, 3.5],
            design: 3.5,
            residual: 0.0,
        }
    );
    assert_eq!(
        super::super::unified::step(&StepWitness::RingMismatch {
            segment: 7,
            position: [1.0, 2.0, 0.25],
            design: 1.0,
        }),
        UnifiedStep {
            kind: 2,
            index: 7,
            position: [1.0, 2.0, 0.25],
            design: 1.0,
            residual: 0.75,
        }
    );
    assert_eq!(
        super::super::unified::step(&StepWitness::StepStrip {
            position: [1.0, 2.0, 0.5],
            design: 0.8,
            residual: 0.04,
        }),
        UnifiedStep {
            kind: 3,
            index: u32::MAX,
            position: [1.0, 2.0, 0.5],
            design: 0.8,
            residual: 0.04,
        }
    );
}

fn kinds(steps: &[UnifiedStep], kind: u32) -> Vec<UnifiedStep> {
    steps
        .iter()
        .copied()
        .filter(|row| row.kind == kind)
        .collect()
}

#[test]
fn reach_limit_witnesses_locate_published_vertices_under_the_cap() {
    // A 1 m reach cannot bring a 2 m high pavement down at 1:2: reach-limit vertices.
    let mut short = Storage::new();
    assert_eq!(outline_build(1.0, &mut short), Ok(()));
    let limit = tapioca_geometry::terrain::outline::STEP_WITNESS_LIMIT;
    assert!(
        short.meta.outline_unreached as usize > limit,
        "{}",
        short.meta.outline_unreached
    );
    assert_eq!(
        short.counts.steps, limit,
        "the count is the retained rows, not the observations"
    );
    assert_eq!(kinds(&short.steps, 1).len(), limit);
    for row in &short.steps {
        let at = row.index as usize * 3;
        let published = short.points.get(at..at + 3).unwrap_or_default();
        assert_eq!(published, row.position.as_slice(), "{row:?}");
        assert_eq!((row.design, row.residual), (row.position[2], 0.0));
    }
}

#[test]
fn ring_and_strip_witnesses_publish_in_kernel_order_on_the_retained_ground() {
    // A 10 m reach meets the ground 4 m out, past the property ring 3 m out: ring mismatches.
    let mut long = Storage::new();
    assert_eq!(outline_build(10.0, &mut long), Ok(()));
    let limit = tapioca_geometry::terrain::outline::STEP_WITNESS_LIMIT;
    assert_eq!(long.meta.outline_unreached, 0);
    assert!(
        long.meta.outline_clipped as usize > limit,
        "{}",
        long.meta.outline_clipped
    );
    let strips = kinds(&long.steps, 3);
    let ring = kinds(&long.steps, 2);
    assert!(!strips.is_empty() && strips.len() <= limit);
    assert_eq!(ring.len(), limit);
    assert_eq!(long.counts.steps, strips.len() + ring.len());
    // Mesher strips precede ring samples; each keeps its own observation order.
    assert!(
        long.steps
            .get(..strips.len())
            .unwrap_or_default()
            .iter()
            .all(|row| row.kind == 3)
    );
    let segments = long.mappings.iter().filter(|row| row.kind == 17).count();
    for row in &ring {
        assert!((row.index as usize) < segments, "{row:?}");
        let [x, y, z] = row.position;
        assert!(
            [x, y]
                .iter()
                .any(|v| v.abs() < 1e-12 || (v - 8.0).abs() < 1e-12),
            "on the property ring: {row:?}"
        );
        assert_eq!(z, 0.0, "sampled on the retained ground");
        assert!(row.design > 0.0);
        assert_eq!(row.residual.to_bits(), (row.design - z).to_bits());
    }
    for row in &strips {
        assert_eq!(
            row.index,
            u32::MAX,
            "a classification event, not a triangle"
        );
        assert!(row.residual > 0.0 && row.design.is_finite());
    }
    // Deterministic: a second query/fill publishes the same rows.
    let mut again = Storage::new();
    assert_eq!(outline_build(10.0, &mut again), Ok(()));
    assert_eq!(again.steps, long.steps);
}

#[test]
fn a_short_step_capacity_refuses_before_any_write() {
    let scene = OutlineScene::new(10.0);
    let request = scene.request();
    let mut storage = Storage::new();
    assert_eq!(build(&request, &mut storage), Ok(()));
    assert!(storage.counts.steps > 0);
    storage.size();
    storage.steps.pop();
    storage.meta = UnifiedMeta::default();
    storage.counts = UnifiedCounts::default();
    let before = storage.clone();
    let fill = storage.fill();
    let mut buffer = [0_u8; 256];
    // SAFETY: live immutable request pools; every output array is exclusive storage.
    let status = unsafe {
        tapioca_terrain_unified_build(
            &raw const request,
            &raw const fill,
            buffer.as_mut_ptr(),
            buffer.len(),
        )
    };
    assert_ne!(status, 0);
    assert!(
        message(&buffer).contains("TerrainOutputTooSmall"),
        "{}",
        message(&buffer)
    );
    assert_eq!(storage, before, "nothing is written on refusal");
    // Query mode takes no step storage.
    let mut counts = UnifiedCounts::default();
    let mut meta = UnifiedMeta::default();
    let (mut site, mut context) = (
        TerrainHybridVolumes::default(),
        TerrainHybridVolumes::default(),
    );
    let mut steps = [UnifiedStep::default()];
    let mut query = Storage::query(&mut counts, &mut meta, &mut site, &mut context);
    query.steps = steps.as_mut_ptr();
    query.step_capacity = steps.len();
    // SAFETY: as above.
    let status = unsafe {
        tapioca_terrain_unified_build(
            &raw const request,
            &raw const query,
            buffer.as_mut_ptr(),
            buffer.len(),
        )
    };
    assert_ne!(status, 0);
    assert!(
        message(&buffer).contains("TerrainOutputMode"),
        "{}",
        message(&buffer)
    );
    assert_eq!(counts, UnifiedCounts::default());
}
