//! Rounded faces: the top and toe creases ease into the band and the ground,
//! the straight face between them is the sharp one, and the build carries the
//! lines the curves need.

use super::*;
use crate::terrain::field::Field;

/// Flat ground at 10 m under a level 4 m path at 12 m: both sides fill 1:2 and
/// daylight 4 m out from each edge.
fn embankment(rounding: f64) -> Vec<Station> {
    (0..=8)
        .map(|i| {
            let x = f64::from(i) * 5.0;
            Station {
                rounding,
                ..station(x, 12.0, 0.0)
            }
        })
        .collect()
}

fn flat(_: f64, _: f64) -> f64 {
    10.0
}

/// Height `d` metres out from the left edge at `x = 20`.
fn across(design: &Design<'_>, d: f64) -> Result<f64, HybridError> {
    design
        .height(20.0, 2.0 + d)
        .map_err(HybridError::Design)?
        .map(|sample| sample.height)
        .ok_or(HybridError::Design(DesignError::Coverage))
}

#[test]
fn a_rounded_face_leaves_the_edge_level_meets_the_ground_level_and_is_straight_between()
-> Result<(), HybridError> {
    let (points, triangles) = base(flat);
    let field = Field::new(&points, &triangles).map_err(|_| HybridError::Field)?;
    let (sharp, rounded) = (embankment(0.0), embankment(1.0));
    let band = |stations| Band {
        stations,
        effect: Effect::Both,
    };
    let sharp = Design::new(&band(&sharp), &field, 1e-6).map_err(HybridError::Design)?;
    let rounded = Design::new(&band(&rounded), &field, 1e-6).map_err(HybridError::Design)?;
    let step = 1e-4;
    let slope = |design: &Design<'_>, d: f64| -> Result<f64, HybridError> {
        Ok((across(design, d + step)? - across(design, d - step)?) / (2.0 * step))
    };
    // The edge itself and the straight middle of the face are unchanged.
    assert!(close(across(&rounded, 0.0)?, 12.0, 1e-12));
    for d in [1.5, 2.0, 2.5, 3.0] {
        assert!(
            close(across(&rounded, d)?, across(&sharp, d)?, 1e-12),
            "{d}"
        );
    }
    // Sharp: level band, then -1/2 at once. Rounded: still level just past the edge.
    assert!(close(slope(&sharp, step * 2.0)?, -0.5, 1e-6));
    assert!(slope(&rounded, step * 2.0)?.abs() < 1e-3);
    // At the toe the sharp face still falls at 1/2; the rounded one has eased level.
    assert!(close(slope(&sharp, 4.0 - step * 2.0)?, -0.5, 1e-6));
    assert!(slope(&rounded, 4.0 - step * 2.0)?.abs() < 1e-3);
    assert!(close(across(&rounded, 4.0)?, 10.0, 1e-12));
    Ok(())
}

/// Largest angle between neighbouring triangles' normals, degrees, away from
/// the band's ends (whose caps are a later phase and stay abrupt).
fn sharpest(built: &Built) -> f64 {
    let points = plan::vertices(&built.points);
    let normal = |triple: &[u32; 3]| -> Option<[f64; 3]> {
        let [first, second, third] = triple.map(|v| points.get(v as usize).copied());
        let (origin, along, other) = (first?, second?, third?);
        let (u, w) = (
            [
                along[0] - origin[0],
                along[1] - origin[1],
                along[2] - origin[2],
            ],
            [
                other[0] - origin[0],
                other[1] - origin[1],
                other[2] - origin[2],
            ],
        );
        let cross = [
            u[1] * w[2] - u[2] * w[1],
            u[2] * w[0] - u[0] * w[2],
            u[0] * w[1] - u[1] * w[0],
        ];
        let length = (cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]).sqrt();
        (length > 0.0).then(|| cross.map(|v| v / length))
    };
    let triangles = built.mesh.triangles.as_chunks::<3>().0;
    let mut by_edge = std::collections::BTreeMap::<(u32, u32), Vec<usize>>::new();
    for (index, triple) in triangles.iter().enumerate() {
        for k in 0..3 {
            let (Some(&from), Some(&to)) = (triple.get(k), triple.get((k + 1) % 3)) else {
                continue;
            };
            by_edge
                .entry((from.min(to), from.max(to)))
                .or_default()
                .push(index);
        }
    }
    let mut worst: f64 = 0.0;
    for faces in by_edge.values() {
        let inside = |index: usize| {
            triangles.get(index).is_some_and(|triple| {
                let x = triple
                    .iter()
                    .filter_map(|&v| points.get(v as usize))
                    .map(|p| p[0])
                    .sum::<f64>()
                    / 3.0;
                x > 5.0 && x < 35.0
            })
        };
        if let [first, second] = faces.as_slice()
            && inside(*first)
            && inside(*second)
            && let (Some(Some(n)), Some(Some(m))) = (
                triangles.get(*first).map(normal),
                triangles.get(*second).map(normal),
            )
        {
            let dot = (n[0] * m[0] + n[1] * m[1] + n[2] * m[2]).clamp(-1.0, 1.0);
            worst = worst.max(dot.acos().to_degrees());
        }
    }
    worst
}

#[test]
fn a_rounded_build_lines_its_faces_and_softens_their_creases() -> Result<(), HybridError> {
    let (points, triangles) = base(flat);
    let sharp = run(&points, &triangles, &embankment(0.0))?;
    let rounded = run(&points, &triangles, &embankment(1.0))?;
    let faces = |built: &Built| {
        built
            .lines
            .iter()
            .filter(|kind| matches!(kind, LineKind::Face { .. }))
            .count()
    };
    assert_eq!(faces(&sharp), 0);
    assert_eq!(faces(&rounded), 2 * FACE_LINES);
    // The 1:2 face meets its level band and ground at 26.6 degrees when sharp;
    // Rounded over a metre, every crease is gentler -- triangles that cross the
    // face lines diagonally (at the base grid's crossings) keep the worst of them.
    assert!(sharpest(&sharp) > 26.0);
    assert!(sharpest(&rounded) < 20.0, "{}", sharpest(&rounded));
    // The daylight stays on the ground, so the volume is still bounded by it.
    let ([sharp_fill, _], [round_fill, _]) =
        (between(&sharp, 0.0, 40.0), between(&rounded, 0.0, 40.0));
    assert!(round_fill > 0.0 && round_fill < sharp_fill);
    Ok(())
}
