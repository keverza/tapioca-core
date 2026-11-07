//! #149 T3: the faces' vertical error bounded by refinement, not offset rings.

use super::*;
use crate::terrain::outline::refine::mix;

/// The ground tilted 5 % along x, so the toe wanders across the base triangles;
/// the pavement only (no offset rings) as the caller's line, refined to `refine`.
fn refined(rounding: f64, refine: Option<Refine>) -> Result<(Outlined, Vec<f64>), OutlineError> {
    refined_with(rounding, refine, &[0.0])
}

/// As [`refined`], with the pavement grown by each of `rings` as lines.
fn refined_with(
    rounding: f64,
    refine: Option<Refine>,
    rings: &[f64],
) -> Result<(Outlined, Vec<f64>), OutlineError> {
    let (mut points, triangles) = base();
    for p in points.as_chunks_mut::<3>().0 {
        p[2] = 0.05f64.mul_add(p[0], 9.0);
    }
    let outline: Vec<Vec<f64>> = rings.iter().flat_map(|&d| ring(d)).collect();
    let lines: Vec<&[f64]> = outline.iter().map(Vec::as_slice).collect();
    // Refinement alone, as T3 measured it: without the toe lines (#149 T2 measures those).
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
            refine,
            grading: Grading {
                cut: HALF,
                fill: HALF,
                limit: 10.0,
                rounding,
                effect: Effect::Both,
                tolerance: 1e-6,
            },
        },
        None,
    )?;
    Ok((built, points))
}

/// Largest gap between the mesh and the exact faces over ten samples per
/// inside triangle (more than the refinement takes), and the fifth-percentile
/// smallest angle, degrees, of the inside triangles off the pavement.
fn measure(built: &Outlined, points: &[f64], rounding: f64) -> Result<(f64, f64), OutlineError> {
    let base = Field::new(points, &base().1).map_err(|_| OutlineError::Field)?;
    let surface = Field::new(&PAVED, &PAVED_TRIANGLES).map_err(|_| OutlineError::Surface)?;
    let uniform = SurfaceGrade {
        cut: 0.5,
        fill: 0.5,
        rounding,
    };
    let edges = outline(&PAVED, &PAVED_TRIANGLES, &[], uniform);
    let faces = Faces::new(
        &base,
        &surface,
        &edges,
        Grading {
            cut: HALF,
            fill: HALF,
            limit: 10.0,
            rounding,
            effect: Effect::Both,
            tolerance: 1e-6,
        },
    );
    let corners = plan::vertices(&built.points);
    let mut gap = 0.0_f64;
    let mut angles = Vec::new();
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
        for [wa, wb] in [
            [0.1_f64, 0.1],
            [0.8, 0.1],
            [0.1, 0.8],
            [0.45, 0.1],
            [0.1, 0.45],
            [0.45, 0.45],
            [0.6, 0.2],
            [0.2, 0.6],
            [0.2, 0.2],
            [1.0 / 3.0, 1.0 / 3.0],
        ] {
            let wc = 1.0 - wa - wb;
            let [x, y, z] = mix([wa, wb, wc], [pa, pb, pc]);
            if let Some((height, _)) = faces.height(x, y) {
                gap = gap.max((height - z).abs());
            }
        }
        let paved = [pa, pb, pc]
            .iter()
            .all(|p| (-1e-9..=40.0 + 1e-9).contains(&p[0]) && p[1].abs() <= 2.0 + 1e-9);
        if !paved {
            let angle = |p: [f64; 3], q: [f64; 3], r: [f64; 3]| {
                let (u, v) = ([q[0] - p[0], q[1] - p[1]], [r[0] - p[0], r[1] - p[1]]);
                u[0].mul_add(v[1], -u[1] * v[0])
                    .abs()
                    .atan2(u[0].mul_add(v[0], u[1] * v[1]))
                    .to_degrees()
            };
            let smallest = angle(pa, pb, pc)
                .min(angle(pb, pc, pa))
                .min(angle(pc, pa, pb));
            angles.push(smallest);
        }
    }
    angles.sort_by(f64::total_cmp);
    Ok((gap, angles.get(angles.len() / 20).copied().unwrap_or(0.0)))
}

