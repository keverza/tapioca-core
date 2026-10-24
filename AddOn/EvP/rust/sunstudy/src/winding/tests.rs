//! Mirrors `tests/cpp/test_sunstudywinding.cpp`.

use super::*;

struct Mesh {
    vertices: Vec<f64>,
    triangles: Vec<u32>,
}

fn make_box(size: f64, origin: [f64; 3]) -> Mesh {
    let [x, y, z] = origin;
    let s = size;
    Mesh {
        vertices: vec![
            x,
            y,
            z,
            x + s,
            y,
            z,
            x + s,
            y + s,
            z,
            x,
            y + s,
            z,
            x,
            y,
            z + s,
            x + s,
            y,
            z + s,
            x + s,
            y + s,
            z + s,
            x,
            y + s,
            z + s,
        ],
        triangles: vec![
            0, 3, 2, 0, 2, 1, // bottom, -Z
            4, 5, 6, 4, 6, 7, // top, +Z
            0, 1, 5, 0, 5, 4, // -Y
            1, 2, 6, 1, 6, 5, // +X
            2, 3, 7, 2, 7, 6, // +Y
            3, 0, 4, 3, 4, 7, // -X
        ],
    }
}

fn flip_all(triangles: &mut [u32]) {
    for tri in triangles.as_chunks_mut::<3>().0 {
        tri.swap(1, 2);
    }
}

fn orient(mesh: &Mesh, groups: Option<&[u32]>) -> (Vec<u32>, WindingReport) {
    let result = orient_outward(
        &mesh.vertices,
        &mesh.triangles,
        groups,
        DEFAULT_WELD_TOLERANCE,
    );
    assert!(result.is_ok());
    result.unwrap_or_default()
}

fn all_faces(mesh: &Mesh) -> Vec<usize> {
    (0..mesh.triangles.len() / 3).collect()
}

fn merged(a: &Mesh, b: &Mesh) -> Mesh {
    let base = u32::try_from(a.vertices.len() / 3).unwrap_or(0);
    let mut vertices = a.vertices.clone();
    vertices.extend_from_slice(&b.vertices);
    let mut triangles = a.triangles.clone();
    triangles.extend(b.triangles.iter().map(|i| base + i));
    Mesh {
        vertices,
        triangles,
    }
}

#[test]
fn empty_input_is_handled() {
    let result = orient_outward(&[], &[], None, DEFAULT_WELD_TOLERANCE);
    assert_eq!(result, Ok((Vec::new(), WindingReport::default())));
}

#[test]
fn an_outward_box_has_positive_volume() {
    let mesh = make_box(2.0, [0.0; 3]);
    let volume = signed_volume(&mesh.vertices, &mesh.triangles, &all_faces(&mesh));
    assert!((volume - 8.0).abs() < 1e-9);
}

#[test]
fn an_inward_box_has_negative_volume() {
    let mut mesh = make_box(2.0, [0.0; 3]);
    flip_all(&mut mesh.triangles);
    let volume = signed_volume(&mesh.vertices, &mesh.triangles, &all_faces(&mesh));
    assert!((volume + 8.0).abs() < 1e-9);
}

#[test]
fn an_already_outward_box_is_left_alone() {
    let mesh = make_box(2.0, [0.0; 3]);
    let (oriented, report) = orient(&mesh, None);
    assert_eq!(oriented, mesh.triangles);
    assert_eq!((report.groups, report.closed, report.flipped), (1, 1, 0));
}

#[test]
fn an_inward_box_is_flipped() {
    let mut mesh = make_box(2.0, [0.0; 3]);
    flip_all(&mut mesh.triangles);
    let (oriented, report) = orient(&mesh, None);
    assert_eq!(
        (report.closed, report.flipped, report.flipped_triangles),
        (1, 1, 12)
    );
    let volume = signed_volume(&mesh.vertices, &oriented, &all_faces(&mesh));
    assert!((volume - 8.0).abs() < 1e-9);
}

#[test]
fn flipping_is_idempotent() {
    let mut mesh = make_box(2.0, [0.0; 3]);
    flip_all(&mut mesh.triangles);
    let (once, _) = orient(&mesh, None);
    let again = Mesh {
        vertices: mesh.vertices.clone(),
        triangles: once.clone(),
    };
    let (twice, report) = orient(&again, None);
    assert_eq!(once, twice);
    assert_eq!(report.flipped, 0);
}

