//! S3b.3 terrain field/edit/volume exports against their safe kernels.
use super::*;
use tapioca_geometry::terrain::edit as edit_kernel;
use tapioca_geometry::terrain::field::Field;

const SENTINEL: f64 = -9.5;

/// `side x side` vertices at 1 m, heights `z(x, y)`.
fn grid(side: u32, z: impl Fn(f64, f64) -> f64) -> (Vec<f64>, Vec<u32>) {
    let mut points = Vec::new();
    for row in 0..side {
        for column in 0..side {
            let (x, y) = (f64::from(column), f64::from(row));
            points.extend([x, y, z(x, y)]);
        }
    }
    let mut triangles = Vec::new();
    for row in 0..side - 1 {
        for column in 0..side - 1 {
            let a = row * side + column;
            triangles.extend([a, a + 1, a + side + 1, a, a + side + 1, a + side]);
        }
    }
    (points, triangles)
}

fn message(error: &[u8]) -> String {
    let length = error.iter().position(|&b| b == 0).unwrap_or(error.len());
    String::from_utf8_lossy(error.get(..length).unwrap_or(&[])).into_owned()
}

#[test]
fn layouts_are_the_documented_c_records() {
    assert_eq!(size_of::<TerrainSample>(), 72);
    assert_eq!(size_of::<TerrainFieldSummary>(), 64);
    assert_eq!(size_of::<TerrainSlope>(), 24);
    assert_eq!(size_of::<TerrainDaylight>(), 40);
    assert_eq!(size_of::<TerrainEdit>(), 208);
    assert_eq!(size_of::<TerrainRing>(), 8);
    assert_eq!(size_of::<TerrainFaceOwner>(), 8);
    assert_eq!(size_of::<TerrainEditReport>(), 16);
    assert_eq!(size_of::<TerrainVolumes>(), 40);
    assert_eq!(size_of::<TerrainEditVolumes>(), 40);
    if size_of::<usize>() == 8 {
        assert_eq!(size_of::<TerrainSampleRequest>(), 56);
        assert_eq!(size_of::<TerrainSampleOutput>(), 24);
        assert_eq!(size_of::<TerrainSurfaceView>(), 32);
        assert_eq!(
            size_of::<TerrainEditRequest>(),
            120 + size_of::<ProvenanceRequest>()
        );
        assert_eq!(size_of::<TerrainEditOutput>(), 72);
        assert_eq!(size_of::<TerrainVolumeRequest>(), 176);
        assert_eq!(size_of::<TerrainVolumeOutput>(), 48);
    }
}

