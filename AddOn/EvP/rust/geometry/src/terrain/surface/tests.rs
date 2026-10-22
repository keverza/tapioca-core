use super::*;

fn solid() -> (Vec<f64>, Vec<u32>) {
    // Duplicate top corners as a render mesh with a normal/material seam.
    (
        vec![
            0., 0., 1., 10., 0., 1., 10., 10., 2., 0., 10., 1., 0., 0., 0., 10., 0., 0., 10., 10.,
            0., 0., 10., 0., 0., 0., 1., 10., 10., 2.,
        ],
        vec![0, 1, 2, 8, 9, 3, 4, 6, 5, 4, 7, 6, 4, 5, 1, 4, 1, 0],
    )
}

#[test]
fn solid_sides_bottom_and_seams_keep_complete_native_support() -> Result<(), SurfaceError> {
    let (points, triangles) = solid();
    let surface = extract(&points, &triangles)?;
    assert_eq!(
        surface.points,
        points.get(..12).ok_or(SurfaceError::Topology)?
    );
    assert_eq!(surface.triangles, [0, 1, 2, 0, 2, 3]);
    assert_eq!(
        surface.vertices,
        [[0, 0], [1, 1], [2, 2], [3, 3], [0, 8], [2, 9]]
    );
    assert_eq!(surface.faces, [0, 1]);
    assert_eq!(surface, extract(&points, &triangles)?);
    Ok(())
}

#[test]
fn different_heights_and_duplicate_faces_refuse() {
    let (mut points, triangles) = solid();
    if let Some(z) = points.get_mut(26) {
        *z = 1.1;
    }
    assert_eq!(
        extract(&points, &triangles),
        Err(SurfaceError::MultipleHeights)
    );
    let (points, mut triangles) = solid();
    triangles.extend([0, 1, 2]);
    assert_eq!(extract(&points, &triangles), Err(SurfaceError::Topology));
}

#[test]
fn survey_coordinates_keep_holes_and_nearby_distinct_vertices() -> Result<(), SurfaceError> {
    // A frame with a hole; selection keeps its topology, never fills the hole.
    let mut points = vec![
        0., 0., 0., 10., 0., 0., 10., 10., 0., 0., 10., 0., 3., 3., 0., 7., 3., 0., 7., 7., 0., 3.,
        7., 0.,
    ];
    let triangles = [
        0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7,
    ];
    for point in points.as_chunks_mut::<3>().0 {
        point[0] += 1_000_000.;
        point[1] -= 2_000_000.;
    }
    let output = extract(&points, &triangles)?;
    assert_eq!(output.points, points);
    assert_eq!(output.triangles, triangles);
    let field = Field::new(&output.points, &output.triangles).map_err(SurfaceError::Field)?;
    assert!(field.height_at(1_000_005., -1_999_995.).is_none());
    let (mut points, triangles) = solid();
    if let Some(x) = points.get_mut(24) {
        *x = 0.0_f64.next_up();
    }
    assert_eq!(extract(&points, &triangles)?.points.len(), 15);
    Ok(())
}

#[test]
fn malformed_empty_and_nonfinite_refuse() {
    assert!(extract(&[], &[]).is_err());
    let (mut points, triangles) = solid();
    assert!(extract(&points, &[0, 1, 99]).is_err());
    assert!(extract(&points, &[0, 1]).is_err());
    if let Some(x) = points.first_mut() {
        *x = f64::NAN;
    }
    assert!(extract(&points, &triangles).is_err());
}
