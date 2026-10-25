//! An immutable triangle scene with its bounding-volume hierarchy.

use std::ops::ControlFlow;

use tapioca_geometry::ray::{MAX_TRIANGLES, PreparedRay, Triangle, TriangleIndex};

mod bvh;
#[cfg(test)]
mod tests;

use bvh::Bvh;

/// Borrowed scene arrays: world-metre vertices (`x, y, z` per vertex) and
/// triangles (three vertex indices each), as the C++ engine concatenates a
/// snapshot's meshes.
#[derive(Clone, Copy, Debug)]
pub struct SceneInput<'a> {
    /// `3 * vertex_count` coordinates, finite.
    pub vertices: &'a [f64],
    /// `3 * triangle_count` indices into the vertices.
    pub triangles: &'a [u32],
    /// Identity of the geometry (the snapshot id); 0 means no scene.
    pub version: u64,
}

/// Why a scene was refused.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SceneError {
    /// The vertex array is not whole `x, y, z` triples.
    VertexShape,
    /// The triangle array is not whole index triples.
    TriangleShape,
    /// More triangles than the engine addresses.
    TooManyTriangles,
    /// A coordinate is NaN or infinite.
    NonFiniteVertex,
    /// A triangle names a vertex that does not exist.
    IndexOutOfRange,
}

impl SceneError {
    /// A stable, human-readable reason.
    #[must_use]
    pub fn message(self) -> &'static str {
        match self {
            Self::VertexShape => "scene vertices are not whole x, y, z triples",
            Self::TriangleShape => "scene triangles are not whole index triples",
            Self::TooManyTriangles => "scene has more triangles than the engine addresses",
            Self::NonFiniteVertex => "scene has a NaN or infinite coordinate",
            Self::IndexOutOfRange => "scene triangle names a vertex that does not exist",
        }
    }
}

/// The scene: triangles in tree order, their original indices, the tree.
#[derive(Debug)]
pub struct Scene {
    triangles: Vec<Triangle>,
    ids: Vec<u32>,
    bvh: Bvh,
    version: u64,
}

impl Scene {
    /// Validate `input` and build its hierarchy. Degenerate triangles are
    /// kept (they never hit), as the C++ engine keeps them.
    ///
    /// # Errors
    /// [`SceneError`] for malformed arrays, non-finite coordinates, an index
    /// out of range, or more than `MAX_TRIANGLES` triangles.
    pub fn build(input: &SceneInput<'_>) -> Result<Self, SceneError> {
        if !input.vertices.len().is_multiple_of(3) {
            return Err(SceneError::VertexShape);
        }
        if !input.triangles.len().is_multiple_of(3) {
            return Err(SceneError::TriangleShape);
        }
        if input.triangles.len() / 3 > MAX_TRIANGLES {
            return Err(SceneError::TooManyTriangles);
        }
        if input.vertices.iter().any(|v| !v.is_finite()) {
            return Err(SceneError::NonFiniteVertex);
        }
        let points: Vec<[f64; 3]> = input.vertices.as_chunks::<3>().0.to_vec();
        let mut source = Vec::with_capacity(input.triangles.len() / 3);
        for corners in input.triangles.as_chunks::<3>().0 {
            let mut triangle = [[0.0; 3]; 3];
            for (slot, &corner) in triangle.iter_mut().zip(corners) {
                *slot = *usize::try_from(corner)
                    .ok()
                    .and_then(|i| points.get(i))
                    .ok_or(SceneError::IndexOutOfRange)?;
            }
            source.push(triangle);
        }
        let (bvh, order) = Bvh::build(&source);
        let triangles = order
            .iter()
            .filter_map(|&i| source.get(i).copied())
            .collect();
        let ids = order
            .iter()
            .filter_map(|&i| u32::try_from(i).ok())
            .collect();
        Ok(Self {
            triangles,
            ids,
            bvh,
            version: input.version,
        })
    }

    /// The geometry identity this scene was built for.
    #[must_use]
    pub fn version(&self) -> u64 {
        self.version
    }

    /// Tree nodes, for diagnostics and build measurements.
    #[must_use]
    pub fn node_count(&self) -> usize {
        self.bvh.node_count()
    }
}

impl TriangleIndex for Scene {
    fn triangle_count(&self) -> usize {
        self.triangles.len()
    }

    fn visit(
        &self,
        ray: &PreparedRay,
        visitor: &mut dyn FnMut(u32, &Triangle) -> ControlFlow<()>,
    ) -> ControlFlow<()> {
        self.bvh.visit(ray, &mut |first, count| {
            let leaf = self.triangles.iter().zip(&self.ids).skip(first).take(count);
            for (triangle, &id) in leaf {
                visitor(id, triangle)?;
            }
            ControlFlow::Continue(())
        })
    }
}
