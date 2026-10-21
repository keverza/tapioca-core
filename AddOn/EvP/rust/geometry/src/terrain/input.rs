//! Request validation, exact-position merging and constraint segments.

use std::collections::BTreeMap;

use super::{Error, Request, Role, SegmentRef};

/// One straight constraint segment between two merged output vertices.
#[derive(Clone, Copy, Debug)]
pub(super) struct Segment {
    pub id: SegmentRef,
    pub role: Role,
    pub ends: [u32; 2],
}

/// Validated request: unique positions (input order), the complete input remap
/// and every constraint segment in `(constraint, segment)` order.
pub(super) struct Prepared {
    pub positions: Vec<[f64; 3]>,
    pub input_vertices: Vec<u32>,
    pub segments: Vec<Segment>,
    lookup: BTreeMap<[u64; 2], u32>,
    z_tolerance: f64,
}

impl Prepared {
    /// Add (or reuse) the vertex at an exact plan position.
    ///
    /// Positions are equal only when their x and y are equal as numbers (`-0.0`
    /// equals `0.0`). A reused vertex must agree in z within the request's
    /// tolerance; it keeps its first z.
    pub(super) fn vertex_at(&mut self, position: [f64; 3]) -> Result<u32, Error> {
        let [x, y, z] = position;
        let key = [(x + 0.0).to_bits(), (y + 0.0).to_bits()];
        if let Some(&existing) = self.lookup.get(&key) {
            let first = self.position(existing)?;
            if (first[2] - z).abs() > self.z_tolerance {
                return Err(Error::TriangulationDegenerate);
            }
            return Ok(existing);
        }
        let index = u32::try_from(self.positions.len()).map_err(|_| Error::Shape)?;
        self.positions.push(position);
        self.lookup.insert(key, index);
        Ok(index)
    }

    pub(super) fn position(&self, vertex: u32) -> Result<[f64; 3], Error> {
        usize::try_from(vertex)
            .ok()
            .and_then(|index| self.positions.get(index))
            .copied()
            .ok_or(Error::TriangulationDegenerate)
    }

    pub(super) const fn z_tolerance(&self) -> f64 {
        self.z_tolerance
    }
}

pub(super) fn prepare(request: &Request<'_>) -> Result<Prepared, Error> {
    let (points, remainder) = request.points.as_chunks::<3>();
    if !remainder.is_empty() || u32::try_from(points.len()).is_err() {
        return Err(Error::Shape);
    }
    if !request.points.iter().all(|value| value.is_finite()) {
        return Err(Error::NonFinite);
    }
    if !request.z_tolerance.is_finite() || request.z_tolerance < 0.0 {
        return Err(Error::Tolerance);
    }
    if u32::try_from(request.constraints.len()).is_err() {
        return Err(Error::Shape);
    }
    let mut prepared = Prepared {
        positions: Vec::with_capacity(points.len()),
        input_vertices: Vec::with_capacity(points.len()),
        segments: Vec::new(),
        lookup: BTreeMap::new(),
        z_tolerance: request.z_tolerance,
    };
    for point in points {
        let vertex = prepared.vertex_at(*point)?;
        prepared.input_vertices.push(vertex);
    }
    for (constraint, item) in (0_u32..).zip(request.constraints) {
        add_segments(&mut prepared, constraint, item.role, item.vertices)?;
    }
    if prepared.positions.len() < 3 {
        return Err(Error::TriangulationDegenerate);
    }
    Ok(prepared)
}

fn add_segments(
    prepared: &mut Prepared,
    constraint: u32,
    role: Role,
    vertices: &[u32],
) -> Result<(), Error> {
    let closed = role != Role::Breakline;
    let minimum = if closed { 3 } else { 2 };
    if vertices.len() < minimum || u32::try_from(vertices.len()).is_err() {
        return Err(Error::Shape);
    }
    let mapped = vertices
        .iter()
        .map(|&input| {
            usize::try_from(input)
                .ok()
                .and_then(|index| prepared.input_vertices.get(index))
                .copied()
                .ok_or(Error::Index)
        })
        .collect::<Result<Vec<_>, _>>()?;
    let closing = mapped.first().copied().filter(|_| closed);
    let ends = mapped
        .iter()
        .copied()
        .zip(mapped.iter().copied().skip(1).chain(closing));
    for (segment, (from, to)) in (0_u32..).zip(ends) {
        if from == to {
            return Err(Error::TriangulationDegenerate);
        }
        prepared.segments.push(Segment {
            id: SegmentRef {
                constraint,
                segment,
            },
            role,
            ends: [from, to],
        });
    }
    Ok(())
}
