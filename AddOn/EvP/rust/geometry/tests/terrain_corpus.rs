//! S3a.3 shared neutral landscape fixtures: no SDK/native capture claims.
#![forbid(unsafe_code)]
#[path = "../../tests/terrain_fixture.rs"]
mod fixture;
use tapioca_geometry::terrain::{self, Host, SplitRequest};

fn point(points: &[f64], index: u32) -> [f64; 3] {
    let offset = index as usize * 3;
    let Some(&[x, y, z]) = points.get(offset..offset + 3) else {
        unreachable!("test index")
    };
    [x, y, z]
}
fn area(points: &[f64], triangle: &[u32]) -> f64 {
    let [a, b, c] = triangle else {
        unreachable!("test triangle")
    };
    let ([ax, ay, _], [bx, by, _], [cx, cy, _]) =
        (point(points, *a), point(points, *b), point(points, *c));
    0.5 * ((bx - ax) * (cy - ay) - (by - ay) * (cx - ax))
}

#[test]
fn landscape_tin_split_surface_and_ancestry() -> Result<(), String> {
    let corpus = fixture::corpus()?;
    assert_eq!(corpus.len(), 6);
    for input in corpus {
        let constraints = input.constraints();
        let request = input.request(&constraints);
        let tin = terrain::triangulate(&request).map_err(|e| format!("{}: {e:?}", input.name))?;
        assert_eq!(terrain::verify_constraints(&request, &tin), Ok(()));
        assert!(tin.crossings.len() >= input.crossings, "{}", input.name);
        let total: f64 = tin
            .triangles
            .as_chunks::<3>()
            .0
            .iter()
            .map(|face| {
                let a = area(&tin.points, face);
                assert!(a > 0.0);
                a
            })
            .sum();
        assert!(
            (total - input.area).abs() < 1e-7,
            "{} area {total}",
            input.name
        );
        assert_eq!(
            terrain::triangulate(&request).map_err(terrain::Error::message)?,
            tin
        );
        // Analytic piecewise-x features must have no triangle bridging a riser.
        for face in tin.triangles.as_chunks::<3>().0 {
            let centroid = face
                .iter()
                .map(|&i| point(&tin.points, i))
                .fold([0.0; 3], |a, b| {
                    std::array::from_fn(|i| {
                        a.get(i).copied().unwrap_or_default()
                            + b.get(i).copied().unwrap_or_default()
                    })
                });
            let [x, y, z] = centroid.map(|v| v / 3.0);
            let local = x - 500_000.0;
            let ramp = |start: f64, width: f64, height: f64| {
                ((local - start) / width).clamp(0.0, 1.0) * height
            };
            let expected = match input.name.as_str() {
                "wall_sides" => ramp(6.0, 0.2, 3.0),
                "steep_curb" => ramp(12.0, 0.15, 0.18),
                "stair_treads" => ramp(5.0, 0.1, 0.2) + ramp(7.0, 0.1, 0.2) + ramp(9.0, 0.1, 0.2),
                _ => 0.1 * local + 0.05 * (y - 6_000_000.0),
            };
            assert!(
                (z - expected).abs() < 1e-7,
                "{} surface {x} {y} {z} != {expected}",
                input.name
            );
        }
        let split_request = SplitRequest {
            points: &tin.points,
            triangles: &tin.triangles,
            line: &input.line,
            arc_angles: &[],
        };
        let split = terrain::split_mesh(&split_request)
            .map_err(|e| format!("{} split: {e:?}", input.name))?;
        assert_eq!(
            terrain::split_mesh(&split_request).map_err(terrain::Error::message)?,
            split
        );
        assert_eq!(
            split.points.get(..tin.points.len()),
            Some(tin.points.as_slice())
        );
        let mut parent_areas = vec![0.0; tin.triangles.len() / 3];
        let mut inside = 0.0;
        for ((face, parent), is_inside) in split
            .triangles
            .as_chunks::<3>()
            .0
            .iter()
            .zip(&split.parents)
            .zip(&split.inside)
        {
            let a = area(&split.points, face);
            assert!(a > 0.0);
            let Some(sum) = parent_areas.get_mut(*parent as usize) else {
                return Err("parent index".into());
            };
            *sum += a;
            if *is_inside {
                inside += a;
            }
        }
        assert!(
            (inside - input.inside_area).abs() < 1e-7,
            "{} inside",
            input.name
        );
        for (sum, face) in parent_areas.iter().zip(tin.triangles.as_chunks::<3>().0) {
            assert!(
                (sum - area(&tin.points, face)).abs() < 1e-7,
                "{} parent",
                input.name
            );
        }
        // New points must lie on their ORIGINAL host surface, not a new diagonal.
        for (i, vertex) in split.new_vertices.iter().enumerate() {
            let index = u32::try_from(tin.points.len() / 3 + i).map_err(|_| "index")?;
            let position = point(&split.points, index);
            match vertex.host {
                Host::Edge([a, b]) => {
                    let a = point(&tin.points, a);
                    let b = point(&tin.points, b);
                    let [ax, ay, az] = a;
                    let [bx, by, bz] = b;
                    let [px, py, pz] = position;
                    let t = if (bx - ax).abs() > (by - ay).abs() {
                        (px - ax) / (bx - ax)
                    } else {
                        (py - ay) / (by - ay)
                    };
                    assert!((pz - (az + t * (bz - az))).abs() < 1e-7);
                }
                Host::Face(parent) => {
                    let offset = parent as usize * 3;
                    let Some(face) = tin.triangles.get(offset..offset + 3) else {
                        return Err("host face".into());
                    };
                    let [first, second, third] = face else {
                        return Err("host shape".into());
                    };
                    let ([ax, ay, az], [bx, by, bz], [cx, cy, cz]) = (
                        point(&tin.points, *first),
                        point(&tin.points, *second),
                        point(&tin.points, *third),
                    );
                    let [px, py, pz] = position;
                    let d = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
                    let u = ((px - ax) * (cy - ay) - (py - ay) * (cx - ax)) / d;
                    let v = ((bx - ax) * (py - ay) - (by - ay) * (px - ax)) / d;
                    assert!((pz - (az + u * (bz - az) + v * (cz - az))).abs() < 1e-7);
                }
            }
        }
        // The remote spot forces actual untouched faces beyond the line, so
        // this must never pass vacuously on a coarse mesh spanning the site.
        let right = input
            .line
            .as_chunks::<3>()
            .0
            .iter()
            .map(|&[x, _, _]| x)
            .fold(f64::NEG_INFINITY, f64::max);
        let mut untouched = 0;
        for (parent, face) in
            tin.triangles
                .as_chunks::<3>()
                .0
                .iter()
                .enumerate()
                .filter(|(_, f)| {
                    f.iter().all(|&i| {
                        let [x, _, _] = point(&tin.points, i);
                        x > right
                    })
                })
        {
            let children: Vec<_> = split
                .triangles
                .as_chunks::<3>()
                .0
                .iter()
                .zip(&split.parents)
                .filter(|(_, p)| **p as usize == parent)
                .map(|(f, _)| f)
                .collect();
            assert_eq!(children, vec![face]);
            untouched += 1;
        }
        assert!(untouched > 0, "{} remote coverage", input.name);
    }
    Ok(())
}

#[test]
fn target_workload_counts_are_reproducible() -> Result<(), String> {
    let input = fixture::workload();
    let constraints = input.constraints();
    assert_eq!(input.points.len() / 3, 10_000);
    assert_eq!(constraints.len(), 500);
    let tin =
        terrain::triangulate(&input.request(&constraints)).map_err(terrain::Error::message)?;
    assert_eq!(tin.triangles.len() / 3, 19_602);
    // Ordering regression pin for spade 2.15.1, separate from analytic geometry
    // oracles above; identical to both measured lanes, not a native mesh oracle.
    assert_eq!(
        fixture::digest(&tin.points, &tin.triangles),
        0xb48f_aa25_01bb_4346
    );
    Ok(())
}
