//! #149 T4: the edge index changes no height, bit for bit.

use super::*;

#[test]
fn the_edge_index_gives_the_full_scans_heights_bit_for_bit() -> Result<(), OutlineError> {
    // The inside-corner pavement on ground rising 10 % along x, so its faces
    // are fill to the west and cut to the east, with rounded tops and creases.
    let surface = [
        0.0, -2.0, 12.0, 20.0, -2.0, 12.0, 20.0, 2.0, 12.0, 0.0, 2.0, 12.0, 16.0, 2.0, 12.0, 16.0,
        20.0, 11.0, 20.0, 20.0, 11.0,
    ];
    let surface_triangles = [0_u32, 1, 2, 0, 2, 4, 0, 4, 3, 4, 2, 6, 4, 6, 5];
    let (mut points, triangles) = base();
    for p in points.as_chunks_mut::<3>().0 {
        p[2] = 0.1f64.mul_add(p[0], 9.0);
    }
    let ground = Field::new(&points, &triangles).map_err(|_| OutlineError::Field)?;
    let paved = Field::new(&surface, &surface_triangles).map_err(|_| OutlineError::Surface)?;
    for rounding in [0.0, 1.0] {
        let uniform = SurfaceGrade {
            cut: 0.5,
            fill: 1.0 / 3.0,
            rounding,
        };
        let edges = outline(&surface, &surface_triangles, &[], uniform);
        let grading = Grading {
            cut: HALF,
            fill: HALF,
            limit: 6.0,
            rounding,
            effect: Effect::Both,
            tolerance: 1e-6,
        };
        let indexed = Faces::new(&ground, &paved, &edges, grading);
        let mut scanned = Faces::new(&ground, &paved, &edges, grading);
        scanned.reach = crate::terrain::outline::reach::Reach::everything(&edges);
        let (mut moved, mut short) = (0, 0);
        for i in 0..160 {
            for j in 0..120 {
                let (x, y) = (
                    f64::from(i).mul_add(0.37, -19.9),
                    f64::from(j).mul_add(0.43, -25.6),
                );
                let (a, b) = (indexed.height(x, y), scanned.height(x, y));
                assert_eq!(
                    a.map(|(h, s)| (h.to_bits(), s)),
                    b.map(|(h, s)| (h.to_bits(), s)),
                    "({x}, {y}) rounding {rounding}"
                );
                if let (Some((h, s)), Some(g)) = (a, ground.height_at(x, y)) {
                    moved += usize::from(h.to_bits() != g.height.to_bits());
                    short += usize::from(s);
                }
            }
        }
        // The comparison covered faces, creases and the limit's step.
        assert!(moved > 1000 && short > 0, "{moved} moved, {short} short");
    }
    Ok(())
}

#[test]
fn a_long_winding_pavement_prunes_without_changing_a_height() -> Result<(), OutlineError> {
    // A 3 m strip winding along y = 3 sin(x / 6) and falling 5 %, sampled every
    // half metre: about 160 outline edges, most of them far from any point,
    // on ground rising 10 % along x (fill to the west, cut to the east).
    let mut surface = Vec::new();
    let mut surface_triangles = Vec::new();
    let mut section = 0_u32;
    let mut along = 0.0_f64;
    while along <= 40.0 {
        let (middle, level) = (3.0 * (along / 6.0).sin(), 0.05f64.mul_add(-along, 12.0));
        surface.extend([along, middle - 1.5, level, along, middle + 1.5, level]);
        if section > 0 {
            let [right, left, next_right, next_left] = [
                2 * section - 2,
                2 * section - 1,
                2 * section,
                2 * section + 1,
            ];
            surface_triangles.extend([right, next_right, next_left, right, next_left, left]);
        }
        section += 1;
        along += 0.5;
    }
    let (mut points, triangles) = base();
    for p in points.as_chunks_mut::<3>().0 {
        p[2] = 0.1f64.mul_add(p[0], 9.0);
    }
    let ground = Field::new(&points, &triangles).map_err(|_| OutlineError::Field)?;
    let paved = Field::new(&surface, &surface_triangles).map_err(|_| OutlineError::Surface)?;
    let uniform = SurfaceGrade {
        cut: 0.5,
        fill: 1.0 / 3.0,
        rounding: 1.0,
    };
    let edges = outline(&surface, &surface_triangles, &[], uniform);
    assert!(edges.len() > 150, "{} edges", edges.len());
    let grading = Grading {
        cut: HALF,
        fill: HALF,
        limit: 8.0,
        rounding: 1.0,
        effect: Effect::Both,
        tolerance: 1e-6,
    };
    let indexed = Faces::new(&ground, &paved, &edges, grading);
    let mut scanned = Faces::new(&ground, &paved, &edges, grading);
    scanned.reach = crate::terrain::outline::reach::Reach::everything(&edges);
    let mut moved = 0;
    for i in 0..140 {
        for j in 0..100 {
            let (x, y) = (
                f64::from(i).mul_add(0.41, -15.3),
                f64::from(j).mul_add(0.29, -14.1),
            );
            let (a, b) = (indexed.height(x, y), scanned.height(x, y));
            assert_eq!(
                a.map(|(h, s)| (h.to_bits(), s)),
                b.map(|(h, s)| (h.to_bits(), s)),
                "({x}, {y})"
            );
            if let (Some((h, _)), Some(g)) = (a, ground.height_at(x, y)) {
                moved += usize::from(h.to_bits() != g.height.to_bits());
            }
        }
    }
    assert!(moved > 2000, "{moved} moved");
    Ok(())
}
