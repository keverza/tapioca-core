//! Fixed-height control points and polylines for the hybrid TIN. These are
//! constraints, not grading tools: incompatible heights refuse, never generate
//! an implicit ramp or silently flatten a band. Surface/area locks and the
//! soft-tier consumer are separate increments.

use std::collections::BTreeMap;

use super::{HybridError, index};
use crate::terrain::plan;
use crate::terrain::region::RegionMesh;

/// Borrowed fixed-height controls, world xyz metres. Lines are open polylines
/// with at least two vertices; points are whole xyz triples. All must lie
/// strictly inside the rebuild ring. Heights interpolate linearly on a line;
/// every crossing/base-edge split vertex is constrained, not just its endpoints.
#[derive(Clone, Copy, Debug, Default)]
pub struct Locked<'a> {
    /// Fixed-height polylines in authored order.
    pub lines: &'a [&'a [f64]],
    /// Independent fixed-height points in authored order.
    pub points: &'a [f64],
}

/// A control's locator in the borrowed input; no semantic IDs are allocated.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LockRef {
    /// Segment of a fixed-height polyline.
    Segment {
        /// Line in [`Locked::lines`].
        line: u32,
        /// Segment from vertex `segment` to `segment + 1`.
        segment: u32,
    },
    /// Point in [`Locked::points`] (vertex index, not scalar offset).
    Point(u32),
}

/// One constrained output vertex, sorted by output index. Complete contributor
/// lists retain coincident controls and overlapping/crossing segments. Compatible
/// heights within the request's absolute height tolerance keep the first input
/// height (lines, then points); endpoints/points keep their supplied height bits.
/// Daylight vertices must additionally match the base height bit-for-bit.
#[derive(Clone, Debug, PartialEq)]
pub struct LockedVertex {
    /// Output vertex in the built mesh.
    pub vertex: u32,
    /// The fixed height, world metres.
    pub height: f64,
    /// Every contributing control, in deterministic input traversal order.
    pub supports: Vec<LockRef>,
}

/// A hard, located `LockedConflict`; no partial surface is published.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct LockedConflict {
    /// The first control at the vertex.
    pub control: LockRef,
    /// Another disagreeing control, or `None` for the proposed grading surface.
    pub other: Option<LockRef>,
    /// Output vertex (in the refused candidate's topology).
    pub vertex: u32,
    /// Position at the first control's fixed height, world metres.
    pub position: [f64; 3],
    /// Other/proposed height minus the fixed height, metres.
    pub magnitude: f64,
}

impl LockedConflict {
    /// Stable conflict code.
    #[must_use]
    pub const fn name(self) -> &'static str {
        "LockedConflict"
    }

    /// Suggested authoring fix; constraints are never silently relaxed.
    #[must_use]
    pub const fn suggestion(self) -> &'static str {
        if self.other.is_some() {
            "align the fixed elevations of the intersecting controls"
        } else {
            "narrow the band or change its elevation to meet the fixed control"
        }
    }
}

pub(super) fn validate(locked: Locked<'_>) -> Result<(), HybridError> {
    let valid = |points: &[f64]| {
        points.len().is_multiple_of(3) && points.iter().all(|value| value.is_finite())
    };
    if !valid(locked.points)
        || u32::try_from(locked.points.len() / 3).is_err()
        || u32::try_from(locked.lines.len()).is_err()
        || locked.lines.iter().any(|line| {
            !valid(line)
                || line.len() < 6
                || u32::try_from(line.len() / 3).is_err()
                || plan::vertices(line).windows(2).any(
                    |pair| matches!(pair, [a, b] if (b[0] - a[0]).hypot(b[1] - a[1]) <= plan::ON),
                )
        })
    {
        return Err(HybridError::LockedParameters);
    }
    Ok(())
}

fn at(mesh: &RegionMesh, vertex: u32) -> Result<[f64; 3], HybridError> {
    plan::vertices(&mesh.points)
        .get(vertex as usize)
        .copied()
        .ok_or(HybridError::LockedParameters)
}

/// Use the dominant plan axis, avoiding squared-length overflow. Endpoint
/// branches retain the authored z bits rather than round-tripping through lerp.
fn height(first: [f64; 3], last: [f64; 3], point: [f64; 3]) -> Result<f64, HybridError> {
    let axis = usize::from((last[1] - first[1]).abs() > (last[0] - first[0]).abs());
    let (Some(&start), Some(&end), Some(&coordinate)) =
        (first.get(axis), last.get(axis), point.get(axis))
    else {
        return Err(HybridError::LockedParameters);
    };
    let t = ((coordinate - start) / (end - start)).clamp(0.0, 1.0);
    let z = if t <= 0.0 {
        first[2]
    } else if t >= 1.0 {
        last[2]
    } else {
        (1.0 - t) * first[2] + t * last[2]
    };
    if !z.is_finite() {
        return Err(HybridError::LockedParameters);
    }
    Ok(z)
}

