//! Inside/outside classification of the triangulated faces by winding depth.
//!
//! The depth of a face is the number of boundary rings around it minus the
//! number of hole rings around it. It is computed topologically, not by testing
//! points: outside the triangulation the depth is 0 (1 when the request has no
//! boundary, so the convex hull acts as one), and crossing a ring edge changes
//! it by one according to the edge's direction and the ring's orientation. A
//! face reached with two different depths, or any depth other than 0 or 1,
//! refuses. Depth 1 is kept. Every ring edge must then have a kept face on
//! exactly one side, so a hole is never silently filled and a boundary never
//! bridged. Testing centroids instead fails on the sliver faces that rings with
//! nearly collinear vertices produce.

use std::collections::{BTreeMap, VecDeque};

use super::input::Prepared;
use super::{ConstraintEdge, Error, Role};

/// Per-face keep flag and, for kept faces, the boundary ring that bounds the
/// face's region (`None` when the domain is the convex hull).
pub(super) struct Classified {
    pub kept: Vec<bool>,
    pub boundary: Vec<Option<u32>>,
}

pub(super) fn classify(
    prepared: &Prepared,
    faces: &[[u32; 3]],
    rows: &[ConstraintEdge],
) -> Result<Classified, Error> {
    let roles = ring_weights(prepared)?;
    let mut ring_edges: BTreeMap<[u32; 2], Vec<u32>> = BTreeMap::new();
    let mut weights: BTreeMap<[u32; 2], i64> = BTreeMap::new();
    for row in rows {
        if let Some(&(_, weight)) = roles.get(&row.segment.constraint) {
            ring_edges
                .entry(key(row.vertices))
                .or_default()
                .push(row.segment.constraint);
            *weights.entry(row.vertices).or_default() += weight;
        }
    }
    let has_boundary = roles.values().any(|(role, _)| *role == Role::Boundary);
    let depths = winding(faces, &weights, i64::from(!has_boundary))
        .ok_or(Error::HoleClassificationFailed)?;
    let mut kept = Vec::with_capacity(faces.len());
    for depth in depths {
        match depth {
            0 => kept.push(false),
            1 => kept.push(true),
            _ => return Err(Error::HoleClassificationFailed),
        }
    }
    let adjacent = adjacency(faces);
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
    let mut boundary = vec![None; faces.len()];
    let mut region = vec![false; faces.len()];
    for seed in 0..faces.len() {
        if region.get(seed).copied().unwrap_or(true) || !kept.get(seed).copied().unwrap_or(false) {
            continue;
        }
        let (members, borders) = flood(faces, &adjacent, &ring_edges, &mut region, seed);
        let bound = borders
            .iter()
            .filter(|constraint| matches!(roles.get(constraint), Some((Role::Boundary, _))))
            .min()
            .copied();
        if has_boundary && bound.is_none() {
            return Err(Error::HoleClassificationFailed);
        }
        for member in members {
            if let Some(owner) = boundary.get_mut(member) {
                *owner = bound;
            }
        }
    }
    Ok(Classified { kept, boundary })
}

/// Winding depth of every face of a triangle list.
///
/// `weights[[u, v]]` is how much the depth rises when crossing the directed
/// edge `u -> v` from its right side to its left side (several rows on one
/// edge add up). Outside the triangle list the depth is `outside`. Faces are
/// counter-clockwise, so each face lies left of its own edges. Returns `None`
/// when a face is reached with two different depths (or none at all).
pub(super) fn winding(
    faces: &[[u32; 3]],
    weights: &BTreeMap<[u32; 2], i64>,
    outside: i64,
) -> Option<Vec<i64>> {
    let rise = |[a, b]: [u32; 2]| {
        weights.get(&[a, b]).copied().unwrap_or(0) - weights.get(&[b, a]).copied().unwrap_or(0)
    };
    let adjacent = adjacency(faces);
    let mut depths: Vec<Option<i64>> = vec![None; faces.len()];
    let mut queue = VecDeque::new();
    for (index, face) in faces.iter().enumerate() {
        for edge in face_edges(*face) {
            if adjacent.get(&key(edge)).is_some_and(|list| list.len() == 1) {
                assign(&mut depths, &mut queue, index, outside + rise(edge))?;
            }
        }
    }
    while let Some(index) = queue.pop_front() {
        let depth = depths.get(index).copied().flatten()?;
        let face = faces.get(index)?;
        for edge in face_edges(*face) {
            for &next in adjacent.get(&key(edge)).into_iter().flatten() {
                if next != index {
                    assign(&mut depths, &mut queue, next, depth - rise(edge))?;
                }
            }
        }
    }
    depths.into_iter().collect()
}

/// Give a face its depth, or check it against the one it already has.
fn assign(
    depths: &mut [Option<i64>],
    queue: &mut VecDeque<usize>,
    face: usize,
    depth: i64,
) -> Option<()> {
    match depths.get_mut(face)? {
        Some(existing) if *existing != depth => None,
        Some(_) => Some(()),
        slot @ None => {
            *slot = Some(depth);
            queue.push_back(face);
            Some(())
        }
    }
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

fn adjacency(faces: &[[u32; 3]]) -> BTreeMap<[u32; 2], Vec<usize>> {
    let mut adjacent: BTreeMap<[u32; 2], Vec<usize>> = BTreeMap::new();
    for (index, face) in faces.iter().enumerate() {
        for edge in face_edges(*face) {
            adjacent.entry(key(edge)).or_default().push(index);
        }
    }
    adjacent
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

/// Per ring constraint: its role and the depth rise across its edges in ring
/// direction (+1 for a counter-clockwise boundary, -1 for a counter-clockwise
/// hole, negated for clockwise rings). A ring with zero signed area refuses.
fn ring_weights(prepared: &Prepared) -> Result<BTreeMap<u32, (Role, i64)>, Error> {
    let mut areas: BTreeMap<u32, (Role, f64)> = BTreeMap::new();
    for segment in prepared
        .segments
        .iter()
        .filter(|s| s.role != Role::Breakline)
    {
        let [a, b] = segment.ends.map(|vertex| prepared.position(vertex));
        let (a, b) = (a?, b?);
        let entry = areas
            .entry(segment.id.constraint)
            .or_insert((segment.role, 0.0));
        entry.1 += a[0] * b[1] - a[1] * b[0];
    }
    areas
        .into_iter()
        .map(|(constraint, (role, area))| {
            let orientation = if area > 0.0 {
                1
            } else if area < 0.0 {
                -1
            } else {
                return Err(Error::HoleClassificationFailed);
            };
            let sign = if role == Role::Hole { -1 } else { 1 };
            Ok((constraint, (role, sign * orientation)))
        })
        .collect()
}

pub(super) const fn face_edges([a, b, c]: [u32; 3]) -> [[u32; 2]; 3] {
    [[a, b], [b, c], [c, a]]
}

pub(super) const fn key([a, b]: [u32; 2]) -> [u32; 2] {
    if a < b { [a, b] } else { [b, a] }
}
