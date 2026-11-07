//! #149 T2: the faces' creases as breaklines, measured against the exact faces.

use super::*;
use crate::terrain::outline::toe::Toe;

/// An L of pavement: leg A along x at y in [-2, 2] from x = 0 to 20, leg B along y at x in
/// [16, 20] from y = 2 to 20, falling from 12 to 11. Its inside corner at (16, 2) is where
/// leg A's north face and leg B's west face meet in a crease.
const L_SURFACE: [f64; 21] = [
    0.0, -2.0, 12.0, 20.0, -2.0, 12.0, 20.0, 2.0, 12.0, 0.0, 2.0, 12.0, 16.0, 2.0, 12.0, 16.0,
    20.0, 11.0, 20.0, 20.0, 11.0,
];
const L_TRIANGLES: [u32; 15] = [0, 1, 2, 0, 2, 4, 0, 4, 3, 4, 2, 6, 4, 6, 5];
const L_REGION: [f64; 12] = [
    -10.0, -15.0, 0.0, 40.0, -15.0, 0.0, 40.0, 28.0, 0.0, -10.0, 28.0, 0.0,
];

/// The L graded on ground tilted 5 % along x, with the toe and, as `creases` asks, the
/// creases; and the exact faces, for measuring.
fn graded(
    rounding: f64,
    creases: bool,
) -> Result<(Outlined, Faces<'static>, &'static Field), OutlineError> {
    let (mut points, triangles) = base();
    for p in points.as_chunks_mut::<3>().0 {
        p[2] = 0.05f64.mul_add(p[0], 9.0);
    }
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
            region: &L_REGION,
            surface_points: &L_SURFACE,
            surface_triangles: &L_TRIANGLES,
            breaklines: &[],
            spots: &[],
            surface_grades: &[],
            refine: None,
            grading,
        },
        Some(Toe {
            creases,
            ..Toe::DEFAULT
        }),
    )?;
    // Leaked: a test-only 'static borrow.
    let base: &'static Field = Box::leak(Box::new(
        Field::new(&points, &triangles).map_err(|_| OutlineError::Field)?,
    ));
    let surface: &'static Field = Box::leak(Box::new(
        Field::new(&L_SURFACE, &L_TRIANGLES).map_err(|_| OutlineError::Surface)?,
    ));
    let uniform = SurfaceGrade {
        cut: 0.5,
        fill: 0.5,
        rounding,
    };
    let edges: &'static [Edge] =
        Box::leak(outline(&L_SURFACE, &L_TRIANGLES, &[], uniform).into_boxed_slice());
    Ok((built, Faces::new(base, surface, edges, grading), base))
}

/// Over the inside corner's quadrant: the largest gap between the mesh and the exact faces
/// within half a metre of the crease and more than a metre (the rounding) from the pavement,
/// and how many inside triangles the crease passes through (their samples are won by two
/// creased faces). Within a rounding of the pavement the faces' rounded tops curve whatever the
/// crease does; the editor's offset rings at 0 to 1 m follow them, and this scene sends none.
fn measure(built: &Outlined, faces: &Faces<'_>, base: &Field) -> (f64, usize) {
    let mesh = Field::new(&built.points, &built.mesh.triangles).ok();
    let winner = |x: f64, y: f64| {
        let ground = base.height_at(x, y)?.height;
        faces.winner(ground, x, y).map(|w| (w, ground))
    };
    let creased = |p: [f64; 2], q: [f64; 2]| match (winner(p[0], p[1]), winner(q[0], q[1])) {
        (Some((a, ground)), Some((b, _))) => {
            a.cut == b.cut
                && a.index != b.index
                && faces.creased(ground, p, a.cut, [a.index, b.index])
        }
        _ => false,
    };
    let step = 0.05;
    let mut gap = 0.0_f64;
    for i in 0..=200 {
        for j in 0..=200 {
            let (x, y) = (
                f64::from(i).mul_add(step, 6.0),
                f64::from(j).mul_add(step, 2.0),
            );
            let near = [[0.5, 0.0], [-0.5, 0.0], [0.0, 0.5], [0.0, -0.5]]
                .iter()
                .any(|d| creased([x, y], [x + d[0], y + d[1]]));
            if !near || (y - 2.0).min(16.0 - x) <= 1.0 {
                continue;
            }
            if let (Some(exact), Some(sample)) = (
                faces.height(x, y).map(|(h, _)| h),
                mesh.as_ref().and_then(|m| m.height_at(x, y)),
            ) {
                gap = gap.max((exact - sample.height).abs());
            }
        }
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
        let samples: Vec<[f64; 2]> = [[0.6, 0.2, 0.2], [0.2, 0.6, 0.2], [0.2, 0.2, 0.6]]
            .iter()
            .map(|w| {
                [
                    w[0] * pa[0] + w[1] * pb[0] + w[2] * pc[0],
                    w[0] * pa[1] + w[1] * pb[1] + w[2] * pc[1],
                ]
            })
            .collect();
        if let [p, q, r] = samples.as_slice()
            && (creased(*p, *q) || creased(*q, *r) || creased(*r, *p))
        {
            crossed += 1;
        }
    }
    (gap, crossed)
}

#[test]
fn an_inside_corner_crease_is_a_breakline_and_the_mesh_folds_along_it() -> Result<(), OutlineError>
{
    for rounding in [0.0, 1.0] {
        let (before, faces, base) = graded(rounding, false)?;
        let (after, _, _) = graded(rounding, true)?;
        let (gap0, crossed0) = measure(&before, &faces, base);
        let (gap1, crossed1) = measure(&after, &faces, base);
        eprintln!(
            "rounding {rounding}: toe only ({} lines) gap {gap0:.4} m, {crossed0} triangles cross the crease, \
             {} vertices; with the creases ({} lines) gap {gap1:.4} m, {crossed1} cross, {} vertices",
            before.toe,
            before.points.len() / 3,
            after.toe,
            after.points.len() / 3
        );
        assert!(after.toe > before.toe, "the crease went into the mesh");
        assert!(crossed1 * 4 <= crossed0, "{crossed1} vs {crossed0}");
        assert!(gap1 < gap0, "{gap1} vs {gap0}");
        // The T2 gate: within half a metre of the crease the mesh is within 1 cm of the faces.
        // A sharp crease is a kink on its line; a rounded one is followed across its width
        // (one line down its middle left 8 cm at this 1 m rounding).
        assert!(gap1 <= 0.01, "crease error {gap1}");
    }
    Ok(())
}