#[derive(Default)]
pub(super) struct Diagnostics {
    pub rows: Vec<LockedConflict>,
    pub exhaustive: bool,
}

impl Diagnostics {
    fn report(&mut self, conflict: LockedConflict) -> Result<(), HybridError> {
        if !self.exhaustive {
            return Err(HybridError::LockedConflict(conflict));
        }
        self.rows.push(conflict);
        Ok(())
    }
}

struct Controls<'a> {
    mesh: &'a RegionMesh,
    tolerance: f64,
    vertices: BTreeMap<u32, LockedVertex>,
    conflicts: &'a mut Diagnostics,
}

impl Controls<'_> {
    fn insert(&mut self, vertex: u32, z: f64, control: LockRef) -> Result<(), HybridError> {
        let [x, y, _] = at(self.mesh, vertex)?;
        if let Some(first) = self.vertices.get_mut(&vertex) {
            // A split segment occurs at both ends of adjacent edge rows. It is
            // one contributor, not a second conflict with itself.
            if first.supports.contains(&control) {
                return Ok(());
            }
            let magnitude = z - first.height;
            if !magnitude.is_finite() {
                return Err(HybridError::LockedParameters);
            }
            if magnitude.abs() > self.tolerance {
                let conflict = LockedConflict {
                    control: *first
                        .supports
                        .first()
                        .ok_or(HybridError::LockedParameters)?,
                    other: Some(control),
                    vertex,
                    position: [x, y, first.height],
                    magnitude,
                };
                self.conflicts.report(conflict)?;
            }
            first.supports.push(control);
        } else {
            self.vertices.insert(
                vertex,
                LockedVertex {
                    vertex,
                    height: z,
                    supports: vec![control],
                },
            );
        }
        Ok(())
    }
}

/// Resolve all controls before comparing to grading. A line's split edges
/// already include its intersections with every other line and base edge.
pub(super) fn resolve(
    mesh: &RegionMesh,
    locked: Locked<'_>,
    tolerance: f64,
    conflicts: &mut Diagnostics,
) -> Result<Vec<LockedVertex>, HybridError> {
    let mut controls = Controls {
        mesh,
        tolerance,
        vertices: BTreeMap::new(),
        conflicts,
    };
    for edge in &mesh.line_edges {
        let Some(line) = locked.lines.get(edge.line as usize) else {
            continue; // Design lines follow all the locked lines.
        };
        let segment = edge.segment as usize;
        let vertices = plan::vertices(line);
        let (Some(&a), Some(&b)) = (vertices.get(segment), vertices.get(segment + 1)) else {
            return Err(HybridError::LockedParameters);
        };
        for vertex in edge.vertices {
            let z = height(a, b, at(mesh, vertex)?)?;
            controls.insert(
                vertex,
                z,
                LockRef::Segment {
                    line: edge.line,
                    segment: edge.segment,
                },
            )?;
        }
    }
    if mesh.point_vertices.len() != locked.points.len() / 3 {
        return Err(HybridError::LockedParameters);
    }
    for (point, (&vertex, &[_, _, z])) in mesh
        .point_vertices
        .iter()
        .zip(plan::vertices(locked.points))
        .enumerate()
    {
        controls.insert(vertex, z, LockRef::Point(index(point)?))?;
    }
    Ok(controls.vertices.into_values().collect())
}

/// Check the complete fixed tier before changing even one proposed height.
/// Successful matching within tolerance uses the fixed value exactly, not a
/// blend or a penalty. A changed fixed value has its own attribution, never the
/// band's height owner.
pub(super) fn apply(
    points: &mut [f64],
    locks: &[LockedVertex],
    tolerance: f64,
    fixed: &[bool],
    conflicts: &mut Diagnostics,
) -> Result<(), HybridError> {
    for lock in locks {
        let point = plan::vertices(points)
            .get(lock.vertex as usize)
            .ok_or(HybridError::LockedParameters)?;
        let magnitude = point[2] - lock.height;
        if !magnitude.is_finite() {
            return Err(HybridError::LockedParameters);
        }
        // The region exterior and daylight loci have a stronger bitwise base
        // preservation contract; a compatible lock must not weaken it.
        let exact = fixed
            .get(lock.vertex as usize)
            .copied()
            .ok_or(HybridError::LockedParameters)?;
        if magnitude.abs() > tolerance || (exact && point[2].to_bits() != lock.height.to_bits()) {
            conflicts.report(LockedConflict {
                control: *lock.supports.first().ok_or(HybridError::LockedParameters)?,
                other: None,
                vertex: lock.vertex,
                position: [point[0], point[1], lock.height],
                magnitude,
            })?;
        }
    }
    if let Some(&first) = conflicts.rows.first() {
        return Err(HybridError::LockedConflict(first));
    }
    for lock in locks {
        let z = points
            .get_mut(lock.vertex as usize * 3 + 2)
            .ok_or(HybridError::LockedParameters)?;
        *z = lock.height;
    }
    Ok(())
}

#[cfg(test)]
mod tests;