#[test]
fn refinement_bounds_the_faces_vertical_error_without_offset_rings() -> Result<(), OutlineError> {
    for rounding in [0.0, 1.0] {
        let (coarse, points) = refined(rounding, None)?;
        let (built, _) = refined(rounding, Some(Refine::DEFAULT))?;
        let (before, _) = measure(&coarse, &points, rounding)?;
        let (gap, p5) = measure(&built, &points, rounding)?;
        // What the caller's offset rings gave: half-metre rings out past the toe.
        let rings: Vec<f64> = (0..=12).map(|k| 0.5 * f64::from(k)).collect();
        let (ringed, _) = refined_with(rounding, None, &rings)?;
        let (ring_gap, ring_p5) = measure(&ringed, &points, rounding)?;
        let report = built.refined;
        eprintln!(
            "rounding {rounding}: unrefined {before:.3} m; rings {ring_gap:.3} m, p5 {ring_p5:.1} deg,              {} vertices; refined {gap:.4} m, p5 {p5:.1} deg, {} vertices, {report:?}",
            ringed.points.len() / 3,
            built.points.len() / 3
        );
        assert!(
            before > 0.5,
            "the unrefined mesh misses the faces: {before}"
        );
        // Rounding 1 stalls a little over the tolerance on a few slivers whose
        // sides no split shortens (#149 T3, open); it stops rather than spins.
        assert!(report.residual <= 0.02 && report.rounds < 24, "{report:?}");
        // Ten samples per triangle against the refinement's four: a little over.
        assert!(gap <= 0.02, "{gap}");
        assert!(ring_gap > 10.0 * gap, "{ring_gap} {gap}");
        // Bisection and its crossing lines leave thinner triangles than
        // half-metre rings (3 to 7 against 11 degrees at the fifth
        // percentile): the 15 degree gate of T3 is open. A floor, not a target.
        assert!(p5 >= 2.0 && ring_p5 > p5, "{p5} {ring_p5}");
        // Locality: unmoved triangles are the ground, straddling ones are narrow.
        let (wide, ground) = toe_band(&built, &points)?;
        assert_eq!(wide, 0, "straddling triangles wider than the band");
        assert!(
            ground < 1e-12,
            "an unmoved triangle leaves the ground by {ground}"
        );
    }
    Ok(())
}

/// Over the inside triangles: how many straddle the toe (moved and unmoved
/// vertices) with a side longer than the band, and how far a triangle whose
/// vertices are all unmoved strays from the ground at its samples.
fn toe_band(built: &Outlined, points: &[f64]) -> Result<(usize, f64), OutlineError> {
    let ground = Field::new(points, &base().1).map_err(|_| OutlineError::Field)?;
    let corners = plan::vertices(&built.points);
    let (mut wide, mut stray) = (0, 0.0_f64);
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
        let moved = [a, b, c].map(|v| built.owners.get(v as usize).copied().unwrap_or(false));
        let side = |p: [f64; 3], q: [f64; 3]| (q[0] - p[0]).hypot(q[1] - p[1]);
        if moved.iter().any(|m| *m) && !moved.iter().all(|m| *m) {
            let longest = side(pa, pb).max(side(pb, pc)).max(side(pc, pa));
            wide += usize::from(longest > Refine::DEFAULT.toe + 1e-9);
        } else if !moved.iter().any(|m| *m) {
            for weights in [
                [1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0],
                [0.6, 0.2, 0.2],
                [0.2, 0.6, 0.2],
            ] {
                let [x, y, z] = mix(weights, [pa, pb, pc]);
                if let Some(sample) = ground.height_at(x, y) {
                    stray = stray.max((sample.height - z).abs());
                }
            }
        }
    }
    Ok((wide, stray))
}
