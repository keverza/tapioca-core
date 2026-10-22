//! Native terrain solid -> plan surface, using the field's upward-face rule.
//! No re-triangulation, interpolation or proximity weld. Referenced vertices
//! merge only at numerically equal xy (signed zeros equal), with exactly equal
//! z required; the first input's coordinate bits win. Every contributing native
//! vertex and original face is retained. Input is a native height-field solid:
//! arbitrary overlapping upward sheets are not repaired or certified here.

use std::collections::{BTreeMap, BTreeSet};

use super::field::{Field, FieldError};
use super::plan;

/// Compact plan-view terrain and complete input support.
#[derive(Clone, Debug, PartialEq)]
pub struct Surface {
    /// xyz world metres, in first referenced input vertex order.
    pub points: Vec<f64>,
    /// Upward input faces in original order, with compact vertex indices.
    pub triangles: Vec<u32>,
    /// `(output vertex, input vertex)` for every referenced input vertex.
    pub vertices: Vec<[u32; 2]>,
    /// Original input face per output triangle.
    pub faces: Vec<u32>,
}

/// Surface extraction refusals.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SurfaceError {
    /// Invalid input or no upward face, as for the terrain field.
    Field(FieldError),
    /// Referenced vertices at one xy have distinct heights.
    MultipleHeights,
    /// Duplicate faces or an edge used inconsistently/by more than two faces.
    Topology,
}

impl SurfaceError {
    /// Stable diagnostic code and explanation.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::Field(error) => error.message(),
            Self::MultipleHeights => {
                "TerrainSurfaceMultipleHeights: upward faces have different heights at one xy"
            }
            Self::Topology => "TerrainSurfaceTopology: upward faces are duplicate or non-manifold",
        }
    }
}

/// Select and compact a native height-field solid's upward surface.
///
/// # Errors
/// See [`SurfaceError`]. No partial result is returned.
pub fn extract(points: &[f64], triangles: &[u32]) -> Result<Surface, SurfaceError> {
    let field = Field::new(points, triangles).map_err(SurfaceError::Field)?;
    let referenced: BTreeSet<u32> = field.surface_faces().flat_map(|(_, face)| face).collect();
    let input = plan::vertices(points);
    let mut lookup: BTreeMap<[u64; 2], (u32, f64)> = BTreeMap::new();
    let mut remap = BTreeMap::new();
    let mut output = Surface {
        points: Vec::new(),
        triangles: Vec::new(),
        vertices: Vec::new(),
        faces: Vec::new(),
    };
    for native in referenced {
        let &point = input
            .get(native as usize)
            .ok_or(SurfaceError::Field(FieldError::Index))?;
        let [x, y, z] = point;
        let key = [(x + 0.0).to_bits(), (y + 0.0).to_bits()];
        let vertex = if let Some(&(vertex, height)) = lookup.get(&key) {
            if (z + 0.0).to_bits() != (height + 0.0_f64).to_bits() {
                return Err(SurfaceError::MultipleHeights);
            }
            vertex
        } else {
            let vertex = u32::try_from(output.points.len() / 3)
                .map_err(|_| SurfaceError::Field(FieldError::Shape))?;
            output.points.extend(point);
            lookup.insert(key, (vertex, z));
            vertex
        };
        remap.insert(native, vertex);
        output.vertices.push([vertex, native]);
    }
    let mut faces = BTreeSet::new();
    let mut edges = BTreeMap::new();
    for (native, face) in field.surface_faces() {
        let [a, b, c] = face.map(|v| remap.get(&v).copied().ok_or(SurfaceError::Topology));
        let face = [a?, b?, c?];
        let mut key = face;
        key.sort_unstable();
        if !faces.insert(key) {
            return Err(SurfaceError::Topology);
        }
        for [a, b] in [[face[0], face[1]], [face[1], face[2]], [face[2], face[0]]] {
            let (count, direction) = edges.entry([a.min(b), a.max(b)]).or_insert((0, 0_i32));
            *count += 1;
            *direction += if a < b { 1 } else { -1 };
            if *count > 2 || (*count == 2 && *direction != 0) {
                return Err(SurfaceError::Topology);
            }
        }
        output.triangles.extend(face);
        output.faces.push(native);
    }
    Ok(output)
}

#[cfg(test)]
mod tests;
