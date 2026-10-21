//! Inside/outside classification of the triangulated faces by ring nesting.
//!
//! Faces are grouped into regions that are connected without crossing a
//! boundary or hole edge (breaklines do not separate regions). One face per
//! region is tested against every ring by crossing number, giving its depth:
//! boundaries containing it minus holes containing it (one implicit boundary,
//! the convex hull, when the request has none). Depth 1 is kept, depth 0 is
//! excluded and anything else (nested boundaries, a hole outside every
//! boundary) refuses. Finally every ring edge must have a kept face on exactly
//! one side, so a hole is never silently filled and a boundary never bridged.

use std::collections::BTreeMap;

use super::input::Prepared;
use super::{ConstraintEdge, Error, Role};

/// Per-face keep flag and, for kept faces, the boundary ring that bounds the
/// face's region (`None` when the domain is the convex hull).
pub(super) struct Classified {
    pub kept: Vec<bool>,
    pub boundary: Vec<Option<u32>>,
}

struct Ring {
    constraint: u32,
    role: Role,
    segments: Vec<[[f64; 2]; 2]>,
}

pub(super) fn classify(
    prepared: &Prepared,
    faces: &[[u32; 3]],
    rows: &[ConstraintEdge],
) -> Result<Classified, Error> {
    let rings = rings(prepared)?;
    let roles: BTreeMap<u32, Role> = rings.iter().map(|r| (r.constraint, r.role)).collect();
    let mut ring_edges: BTreeMap<[u32; 2], Vec<u32>> = BTreeMap::new();
    for row in rows
        .iter()
        .filter(|row| roles.contains_key(&row.segment.constraint))
    {
        ring_edges
            .entry(key(row.vertices))
            .or_default()
            .push(row.segment.constraint);
    }
    let mut adjacent: BTreeMap<[u32; 2], Vec<usize>> = BTreeMap::new();
    for (index, face) in faces.iter().enumerate() {
        for edge in face_edges(*face) {
            adjacent.entry(key(edge)).or_default().push(index);
        }
    }
    let has_boundary = rings.iter().any(|ring| ring.role == Role::Boundary);
    let mut kept = vec![false; faces.len()];
    let mut boundary = vec![None; faces.len()];
    let mut region = vec![false; faces.len()];
    for (seed, face) in faces.iter().enumerate() {
        if region.get(seed).copied().unwrap_or(true) {
            continue;
        }
        let depth = depth(&rings, has_boundary, centroid(prepared, *face)?);
        let keep = match depth {
            0 => false,
            1 => true,
            _ => return Err(Error::HoleClassificationFailed),
        };
        let (members, borders) = flood(faces, &adjacent, &ring_edges, &mut region, seed);
        let bound = borders
            .iter()
            .filter(|constraint| roles.get(constraint) == Some(&Role::Boundary))
            .min()
            .copied();
        if keep && has_boundary && bound.is_none() {
            return Err(Error::HoleClassificationFailed);
        }
        for member in members {
            if let (Some(flag), Some(owner)) = (kept.get_mut(member), boundary.get_mut(member)) {
                *flag = keep;
                *owner = bound.filter(|_| keep);
            }
        }
    }
    for edge in ring_edges.keys() {
        let sides = adjacent.get(edge).map_or(0, |list| {
            list.iter()
                .filter(|&&face| kept.get(face).copied().unwrap_or(false))
                .count()
        });
        if sides != 1 {
            return Err(Error::HoleClassificationFailed);
        }
    }
    Ok(Classified { kept, boundary })
}

/// Whether an undirected edge borders at least one kept face.
pub(super) fn edge_kept(faces: &[[u32; 3]], kept: &[bool]) -> BTreeMap<[u32; 2], bool> {
    let mut result: BTreeMap<[u32; 2], bool> = BTreeMap::new();
    for (face, &keep) in faces.iter().zip(kept) {
        for edge in face_edges(*face) {
            *result.entry(key(edge)).or_default() |= keep;
        }
    }
    result
}

/// Collect one region and the ring constraints on its border.
fn flood(
    faces: &[[u32; 3]],
    adjacent: &BTreeMap<[u32; 2], Vec<usize>>,
    ring_edges: &BTreeMap<[u32; 2], Vec<u32>>,
    region: &mut [bool],
    seed: usize,
) -> (Vec<usize>, Vec<u32>) {
    let mut members = Vec::new();
    let mut borders = Vec::new();
    let mut stack = vec![seed];
    if let Some(flag) = region.get_mut(seed) {
        *flag = true;
    }
    while let Some(face) = stack.pop() {
        members.push(face);
        let Some(vertices) = faces.get(face) else {
            continue;
        };
        for edge in face_edges(*vertices).map(key) {
            if let Some(constraints) = ring_edges.get(&edge) {
                borders.extend_from_slice(constraints);
                continue;
            }
            for &next in adjacent.get(&edge).into_iter().flatten() {
                if let Some(flag) = region.get_mut(next).filter(|flag| !**flag) {
                    *flag = true;
                    stack.push(next);
                }
            }
        }
    }
    (members, borders)
}

fn rings(prepared: &Prepared) -> Result<Vec<Ring>, Error> {
    let mut rings: Vec<Ring> = Vec::new();
    for segment in prepared
        .segments
        .iter()
        .filter(|s| s.role != Role::Breakline)
    {
        let [a, b] = segment.ends.map(|vertex| prepared.position(vertex));
        let ends = [a?, b?].map(|p| [p[0], p[1]]);
        match rings.last_mut() {
            Some(ring) if ring.constraint == segment.id.constraint => ring.segments.push(ends),
            _ => rings.push(Ring {
                constraint: segment.id.constraint,
                role: segment.role,
                segments: vec![ends],
            }),
        }
    }
    Ok(rings)
}

fn depth(rings: &[Ring], has_boundary: bool, point: [f64; 2]) -> i64 {
    let mut depth = i64::from(!has_boundary);
    for ring in rings {
        if contains(&ring.segments, point) {
            depth += if ring.role == Role::Hole { -1 } else { 1 };
        }
    }
    depth
}

/// Even-odd crossing number of a horizontal ray to +x.
fn contains(segments: &[[[f64; 2]; 2]], [x, y]: [f64; 2]) -> bool {
    let mut inside = false;
    for &[p, q] in segments {
        if (p[1] > y) != (q[1] > y) {
            let at = p[0] + (y - p[1]) * (q[0] - p[0]) / (q[1] - p[1]);
            if x < at {
                inside = !inside;
            }
        }
    }
    inside
}

fn centroid(prepared: &Prepared, face: [u32; 3]) -> Result<[f64; 2], Error> {
    let [a, b, c] = face.map(|vertex| prepared.position(vertex));
    let (a, b, c) = (a?, b?, c?);
    Ok([(a[0] + b[0] + c[0]) / 3.0, (a[1] + b[1] + c[1]) / 3.0])
}

pub(super) const fn face_edges([a, b, c]: [u32; 3]) -> [[u32; 2]; 3] {
    [[a, b], [b, c], [c, a]]
}

pub(super) const fn key([a, b]: [u32; 2]) -> [u32; 2] {
    if a < b { [a, b] } else { [b, a] }
}
