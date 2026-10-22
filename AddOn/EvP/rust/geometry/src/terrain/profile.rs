//! Opt-in stage attribution only; no clock or profiling API in shipped geometry.
use super::{
    Error, Tin, canonical, classify, constrain, input, presplit, refine, verify_constraints,
};
use std::time::Instant;

#[path = "../../../tests/terrain_fixture.rs"]
mod fixture;

type StageTimes = Vec<(&'static str, f64)>;

fn stage<T>(times: &mut StageTimes, name: &'static str, action: impl FnOnce() -> T) -> T {
    let start = Instant::now();
    let value = action();
    times.push((name, start.elapsed().as_secs_f64() * 1000.0));
    value
}

// This is the production orchestration replayed through its PRIVATE owners,
// never an alternate algorithm. Exact Tin equality against triangulate below
// guards the profiling seam; each stage includes its own temporary destruction.
fn profile(request: &super::Request<'_>) -> Result<(Tin, StageTimes), Error> {
    let mut times = Vec::with_capacity(12);
    let mut prepared = stage(&mut times, "prepare", || input::prepare(request))?;
    let split = stage(&mut times, "presplit", || presplit::presplit(&mut prepared))?;
    let built = stage(&mut times, "constrain", || {
        constrain::constrain(&prepared, &split)
    })?;
    let (built, steiner) = stage(&mut times, "refine", || {
        if let Some(parameters) = request.refinement {
            refine::refine(&mut prepared, built, parameters)
        } else {
            Ok((built, Vec::new()))
        }
    })?;
    let (faces, rows) = stage(&mut times, "canonicalize", || {
        drop(built.cdt);
        let mut faces: Vec<_> = built.faces.into_iter().map(canonical).collect();
        faces.sort_unstable();
        (faces, built.rows)
    });
    let classified = stage(&mut times, "classify", || {
        classify::classify(&prepared, &faces, &rows)
    })?;
    let (triangles, face_boundary, constraint_edges) = stage(&mut times, "pack", || {
        let mut triangles = Vec::new();
        let mut face_boundary = Vec::new();
        for ((face, keep), boundary) in faces.iter().zip(&classified.kept).zip(&classified.boundary)
        {
            if *keep {
                triangles.extend_from_slice(face);
                face_boundary.push(*boundary);
            }
        }
        let constraint_edges = rows
            .into_iter()
            .map(|row| super::ConstraintEdge {
                kept: classified.edge_kept(row.vertices),
                ..row
            })
            .collect();
        (triangles, face_boundary, constraint_edges)
    });
    if triangles.is_empty() {
        return Err(Error::TriangulationDegenerate);
    }
    stage(&mut times, "release_topology", || {
        drop(classified);
        drop(faces);
    });
    let tin = stage(&mut times, "result", || Tin {
        points: prepared.positions.iter().flatten().copied().collect(),
        triangles,
        face_boundary,
        input_vertices: std::mem::take(&mut prepared.input_vertices),
        crossings: split.crossings,
        steiner,
        constraint_edges,
    });
    stage(&mut times, "verify", || verify_constraints(request, &tin))?;
    stage(&mut times, "cleanup", || {
        drop(split.pieces);
        drop(prepared);
    });
    Ok((tin, times))
}

#[test]
#[ignore = "opt-in release stage profile; timing is not a CI assertion"]
fn terrain_stages() -> Result<(), String> {
    if cfg!(debug_assertions) {
        return Err("release profiling build required".into());
    }
    for input in fixture::corpus()?
        .iter()
        .chain(std::iter::once(&fixture::workload()))
    {
        let constraints = input.constraints();
        let request = input.request(&constraints);
        let expected = super::triangulate(&request).map_err(Error::message)?;
        for _ in 0..3 {
            std::hint::black_box(profile(&request).map_err(Error::message)?);
        }
        for sample in 0..9 {
            let (tin, times) = profile(&request).map_err(Error::message)?;
            assert_eq!(tin, expected);
            // Read all fixture framing here as well as the request, so this
            // shared parser has no dead fields hidden by a lint waiver.
            let framing = (
                input.area,
                input.inside_area,
                input.crossings,
                input.line.len(),
            );
            std::hint::black_box(framing);
            print!(
                "{{\"fixture\":\"{}\",\"sample\":{sample},\"hash\":\"{:016x}\",\"stage_ms\":{{",
                input.name,
                fixture::digest(&tin.points, &tin.triangles)
            );
            for (i, (name, ms)) in times.iter().enumerate() {
                if i != 0 {
                    print!(",");
                }
                print!("\"{name}\":{ms}");
            }
            println!("}}}}");
        }
    }
    Ok(())
}
