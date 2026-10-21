//! Proof helpers for the breakline and hole contract, independent of `spade`.

use std::collections::BTreeSet;

use super::classify::{face_edges, key};
use super::{Error, Request, Role, SegmentRef, Tin};

/// Sorted unique undirected edges (`[low, high]`) of a triangle list.
///
/// # Errors
/// `Shape` when the list is not whole vertex triples.
pub fn extract_edges(triangles: &[u32]) -> Result<Vec<[u32; 2]>, Error> {
    let (faces, remainder) = triangles.as_chunks::<3>();
    if !remainder.is_empty() {
        return Err(Error::Shape);
    }
    let edges: BTreeSet<[u32; 2]> = faces
        .iter()
        .flat_map(|face| face_edges(*face).map(key))
        .collect();
    Ok(edges.into_iter().collect())
}

/// Check a result against the request it claims to triangulate.
///
/// For every input constraint segment the result's rows must form one path,
/// in segment direction, from the segment's start vertex to its end vertex
/// (through its crossing and touching vertices). A row is marked kept exactly
/// when its edge is an edge of the kept triangles, and every boundary/hole row
/// must be kept. Each crossing vertex must lie on both of its segments' paths.
///
/// # Errors
/// `Shape`/`Index` when the result does not match the request's shape, and
/// `BreaklineUnenforced` naming the first constraint whose path is broken.
pub fn verify_constraints(request: &Request<'_>, tin: &Tin) -> Result<(), Error> {
    if tin.input_vertices.len() * 3 != request.points.len() {
        return Err(Error::Shape);
    }
    let edges: BTreeSet<[u32; 2]> = extract_edges(&tin.triangles)?.into_iter().collect();
    let mut rows = tin.constraint_edges.iter().peekable();
    for (constraint, item) in (0_u32..).zip(request.constraints) {
        let closed = item.role != Role::Breakline;
        let ends = item
            .vertices
            .iter()
            .map(|&input| {
                usize::try_from(input)
                    .ok()
                    .and_then(|index| tin.input_vertices.get(index))
                    .copied()
                    .ok_or(Error::Index)
            })
            .collect::<Result<Vec<_>, _>>()?;
        let closing = ends.first().copied().filter(|_| closed);
        let pairs = ends
            .iter()
            .copied()
            .zip(ends.iter().copied().skip(1).chain(closing));
        let unenforced = Error::BreaklineUnenforced { constraint };
        for (segment, (from, to)) in (0_u32..).zip(pairs) {
            let id = SegmentRef {
                constraint,
                segment,
            };
            let mut current = from;
            while let Some(row) = rows.next_if(|row| row.segment == id) {
                let present = edges.contains(&key(row.vertices));
                if row.vertices[0] != current || row.kept != present || (closed && !row.kept) {
                    return Err(unenforced);
                }
                current = row.vertices[1];
            }
            if current != to {
                return Err(unenforced);
            }
        }
    }
    if rows.next().is_some() {
        return Err(Error::Shape);
    }
    let on_paths: BTreeSet<(SegmentRef, u32)> = tin
        .constraint_edges
        .iter()
        .flat_map(|row| row.vertices.map(|vertex| (row.segment, vertex)))
        .collect();
    for crossing in &tin.crossings {
        for segment in crossing.segments {
            if !on_paths.contains(&(segment, crossing.vertex)) {
                return Err(Error::BreaklineUnenforced {
                    constraint: segment.constraint,
                });
            }
        }
    }
    Ok(())
}