fn sample_run(
    request: &TerrainSampleRequest,
    samples: &mut [TerrainSample],
    summary: &mut TerrainFieldSummary,
) -> (i32, String) {
    let output = TerrainSampleOutput {
        summary: std::ptr::from_mut(summary),
        samples: samples.as_mut_ptr(),
        sample_capacity: samples.len(),
    };
    let mut error = [0_u8; 128];
    // SAFETY: live disjoint locals.
    let status = unsafe {
        tapioca_terrain_sample(
            std::ptr::from_ref(request),
            &raw const output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    (status, message(&error))
}

#[test]
fn field_queries_match_the_kernel_and_refuse_without_writes() {
    let (points, triangles) = grid(4, |x, _| x);
    let queries = [1.25, 1.5, 9.0, 9.0];
    let request = TerrainSampleRequest {
        points: view(&points),
        triangles: view(&triangles),
        queries: view(&queries),
        step: 0.25,
    };
    let marker = TerrainSample {
        height: SENTINEL,
        ..TerrainSample::default()
    };
    let mut samples = [marker; 3];
    let mut summary = TerrainFieldSummary::default();
    assert_eq!(
        sample_run(&request, &mut samples, &mut summary),
        (0, String::new())
    );
    let field = Field::new(&points, &triangles).ok();
    let expected = field.as_ref().and_then(|f| f.height_at(1.25, 1.5));
    let [on, off, tail] = samples;
    assert_eq!(Some(on.height), expected.map(|s| s.height));
    assert_eq!(Some(on.face), expected.map(|s| s.face));
    assert_eq!(
        on.flags & 0b11,
        sample::SAMPLE_ON_SURFACE | sample::SAMPLE_HAS_ASPECT
    );
    assert_eq!((on.flags >> 2) & 3, 1); // central in x
    assert!((on.slope - std::f64::consts::FRAC_PI_4).abs() < 1e-12);
    assert_eq!(off, TerrainSample::default());
    assert_eq!(tail, marker);
    assert!((summary.area - 9.0).abs() < 1e-12 && summary.faces == 18);
    let odd = [1.0, 1.0, 2.0];
    let mut untouched = [marker; 2];
    let (status, text) = sample_run(
        &TerrainSampleRequest {
            queries: view(&odd),
            ..request
        },
        &mut untouched,
        &mut summary,
    );
    assert_eq!(status, 1);
    assert!(text.starts_with("TerrainSampleShape"));
    assert_eq!(untouched, [marker; 2]);
    let (status, text) = sample_run(&request, &mut untouched[..1], &mut summary);
    assert_eq!((status, text.as_str()), (1, "TerrainOutputTooSmall"));
}

struct EditFixture {
    points: Vec<f64>,
    triangles: Vec<u32>,
    rows: Vec<id::MappingRecord>,
    edits: Vec<TerrainEdit>,
    geometry: Vec<f64>,
}

fn edit_fixture() -> EditFixture {
    let (points, triangles) = grid(5, |_, _| 0.0);
    let rows = rows(id::EntityCounts {
        vertices: 25,
        edges: 0,
        faces: 32,
        patches: 0,
    });
    let raise = TerrainEdit {
        position: [2.0, 2.0, 1.0],
        width: 0.0,
        blur: 2.0,
        max_distance: 10.0,
        strength: 1.0,
        target: 1,
        mode: 2,
        effect: 3,
        transition: 1,
        ..TerrainEdit::default()
    };
    // A second edit: lower along a curve stored in the geometry pool.
    let geometry = vec![0.0, 4.0, -0.5, 4.0, 4.0, -0.5];
    let lower = TerrainEdit {
        width: 0.0,
        blur: 1.0,
        max_distance: 10.0,
        strength: 1.0,
        target: 2,
        mode: 2,
        effect: 3,
        transition: 1,
        geometry_offset: 0,
        geometry_count: 6,
        ..TerrainEdit::default()
    };
    EditFixture {
        points,
        triangles,
        rows,
        edits: vec![raise, lower],
        geometry,
    }
}

fn edit_request(fixture: &EditFixture) -> TerrainEditRequest {
    TerrainEditRequest {
        points: view(&fixture.points),
        triangles: view(&fixture.triangles),
        edits: view(&fixture.edits),
        geometry: view(&fixture.geometry),
        surfaces: empty(),
        protected: empty(),
        clip: TerrainRing::default(),
        has_clip: 0,
        has_smoothing: 0,
        smoothing: 0.0,
        provenance: provenance(
            &fixture.rows,
            id::EntityCounts {
                vertices: 25,
                edges: 0,
                faces: 32,
                patches: 0,
            },
            5,
        ),
    }
}

struct EditOutputs {
    count: usize,
    heights: Vec<f64>,
    owners: Vec<u32>,
    faces: Vec<TerrainFaceOwner>,
    reports: Vec<TerrainEditReport>,
}

fn edit_outputs(vertices: usize, faces: usize, edits: usize) -> EditOutputs {
    EditOutputs {
        count: 7777,
        heights: vec![SENTINEL; vertices],
        owners: vec![7; vertices],
        faces: vec![TerrainFaceOwner::default(); faces],
        reports: vec![TerrainEditReport::default(); edits],
    }
}

fn edit_run(request: &TerrainEditRequest, out: &mut EditOutputs) -> (i32, String) {
    let output = TerrainEditOutput {
        face_count: &raw mut out.count,
        heights: out.heights.as_mut_ptr(),
        height_capacity: out.heights.len(),
        owners: out.owners.as_mut_ptr(),
        owner_capacity: out.owners.len(),
        faces: out.faces.as_mut_ptr(),
        face_capacity: out.faces.len(),
        reports: out.reports.as_mut_ptr(),
        report_capacity: out.reports.len(),
    };
    let mut error = [0_u8; 160];
    // SAFETY: live disjoint locals.
    let status = unsafe {
        tapioca_terrain_edit(
            std::ptr::from_ref(request),
            &raw const output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    (status, message(&error))
}

#[test]
fn edits_match_the_kernel_with_owners_faces_and_reports() {
    let fixture = edit_fixture();
    let mut out = edit_outputs(25, 96, 2);
    assert_eq!(
        edit_run(&edit_request(&fixture), &mut out),
        (0, String::new())
    );
    let point = edit_kernel::Edit {
        target: edit_kernel::Target::Point {
            position: [2.0, 2.0, 1.0],
            radius: 0.0,
        },
        mode: edit_kernel::Mode::Relative,
        effect: edit_kernel::Effect::Both,
        transition: edit_kernel::Transition::Falloff { blur: 2.0 },
        max_distance: 10.0,
        strength: 1.0,
    };
    let curve = edit_kernel::Edit {
        target: edit_kernel::Target::Curve {
            points: &fixture.geometry,
            half_width: 0.0,
        },
        transition: edit_kernel::Transition::Falloff { blur: 1.0 },
        ..point
    };
    let expected = edit_kernel::edit_terrain(&edit_kernel::EditRequest {
        points: &fixture.points,
        triangles: &fixture.triangles,
        edits: &[point, curve],
        clip: None,
        protected: &[],
        smoothing: None,
    })
    .ok();
    assert_eq!(Some(&out.heights), expected.as_ref().map(|e| &e.heights));
    let owners: Option<Vec<u32>> = expected
        .as_ref()
        .map(|e| e.owners.iter().map(|o| o.unwrap_or(u32::MAX)).collect());
    assert_eq!(Some(&out.owners), owners.as_ref());
    assert_eq!(Some(out.count), expected.as_ref().map(|e| e.faces.len()));
    assert!(
        out.count > 0
            && out
                .faces
                .get(out.count..)
                .is_some_and(|tail| tail.iter().all(|f| *f == TerrainFaceOwner::default()))
    );
    assert_eq!(
        out.reports.first().map(|r| r.moved),
        expected
            .as_ref()
            .and_then(|e| e.reports.first())
            .map(|r| r.moved)
    );
}

#[test]
fn edit_refusals_preserve_every_output() {
    let fixture = edit_fixture();
    let base = edit_request(&fixture);
    // Wrong provenance domains: an attribution (STATE) refusal.
    let mut wrong = base;
    wrong.provenance.domains = 3;
    let mut out = edit_outputs(25, 96, 2);
    let (status, _) = edit_run(&wrong, &mut out);
    assert_eq!(status, 2);
    assert!(
        out.heights
            .iter()
            .all(|h| (*h - SENTINEL).abs() < f64::EPSILON)
            && out.count == 7777
    );
    // A geometry range past the pool.
    // A fresh copy: base still points at the fixture's own edits.
    let mut bad_edits = edit_fixture().edits;
    if let Some(edit) = bad_edits.get_mut(1) {
        edit.geometry_count = 9;
    }
    let bad = TerrainEditRequest {
        edits: view(&bad_edits),
        ..base
    };
    let (status, text) = edit_run(&bad, &mut out);
    assert_eq!((status, text.as_str()), (1, "TerrainGeometryRange"));
    assert_eq!(out.owners, vec![7; 25]);
    // Too few face rows for what the edits change.
    let mut short = edit_outputs(25, 1, 2);
    let (status, text) = edit_run(&base, &mut short);
    assert_eq!((status, text.as_str()), (1, "TerrainOutputTooSmall"));
    assert!(
        short
            .heights
            .iter()
            .all(|h| (*h - SENTINEL).abs() < f64::EPSILON)
    );
}

#[test]
fn a_surface_pool_aliasing_an_output_is_refused_before_any_write() {
    let fixture = edit_fixture();
    let mut out = edit_outputs(25, 96, 2);
    // The surface's points alias the heights output.
    let surfaces = [TerrainSurfaceView {
        points: SliceView {
            data: out.heights.as_ptr(),
            count: 9,
        },
        triangles: view(fixture.triangles.get(..3).unwrap_or(&[])),
    }];
    let request = TerrainEditRequest {
        surfaces: view(&surfaces),
        ..edit_request(&fixture)
    };
    let (status, _) = edit_run(&request, &mut out);
    assert_eq!(status, 1);
    assert!(
        out.heights
            .iter()
            .all(|h| (*h - SENTINEL).abs() < f64::EPSILON)
            && out.count == 7777
    );
}

struct VolumeOutputs {
    site: TerrainVolumes,
    context: TerrainVolumes,
    cells: [u32; 2],
    count: usize,
    edits: Vec<TerrainEditVolumes>,
}

fn volume_run(request: &TerrainVolumeRequest, out: &mut VolumeOutputs) -> (i32, String) {
    let output = TerrainVolumeOutput {
        site: &raw mut out.site,
        context: &raw mut out.context,
        cells: out.cells.as_mut_ptr(),
        edit_count: &raw mut out.count,
        edits: out.edits.as_mut_ptr(),
        edit_capacity: out.edits.len(),
    };
    let mut error = [0_u8; 160];
    // SAFETY: live disjoint locals.
    let status = unsafe {
        tapioca_terrain_volume(
            std::ptr::from_ref(request),
            &raw const output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    (status, message(&error))
}

fn volume_request(points: &[f64], triangles: &[u32]) -> TerrainVolumeRequest {
    TerrainVolumeRequest {
        points: view(points),
        triangles: view(triangles),
        proposed_points: empty(),
        proposed_triangles: empty(),
        heights: empty(),
        owners: empty(),
        property: empty(),
        domain: [0.0; 4],
        plane: 2.0,
        cell: 1.0,
        proposed: 2,
        subdivisions: 1,
        has_domain: 0,
        reserved: 0,
    }
}

fn volume_outputs(edits: usize) -> VolumeOutputs {
    VolumeOutputs {
        site: TerrainVolumes::default(),
        context: TerrainVolumes::default(),
        cells: [0; 2],
        count: 99,
        edits: vec![TerrainEditVolumes::default(); edits],
    }
}

#[test]
fn volumes_publish_site_cells_and_edit_rows() {
    let (points, triangles) = grid(5, |_, _| 0.0);
    let mut out = volume_outputs(0);
    assert_eq!(
        volume_run(&volume_request(&points, &triangles), &mut out),
        (0, String::new())
    );
    assert!((out.site.fill - 32.0).abs() < 1e-9 && out.site.samples == 16);
    assert_eq!((out.cells, out.count), ([4, 4], 0));
    // Edited heights with owners: one row per owning edit, net reconciling.
    let fixture = edit_fixture();
    let mut edited = edit_outputs(25, 96, 2);
    assert_eq!(edit_run(&edit_request(&fixture), &mut edited).0, 0);
    let request = TerrainVolumeRequest {
        heights: view(&edited.heights),
        owners: view(&edited.owners),
        proposed: 3,
        cell: 0.5,
        ..volume_request(&points, &triangles)
    };
    let mut out = volume_outputs(2);
    assert_eq!(volume_run(&request, &mut out).0, 0);
    assert_eq!(out.count, 2);
    let net: f64 = out.edits.iter().map(|row| row.net).sum();
    assert!((net - out.site.net).abs() < 1e-9);
    // Too few rows: refused, records untouched.
    let mut short = volume_outputs(1);
    let (status, text) = volume_run(&request, &mut short);
    assert_eq!((status, text.as_str()), (1, "TerrainOutputTooSmall"));
    assert_eq!((short.count, short.site), (99, TerrainVolumes::default()));
}
