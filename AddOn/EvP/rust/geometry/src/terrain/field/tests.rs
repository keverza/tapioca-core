use super::*;
use std::f64::consts::{FRAC_PI_2, FRAC_PI_4, PI};

fn close(left: f64, right: f64, tolerance: f64) -> bool {
    (left - right).abs() <= tolerance
}

/// A `side x side` vertex grid at `spacing`, heights `z(x, y)`, two counter-
/// clockwise triangles per cell split along the (0,0)-(1,1) diagonal.
fn grid(side: u32, spacing: f64, z: impl Fn(f64, f64) -> f64) -> (Vec<f64>, Vec<u32>) {
    let mut points = Vec::new();
    for row in 0..side {
        for column in 0..side {
            let (x, y) = (f64::from(column) * spacing, f64::from(row) * spacing);
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

#[test]
fn a_tilted_plane_is_sampled_exactly() -> Result<(), FieldError> {
    let plane = |x: f64, y: f64| 0.3 * x - 0.2 * y + 5.0;
    let (points, triangles) = grid(11, 1.0, plane);
    let field = Field::new(&points, &triangles)?;
    for (x, y) in [(2.25, 3.5), (0.0, 0.0), (10.0, 10.0), (7.9, 0.1)] {
        let sample = field.height_at(x, y);
        assert!(
            sample.is_some_and(|s| close(s.height, plane(x, y), 1e-12)),
            "{x},{y}"
        );
        assert!(sample.is_some_and(|s| close(s.weights.iter().sum::<f64>(), 1.0, 1e-12)));
    }
    assert_eq!(field.height_at(10.5, 3.0), None);
    assert_eq!(field.height_at(f64::NAN, 3.0), None);
    let gradient = field.gradient_at(5.2, 4.7, 0.5)?;
    assert!(
        gradient.is_some_and(|g| close(g.slope[0], 0.3, 1e-12) && close(g.slope[1], -0.2, 1e-12))
    );
    assert_eq!(
        gradient.map(|g| g.method),
        Some([GradientMethod::Central; 2])
    );
    // One step outside at the east edge: one-sided in x, central in y.
    let edge = field.gradient_at(9.8, 5.0, 0.5)?;
    assert_eq!(
        edge.map(|g| g.method),
        Some([GradientMethod::OneSided, GradientMethod::Central])
    );
    assert!(edge.is_some_and(|g| close(g.slope[0], 0.3, 1e-12)));
    // Steps leaving the surface on both sides fall back to the face plane.
    let wide = field.gradient_at(5.0, 5.0, 20.0)?;
    assert_eq!(wide.map(|g| g.method), Some([GradientMethod::Face; 2]));
    assert!(wide.is_some_and(|g| close(g.slope[0], 0.3, 1e-12) && close(g.slope[1], -0.2, 1e-12)));
    let normal = field.normal_at(4.4, 4.4).unwrap_or([f64::NAN; 3]);
    let length = (0.3_f64 * 0.3 + 0.2 * 0.2 + 1.0).sqrt();
    for (got, want) in normal
        .iter()
        .zip([-0.3 / length, 0.2 / length, 1.0 / length])
    {
        assert!(close(*got, want, 1e-12));
    }
    Ok(())
}

#[test]
fn slope_and_aspect_follow_the_downslope_bearing() -> Result<(), FieldError> {
    // Rising east (z = x): 45 degrees, faces west (bearing 270 degrees).
    let (east, triangles) = grid(5, 1.0, |x, _| x);
    let field = Field::new(&east, &triangles)?;
    assert!(
        field
            .slope_at(2.0, 2.0, 0.25)?
            .is_some_and(|s| close(s, FRAC_PI_4, 1e-12))
    );
    assert!(
        field
            .aspect_at(2.0, 2.0, 0.25)?
            .is_some_and(|a| close(a, 3.0 * FRAC_PI_2, 1e-12))
    );
    // Rising north (z = y): faces south (180 degrees).
    let (north, _) = grid(5, 1.0, |_, y| 0.5 * y);
    let field = Field::new(&north, &triangles)?;
    assert!(
        field
            .aspect_at(2.0, 2.0, 0.25)?
            .is_some_and(|a| close(a, PI, 1e-12))
    );
    // Flat: no aspect, zero slope.
    let (flat, _) = grid(5, 1.0, |_, _| 3.0);
    let field = Field::new(&flat, &triangles)?;
    assert_eq!(field.aspect_at(2.0, 2.0, 0.25)?, None);
    assert_eq!(field.slope_at(2.0, 2.0, 0.25)?, Some(0.0));
    assert_eq!(field.gradient_at(2.0, 2.0, 0.0), Err(FieldError::Step));
    Ok(())
}

#[test]
fn a_shared_edge_reports_the_lowest_face_index() -> Result<(), FieldError> {
    let (points, triangles) = grid(2, 2.0, |x, y| x + y);
    let field = Field::new(&points, &triangles)?;
    // (1, 1) is on the diagonal shared by faces 0 and 1.
    assert_eq!(
        field.height_at(1.0, 1.0).map(|s| (s.face, s.height)),
        Some((0, 2.0))
    );
    assert_eq!(field.height_at(0.5, 1.5).map(|s| s.face), Some(1));
    assert_eq!(field.corners(0.5, 1.5), Some([0, 3, 2]));
    Ok(())
}

#[test]
fn only_upward_faces_are_the_surface() -> Result<(), FieldError> {
    // A closed unit-square box from z = 0 to z = 2: top up, bottom down, sides vertical.
    let points = [
        0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 1.0, 0.0, //
        0.0, 0.0, 2.0, 1.0, 0.0, 2.0, 1.0, 1.0, 2.0, 0.0, 1.0, 2.0,
    ];
    let triangles = [
        4, 5, 6, 4, 6, 7, // top (up)
        0, 2, 1, 0, 3, 2, // bottom (down)
        0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5, // sides
        2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7,
    ];
    let field = Field::new(&points, &triangles)?;
    let coverage = field.coverage();
    assert_eq!((coverage.faces, coverage.excluded), (2, 10));
    assert!(close(coverage.area, 1.0, 1e-12));
    assert!(
        field
            .height_at(0.5, 0.25)
            .is_some_and(|s| close(s.height, 2.0, 0.0) && s.face == 0)
    );
    assert_eq!(field.bounds().min, [0.0, 0.0, 2.0]);
    assert_eq!(field.boundary().len(), 4);
    // Downward faces only: no surface.
    assert_eq!(
        Field::new(&points, &triangles[6..12]).map(|_| ()),
        Err(FieldError::Empty)
    );
    Ok(())
}

#[test]
fn the_bilinear_raster_reproduces_a_plane_and_stops_at_the_edge() -> Result<(), FieldError> {
    let plane = |x: f64, y: f64| 2.0 * x + 0.5 * y - 1.0;
    let (points, triangles) = grid(6, 2.0, plane);
    let field = Field::new(&points, &triangles)?;
    let raster = field.raster([0.0, 0.0], 0.5, [21, 21])?;
    assert_eq!(raster.heights.len(), 441);
    for (x, y) in [(1.3, 2.7), (0.0, 0.0), (10.0, 10.0), (9.99, 0.01)] {
        assert!(
            raster
                .bilinear(x, y)
                .is_some_and(|z| close(z, plane(x, y), 1e-12)),
            "{x},{y}"
        );
    }
    assert_eq!(raster.bilinear(10.1, 3.0), None);
    let outside = field.raster([8.0, 8.0], 1.0, [4, 4])?;
    assert!(
        outside
            .bilinear(9.5, 9.5)
            .is_some_and(|z| close(z, plane(9.5, 9.5), 1e-12))
    );
    assert_eq!(outside.bilinear(10.5, 10.5), None); // its node at 11 is off the surface
    assert_eq!(
        field.raster([0.0, 0.0], 0.0, [2, 2]).map(|_| ()),
        Err(FieldError::Step)
    );
    Ok(())
}

#[test]
fn malformed_meshes_are_refused_by_name() {
    assert_eq!(
        Field::new(&[0.0, 1.0], &[]).map(|_| ()),
        Err(FieldError::Shape)
    );
    assert_eq!(
        Field::new(&[0.0; 9], &[0, 1]).map(|_| ()),
        Err(FieldError::Shape)
    );
    assert_eq!(
        Field::new(
            &[0.0, 0.0, f64::INFINITY, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0],
            &[0, 1, 2]
        )
        .map(|_| ()),
        Err(FieldError::NonFinite)
    );
    assert_eq!(
        Field::new(&[0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0], &[0, 1, 3]).map(|_| ()),
        Err(FieldError::Index)
    );
    for error in [
        FieldError::Shape,
        FieldError::NonFinite,
        FieldError::Index,
        FieldError::Empty,
        FieldError::Step,
    ] {
        assert!(error.message().starts_with("Field"));
    }
}