#[test]
fn an_open_surface_is_never_flipped() {
    let mesh = Mesh {
        vertices: vec![0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0],
        triangles: vec![0, 2, 1],
    };
    let (oriented, report) = orient(&mesh, None);
    assert_eq!(oriented, mesh.triangles);
    assert_eq!((report.groups, report.closed, report.flipped), (1, 0, 0));
}

#[test]
fn a_box_missing_a_face_is_not_closed_so_is_left_alone() {
    let mut mesh = make_box(2.0, [0.0; 3]);
    flip_all(&mut mesh.triangles);
    mesh.triangles.truncate(mesh.triangles.len() - 3);
    let (oriented, report) = orient(&mesh, None);
    assert_eq!(oriented, mesh.triangles);
    assert_eq!((report.closed, report.flipped), (0, 0));
}

#[test]
fn each_element_is_judged_separately() {
    let good = make_box(2.0, [0.0; 3]);
    let mut bad = make_box(2.0, [10.0, 0.0, 0.0]);
    flip_all(&mut bad.triangles);
    let mesh = merged(&good, &bad);
    let groups: Vec<u32> = (0..24).map(|f| u32::from(f >= 12)).collect();
    let (oriented, report) = orient(&mesh, Some(&groups));
    assert_eq!(
        (
            report.groups,
            report.closed,
            report.flipped,
            report.flipped_triangles
        ),
        (2, 2, 1, 12)
    );
    assert_eq!(oriented.get(..36), mesh.triangles.get(..36));
}

#[test]
fn two_elements_merged_without_groups_read_as_one_group() {
    let mesh = merged(&make_box(2.0, [0.0; 3]), &make_box(2.0, [10.0, 0.0, 0.0]));
    let (_, report) = orient(&mesh, None);
    assert_eq!((report.groups, report.flipped), (1, 0));
}

#[test]
fn the_sign_survives_survey_coordinates() {
    let mut mesh = make_box(2.0, [624_000.0, 6_172_000.0, 120.0]);
    flip_all(&mut mesh.triangles);
    let (_, report) = orient(&mesh, None);
    assert_eq!((report.closed, report.flipped), (1, 1));
}

#[test]
fn an_outward_box_at_survey_coordinates_is_still_left_alone() {
    let mesh = make_box(2.0, [624_000.0, 6_172_000.0, 120.0]);
    let (oriented, report) = orient(&mesh, None);
    assert_eq!((report.closed, report.flipped), (1, 0));
    assert_eq!(oriented, mesh.triangles);
}

#[test]
fn adjacency_is_by_position_not_by_index() {
    let mut mesh = make_box(2.0, [0.0; 3]);
    flip_all(&mut mesh.triangles);
    let mut soup = Mesh {
        vertices: Vec::new(),
        triangles: Vec::new(),
    };
    for &index in &mesh.triangles {
        let start = usize::try_from(index).unwrap_or(0) * 3;
        soup.triangles
            .push(u32::try_from(soup.vertices.len() / 3).unwrap_or(0));
        soup.vertices
            .extend_from_slice(mesh.vertices.get(start..start + 3).unwrap_or_default());
    }
    let (_, report) = orient(&soup, None);
    assert_eq!((report.closed, report.flipped), (1, 1));
}

#[test]
fn out_of_range_indices_do_not_crash_or_prove_closed() {
    let mesh = Mesh {
        vertices: vec![0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0],
        triangles: vec![0, 1, 77],
    };
    let (oriented, report) = orient(&mesh, None);
    assert_eq!(oriented, mesh.triangles);
    assert_eq!((report.groups, report.flipped), (0, 0));
}

#[test]
fn a_short_group_list_is_refused() {
    let mesh = make_box(2.0, [0.0; 3]);
    let result = orient_outward(
        &mesh.vertices,
        &mesh.triangles,
        Some(&[0; 3]),
        DEFAULT_WELD_TOLERANCE,
    );
    assert_eq!(result, Err(Error::GroupCount));
}

#[test]
fn a_bad_tolerance_falls_back_to_the_default() {
    let mut mesh = make_box(2.0, [0.0; 3]);
    flip_all(&mut mesh.triangles);
    for tolerance in [0.0, -1.0, f64::NAN] {
        let result = orient_outward(&mesh.vertices, &mesh.triangles, None, tolerance);
        assert!(result.is_ok_and(|(_, report)| report.flipped == 1));
    }
}
