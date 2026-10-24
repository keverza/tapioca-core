//! Proving which way a face points before anything measures it.
//!
//! Port of `SunStudy/SunStudyWinding.cpp`. Every answer rests on the face normal:
//! an inward-wound element reports its sunlit face as dark and lifts its samples
//! into the solid. Winding is consistent only within an element, so the test is
//! per group and is a proof, not a heuristic: the group must be closed (every
//! undirected welded edge shared by exactly two faces) and then the sign of the
//! divergence-theorem volume is the winding. A wrong flip is far worse than a
//! missed one, so anything short of a closed surface is left as it came.

use std::collections::BTreeMap;

/// Counts of what [`orient_outward`] saw and changed.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct WindingReport {
    /// Distinct group ids with at least one usable face.
    pub groups: usize,
    /// Groups that proved closed, so were testable.
    pub closed: usize,
    /// Groups whose volume came out negative and were flipped.
    pub flipped: usize,
    /// Triangles whose winding was swapped.
    pub flipped_triangles: usize,
}

/// Refusals for inputs the C++ original would read out of bounds.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// `groups` has fewer entries than there are faces.
    GroupCount,
}

impl Error {
    /// Stable diagnostic text for the C ABI.
    #[must_use]
    pub fn message(self) -> &'static str {
        match self {
            Self::GroupCount => "WindingGroupCount: expected one group id per face",
        }
    }
}

/// Default weld tolerance in metres.
pub const DEFAULT_WELD_TOLERANCE: f64 = 1e-4;

type Lattice = [i64; 3];

/// A welded position on the tolerance lattice. Rounds half away from zero as
/// `llround`; NaN and out-of-range values saturate (they are
/// implementation-defined in C++ and never prove an edge shared).
#[expect(
    clippy::cast_possible_truncation,
    reason = "saturating float-to-int lattice key; finite in-range values match llround"
)]
fn quantise(p: [f64; 3], inverse: f64) -> Lattice {
    p.map(|c| (c * inverse).round() as i64)
}

fn edge(p: Lattice, q: Lattice) -> (Lattice, Lattice) {
    if q < p { (q, p) } else { (p, q) }
}

fn corner(vertices: &[f64], index: u32) -> Option<[f64; 3]> {
    let start = usize::try_from(index).ok()?.checked_mul(3)?;
    match vertices.get(start..start.checked_add(3)?)? {
        [x, y, z] => Some([*x, *y, *z]),
        _ => None,
    }
}

fn face_corners(vertices: &[f64], triangles: &[u32], face: usize) -> Option<[[f64; 3]; 3]> {
    let start = face.checked_mul(3)?;
    match triangles.get(start..start.checked_add(3)?)? {
        [a, b, c] => Some([
            corner(vertices, *a)?,
            corner(vertices, *b)?,
            corner(vertices, *c)?,
        ]),
        _ => None,
    }
}

/// Signed volume of a closed triangle soup over `faces`, by the divergence
/// theorem. Vertices are centred first: at survey coordinates the triple
/// product runs to ~1e17 against a ~1e2 volume and the sign would be noise.
/// Faces with an out-of-range index contribute nothing.
#[must_use]
pub fn signed_volume(vertices: &[f64], triangles: &[u32], faces: &[usize]) -> f64 {
    let mut centre = [0.0; 3];
    let mut corners = 0_usize;
    for &face in faces {
        for p in face_corners(vertices, triangles, face)
            .into_iter()
            .flatten()
        {
            for (sum, value) in centre.iter_mut().zip(p) {
                *sum += value;
            }
            corners += 1;
        }
    }
    if corners == 0 {
        return 0.0;
    }
    let count = crate::series::count_to_f64(corners);
    let centre = centre.map(|sum| sum / count);
    let mut total = 0.0;
    for &face in faces {
        let Some([p0, p1, p2]) = face_corners(vertices, triangles, face) else {
            continue;
        };
        let shift = |p: [f64; 3]| [p[0] - centre[0], p[1] - centre[1], p[2] - centre[2]];
        let (a, b, c) = (shift(p0), shift(p1), shift(p2));
        let cross = [
            b[1] * c[2] - b[2] * c[1],
            b[2] * c[0] - b[0] * c[2],
            b[0] * c[1] - b[1] * c[0],
        ];
        total += a[0] * cross[0] + a[1] * cross[1] + a[2] * cross[2];
    }
    total / 6.0
}

/// Return a copy of `triangles` with inward-wound closed groups swapped.
///
/// `groups` holds one element id per face, or `None` for "all one element".
/// Adjacency is by welded position, not index: the snapshot merges elements
/// without sharing vertices. A non-positive or NaN `weld_tolerance` falls back
/// to [`DEFAULT_WELD_TOLERANCE`]. Faces with an out-of-range index are copied
/// unchanged and ignored by the proof. Empty vertices or triangles give an
/// empty result.
///
/// # Errors
/// [`Error::GroupCount`] when `groups` is shorter than the face count.
pub fn orient_outward(
    vertices: &[f64],
    triangles: &[u32],
    groups: Option<&[u32]>,
    weld_tolerance: f64,
) -> Result<(Vec<u32>, WindingReport), Error> {
    let face_count = triangles.len() / 3;
    if groups.is_some_and(|g| g.len() < face_count) {
        return Err(Error::GroupCount);
    }
    let mut report = WindingReport::default();
    if vertices.is_empty() || face_count == 0 {
        return Ok((Vec::new(), report));
    }
    let tolerance = if weld_tolerance > 0.0 {
        weld_tolerance
    } else {
        DEFAULT_WELD_TOLERANCE
    };
    let inverse = 1.0 / tolerance;
    let mut oriented = triangles.get(..face_count * 3).unwrap_or_default().to_vec();

    let mut by_group: BTreeMap<u32, Vec<usize>> = BTreeMap::new();
    for face in 0..face_count {
        if face_corners(vertices, triangles, face).is_none() {
            continue; // an unusable face cannot help prove a group closed
        }
        let id = groups.and_then(|g| g.get(face).copied()).unwrap_or(0);
        by_group.entry(id).or_default().push(face);
    }
    report.groups = by_group.len();

    for faces in by_group.values() {
        let mut edges: BTreeMap<(Lattice, Lattice), u32> = BTreeMap::new();
        for &face in faces {
            let Some(points) = face_corners(vertices, triangles, face) else {
                continue;
            };
            let [k0, k1, k2] = points.map(|p| quantise(p, inverse));
            for key in [edge(k0, k1), edge(k1, k2), edge(k2, k0)] {
                *edges.entry(key).or_insert(0) += 1;
            }
        }
        if edges.values().any(|&uses| uses != 2) {
            continue; // open surface: "outward" is undefined
        }
        report.closed += 1;
        if signed_volume(vertices, triangles, faces) >= 0.0 {
            continue;
        }
        report.flipped += 1;
        for &face in faces {
            if let Some(tri) = oriented.get_mut(face * 3..face * 3 + 3) {
                tri.swap(1, 2);
                report.flipped_triangles += 1;
            }
        }
    }
    Ok((oriented, report))
}

#[cfg(test)]
mod tests;
