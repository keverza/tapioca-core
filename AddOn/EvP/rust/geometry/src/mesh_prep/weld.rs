//! First-encounter body-local welding, matching `VertexWelder::Add/Reset`.

use super::{Error, stored_normal};
use std::collections::BTreeMap;

/// A native-capture corner with a body-local source vertex and GIVEN normal.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct WeldCorner {
    /// Opaque signed body-local key, not an index into a supplied position pool.
    pub source_vertex: i32,
    /// World-metre position, retained from the first matching corner.
    pub position: [f64; 3],
    /// Dimensionless normal, stored as supplied (not normalized here).
    pub normal: [f64; 3],
}

/// Ordered Add calls partitioned into bodies by offsets, including final end.
pub struct WeldRequest<'a> {
    /// Sequential corners, in caller emission order.
    pub corners: &'a [WeldCorner],
    /// Nondecreasing offsets starting at zero and ending at `corners.len()`.
    /// Empty bodies are valid. Empty corners still require at least [0].
    pub body_offsets: &'a [u64],
}

/// Owned arrays plus the complete source-corner correspondence.
pub struct WeldedCorners {
    /// New vertices, f64 metres, first occurrence order, no coordinate averaging.
    pub positions: Vec<f64>,
    /// First given normal per welded vertex, narrowed to f32 as in C++.
    pub normals: Vec<f32>,
    /// One output vertex index for EVERY input corner. Reverse this map to find
    /// all contributors; first-corner-only provenance would lose contributors.
    pub corner_vertices: Vec<u32>,
}

/// Weld by (source vertex, rounded normal*1e4), resetting between bodies.
///
/// C++ lround rounds halfway AWAY from zero. Normal-key quantization is
/// dimensionless, not a position tolerance; coordinates are never welded by
/// proximity. Conflicting repeated positions retain the first occurrence, as in
/// the legacy Add oracle. All input corners are validated even if deduplicated.
/// Returned mapping is geometric source correspondence, not newly authored IDs;
/// C++ retains native/DAG provenance and must not use this as D14 generation.
///
/// # Errors
/// Malformed body offsets, >u32 index capacity, nonfinite input or an
/// unrepresentable rounded i32 normal key refuses the entire owned result.
pub fn weld_corners(input: &WeldRequest<'_>) -> Result<WeldedCorners, Error> {
    let count = input.corners.len();
    if u32::try_from(count).is_err()
        || count.checked_mul(3).is_none()
        || input.body_offsets.first() != Some(&0)
        || input.body_offsets.last().copied() != u64::try_from(count).ok()
        || input
            .body_offsets
            .iter()
            .zip(input.body_offsets.iter().skip(1))
            .any(|(a, b)| a > b)
    {
        return Err(Error::Shape);
    }
    let mut keys = Vec::with_capacity(count);
    for corner in input.corners {
        if !corner
            .position
            .iter()
            .chain(&corner.normal)
            .all(|x| x.is_finite())
        {
            return Err(Error::NonFinite);
        }
        let mut normal_key = [0_i32; 3];
        for (target, &normal) in normal_key.iter_mut().zip(&corner.normal) {
            let rounded = (normal * 1.0e4).round();
            if !rounded.is_finite()
                || rounded < f64::from(i32::MIN)
                || rounded > f64::from(i32::MAX)
            {
                return Err(Error::NormalQuantum);
            }
            *target = rounded_key(rounded);
        }
        keys.push((corner.source_vertex, normal_key));
    }
    let mut result = WeldedCorners {
        positions: Vec::new(),
        normals: Vec::new(),
        corner_vertices: Vec::with_capacity(count),
    };
    let mut map = BTreeMap::new();
    for (&start, &end) in input
        .body_offsets
        .iter()
        .zip(input.body_offsets.iter().skip(1))
    {
        map.clear();
        let start = usize::try_from(start).map_err(|_| Error::Shape)?;
        let end = usize::try_from(end).map_err(|_| Error::Shape)?;
        let corners = input.corners.get(start..end).ok_or(Error::Shape)?;
        let body_keys = keys.get(start..end).ok_or(Error::Shape)?;
        for (corner, &key) in corners.iter().zip(body_keys) {
            let index = if let Some(&index) = map.get(&key) {
                index
            } else {
                let index = u32::try_from(result.positions.len() / 3).map_err(|_| Error::Shape)?;
                result.positions.extend(corner.position);
                result.normals.extend(corner.normal.map(stored_normal));
                map.insert(key, index);
                index
            };
            result.corner_vertices.push(index);
        }
    }
    Ok(result)
}

#[expect(
    clippy::cast_possible_truncation,
    reason = "Caller checked rounded value finite and inclusively within i32 range"
)]
fn rounded_key(value: f64) -> i32 {
    value as i32
}
