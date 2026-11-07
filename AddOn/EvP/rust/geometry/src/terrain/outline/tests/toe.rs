//! #149 T2: the toe as breaklines, measured against the exact faces.

use super::*;

/// The editor's arrangement on tilted ground (5 % along x, so the toe slants
/// across the half-metre rings and the base triangles): rings at 0..1 m by a
/// quarter and 2..5 m by a metre, as the outline lane's caller sends them.
fn graded(
    rounding: f64,
    toe: Option<crate::terrain::outline::toe::Toe>,
) -> Result<(Outlined, Vec<f64>, Faces<'static>), OutlineError> {
    let (mut points, triangles) = base();
    for p in points.as_chunks_mut::<3>().0 {
        p[2] = 0.05f64.mul_add(p[0], 9.0);
    }
    let rings: Vec<Vec<f64>> = [0.0, 0.25, 0.5, 0.75, 1.0, 2.0, 3.0, 4.0, 5.0]
        .into_iter()
        .flat_map(ring)
        .collect();
    let lines: Vec<&[f64]> = rings.iter().map(Vec::as_slice).collect();
    let grading = Grading {
        cut: HALF,
        fill: HALF,
        limit: 10.0,
        rounding,
        effect: Effect::Both,
        tolerance: 1e-6,
    };
    let built = build_with(
        &OutlineRequest {
            points: &points,
            triangles: &triangles,
            region: &REGION,
            surface_points: &PAVED,
            surface_triangles: &PAVED_TRIANGLES,
            breaklines: &lines,
            spots: &[],
            surface_grades: &[],
            refine: None,
            grading,
        },
        toe,
    )?;
    // The exact faces, for measuring (leaked: a test-only 'static borrow).
    let base: &'static Field = Box::leak(Box::new(
        Field::new(&points, &triangles).map_err(|_| OutlineError::Field)?,
    ));
    let surface: &'static Field = Box::leak(Box::new(
        Field::new(&PAVED, &PAVED_TRIANGLES).map_err(|_| OutlineError::Surface)?,
    ));
    let uniform = SurfaceGrade {
        cut: 0.5,
        fill: 0.5,
        rounding,
    };
    let edges: &'static [Edge] =
        Box::leak(outline(&PAVED, &PAVED_TRIANGLES, &[], uniform).into_boxed_slice());
    Ok((built, points, Faces::new(base, surface, edges, grading)))
}

/// Along transects across the north face: the exact toe, bisected to 1e-7 m;
/// the largest gap between the mesh and the exact faces within half a metre of
/// it; and how many inside triangles the exact toe passes through (samples of
/// a triangle disagree on moved).
fn measure(built: &Outlined, faces: &Faces<'_>) -> (f64, usize) {
    let mesh = Field::new(&built.points, &built.mesh.triangles).ok();
    let moved = |x: f64, y: f64| {
        faces
            .height(x, y)
            .map(|(h, _)| (h - 0.05f64.mul_add(x, 9.0)).abs() > 1e-9)
    };
    let mut gap = 0.0_f64;
    let mut x = 1.0;
    while x < 39.0 {
        let (mut lo, mut hi) = (2.5, 9.0);
        if moved(x, lo) == Some(true) && moved(x, hi) == Some(false) {
            for _ in 0..60 {
                let mid = lo.midpoint(hi);
                if moved(x, mid) == Some(true) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            let mut s = -0.5;
            while s <= 0.5 {
                let y = lo + s;
                if let (Some(exact), Some(sample)) = (
                    faces.height(x, y).map(|(h, _)| h),
                    mesh.as_ref().and_then(|m| m.height_at(x, y)),
                ) {
                    gap = gap.max((exact - sample.height).abs());
                }
                s += 0.02;
            }
        }
        x += 0.37;
    }
    let corners = plan::vertices(&built.points);
    let mut crossed = 0;
    for (&[a, b, c], _) in built
        .mesh
        .triangles
        .as_chunks::<3>()
        .0
        .iter()
        .zip(&built.mesh.inside)
        .filter(|(_, inside)| **inside)
    {
        let [Some(&pa), Some(&pb), Some(&pc)] = [a, b, c].map(|v| corners.get(v as usize)) else {
            continue;
        };
        // Strictly inside the triangle only: a toe along an edge is not crossed.
        let samples: Vec<Option<bool>> = [
            [0.6, 0.2, 0.2],
            [0.2, 0.6, 0.2],
            [0.2, 0.2, 0.6],
            [1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0],
        ]
        .iter()
        .map(|w| {
            let x = w[0] * pa[0] + w[1] * pb[0] + w[2] * pc[0];
            let y = w[0] * pa[1] + w[1] * pb[1] + w[2] * pc[1];
            moved(x, y)
        })
        .collect();
        if samples.contains(&Some(true)) && samples.contains(&Some(false)) {
            crossed += 1;
        }
    }
    (gap, crossed)
}

#[test]
fn the_toe_is_a_breakline_and_the_mesh_meets_the_ground_along_it() -> Result<(), OutlineError> {
    for rounding in [0.0, 1.0] {
        let (before, _, faces) = graded(rounding, None)?;
        let (after, _, _) = graded(rounding, Some(crate::terrain::outline::toe::Toe::DEFAULT))?;
        let (gap0, crossed0) = measure(&before, &faces);
        let (gap1, crossed1) = measure(&after, &faces);
        eprintln!(
            "rounding {rounding}: rings only gap {gap0:.4} m, {crossed0} triangles cross the toe, {} vertices; \
             with the toe ({} lines) gap {gap1:.4} m, {crossed1} cross, {} vertices",
            before.points.len() / 3,
            after.toe,
            after.points.len() / 3
        );
        assert_eq!(before.toe, 0);
        assert!(after.toe > 0, "the toe went into the mesh");
        assert!(crossed1 * 10 <= crossed0, "{crossed1} vs {crossed0}");
        // The T2 gate: within half a metre of the toe the mesh is within 1 cm of the faces. A
        // sharp toe is a kink on its breakline; a rounded one is followed across its width.
        assert!(gap1 <= 0.01, "toe error {gap1}");
        assert!(gap0 > 10.0 * gap1.max(1e-4), "{gap1} vs {gap0}");
        assert_eq!(after.unreached, before.unreached);
    }
    Ok(())
}
