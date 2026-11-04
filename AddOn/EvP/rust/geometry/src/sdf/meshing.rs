//! Deterministic shared-edge Marching Cubes over dense or sparse samples.
//!
//! This clean-room implementation constructs intersection loops from cube-face
//! crossings instead of importing a triangle lookup table. Checkerboard faces
//! use the exact field at the face center, identically on both adjacent cells.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt;

use super::{
    GridSpec, PreparedField, SampledGrid, SamplingError, SamplingLimits, SamplingStats,
    SamplingStrategy, field, sample_with_cancellation, sampling::sampling_preflight,
};
use crate::affine::Vec3;

const CORNERS: [[u32; 3]; 8] = [
    [0, 0, 0],
    [1, 0, 0],
    [1, 1, 0],
    [0, 1, 0],
    [0, 0, 1],
    [1, 0, 1],
    [1, 1, 1],
    [0, 1, 1],
];
const EDGES: [[usize; 2]; 12] = [
    [0, 1],
    [1, 2],
    [2, 3],
    [3, 0],
    [4, 5],
    [5, 6],
    [6, 7],
    [7, 4],
    [0, 4],
    [1, 5],
    [2, 6],
    [3, 7],
];
const FACES: [Face; 6] = [
    Face {
        corners: [0, 1, 2, 3],
        edges: [0, 1, 2, 3],
    },
    Face {
        corners: [4, 5, 6, 7],
        edges: [4, 5, 6, 7],
    },
    Face {
        corners: [0, 1, 5, 4],
        edges: [0, 9, 4, 8],
    },
    Face {
        corners: [1, 2, 6, 5],
        edges: [1, 10, 5, 9],
    },
    Face {
        corners: [2, 3, 7, 6],
        edges: [2, 11, 6, 10],
    },
    Face {
        corners: [3, 0, 4, 7],
        edges: [3, 8, 7, 11],
    },
];
const SLAB_CELLS: u32 = 64;
const CANCEL_POLL_INTERVAL: usize = 256;

#[derive(Clone, Copy)]
struct Face {
    corners: [usize; 4],
    edges: [usize; 4],
}

/// Per-job meshing ceiling. Values above the SDF-0 hard ceiling are capped.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MeshingLimits {
    /// Maximum emitted nondegenerate triangles.
    pub max_triangles: u64,
}

impl Default for MeshingLimits {
    fn default() -> Self {
        Self {
            max_triangles: 2_000_000,
        }
    }
}

/// Combined sampling and meshing budgets for a complete build.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct BuildLimits {
    /// Dense/stored-sparse sampling ceilings.
    pub sampling: SamplingLimits,
    /// Triangle ceiling.
    pub meshing: MeshingLimits,
}

/// Marching Cubes work/output counters.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MeshingStats {
    /// Total grid cells.
    pub cells: u64,
    /// Cells with both inside and outside corners.
    pub active_cells: u64,
    /// Emitted nondegenerate triangles.
    pub triangles: u64,
    /// Fan triangles omitted because their indices/positions are degenerate.
    pub degenerate_triangles: u64,
    /// Fixed X-slab width; independent of machine core count.
    pub slab_cells: u32,
}

/// Complete deterministic triangle mesh in world metres.
#[derive(Clone, Debug, PartialEq)]
pub struct MeshResult {
    /// Interleaved f64 xyz positions.
    pub positions: Vec<f64>,
    /// Interleaved f32 xyz unit normals, one per position vertex.
    pub normals: Vec<f32>,
    /// Three vertex indices per nondegenerate triangle.
    pub triangles: Vec<u32>,
    /// One positive material pool ID per triangle.
    pub tri_material: Vec<i32>,
    /// FNV-1a content hash matching the published-mesh channel order.
    pub content_hash: u64,
    /// Meshing counters.
    pub stats: MeshingStats,
    /// Sampling diagnostics that produced this mesh.
    pub sampling: SamplingStats,
}

/// Meshing refusal. The partially built mesh remains private and is discarded.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum MeshingError {
    /// Sampling input construction failed.
    Sampling(SamplingError),
    /// Field material/gradient evaluation failed.
    Field(field::Error),
    /// Cooperative cancellation was observed during meshing.
    Cancelled,
    /// Grid cell arithmetic overflowed.
    CellBudget,
    /// Automatic meshing requires finite prepared field bounds.
    BoundsRequired,
    /// The field is inside or on at least one meshing-domain boundary node.
    SurfaceTouchesBounds,
    /// Vertex indices exceed the `u32` output contract.
    VertexBudget,
    /// The triangle ceiling was reached; no partial mesh is returned.
    TriangleBudget,
    /// No nondegenerate isosurface was found.
    EmptySurface,
    /// A required sampled corner is absent or internally inconsistent.
    MissingValue,
    /// Face crossings or emitted mesh edges did not form a closed oriented surface.
    InvalidTopology,
    /// A computed position/normal/centroid is non-finite.
    NonFinite,
    /// A usable field gradient could not orient or shade a surface.
    NormalUnavailable,
}

impl MeshingError {
    /// Stable allocation-free diagnostic prefix and description.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::Sampling(error) => error.message(),
            Self::Field(error) => error.message(),
            Self::Cancelled => "Cancelled: meshing was cancelled; no partial mesh is returned",
            Self::CellBudget => "CellBudget: grid cell arithmetic overflowed",
            Self::BoundsRequired => "BoundsRequired: automatic meshing needs finite field bounds",
            Self::SurfaceTouchesBounds => {
                "SurfaceTouchesBounds: every meshing-domain boundary node must be outside"
            }
            Self::VertexBudget => "VertexBudget: mesh vertices exceed the u32 index contract",
            Self::TriangleBudget => "TriangleBudget: mesh triangles exceed their ceiling",
            Self::EmptySurface => "EmptySurface: no nondegenerate zero isosurface was found",
            Self::MissingValue => "MissingValue: a required grid corner has no sampled value",
            Self::InvalidTopology => {
                "InvalidTopology: cube crossings or mesh edges do not form a closed oriented surface"
            }
            Self::NonFinite => "NonFinite: mesh arithmetic produced NaN or infinity",
            Self::NormalUnavailable => "NormalUnavailable: no usable field gradient at the surface",
        }
    }
}

impl fmt::Display for MeshingError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.message())
    }
}

impl From<SamplingError> for MeshingError {
    fn from(error: SamplingError) -> Self {
        Self::Sampling(error)
    }
}

impl From<field::Error> for MeshingError {
    fn from(error: field::Error) -> Self {
        Self::Field(error)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
struct GridEdge {
    a: [u32; 3],
    b: [u32; 3],
}

#[derive(Clone, Copy)]
struct EdgePosition {
    key: GridEdge,
    position: Vec3,
}

struct MeshBuilder<'a> {
    field: &'a PreparedField,
    sampled: &'a SampledGrid,
    limits: MeshingLimits,
    cancel: Option<&'a dyn Fn() -> bool>,
    normal_step: f64,
    vertices: Vec<Vec3>,
    edge_vertices: BTreeMap<GridEdge, u32>,
    node_vertices: BTreeMap<[u32; 3], u32>,
    triangles: Vec<u32>,
    materials: Vec<i32>,
    stats: MeshingStats,
}

/// Sample and mesh a prepared field using its automatic conservative bounds,
/// padded by two requested cells on every side. Preview and final callers share
/// this function and differ only in voxel size/strategy and budgets.
///
/// # Errors
/// Missing bounds, invalid voxel size, sampling failures, cancellation and mesh
/// failures return no partial result. An unbounded field must instead supply an
/// authoritative grid through the lower-level sampling/meshing calls.
pub fn build_mesh(
    field: &PreparedField,
    voxel_size: f64,
    strategy: SamplingStrategy,
    limits: BuildLimits,
    cancel: &super::CancellationToken,
) -> Result<MeshResult, MeshingError> {
    let poll = || cancel.is_cancelled();
    build_mesh_with_cancellation(field, voxel_size, strategy, limits, Some(&poll))
}

/// Callback-driven variant of [`build_mesh`] for the host-independent FFI seam.
///
/// # Errors
/// Same atomic bounds/sampling/meshing/cancellation contract as [`build_mesh`].
pub fn build_mesh_with_cancellation(
    field: &PreparedField,
    voxel_size: f64,
    strategy: SamplingStrategy,
    limits: BuildLimits,
    cancel: Option<&dyn Fn() -> bool>,
) -> Result<MeshResult, MeshingError> {
    if !voxel_size.is_finite() || voxel_size <= 0.0 {
        return Err(MeshingError::Sampling(SamplingError::InvalidVoxelSize));
    }
    let bounds = field.bounds().ok_or(MeshingError::BoundsRequired)?;
    let padded = bounds
        .inflated(2.0 * voxel_size)
        .ok_or(MeshingError::BoundsRequired)?;
    let grid = GridSpec::new(padded, voxel_size)?;
    sampling_preflight(grid, strategy, limits.sampling)?;
    preflight_boundary(field, grid, cancel)?;
    let sampled = sample_with_cancellation(field, grid, strategy, limits.sampling, cancel)?;
    march_sampled(field, &sampled, limits.meshing, cancel)
}

fn preflight_boundary(
    field: &PreparedField,
    grid: GridSpec,
    cancel: Option<&dyn Fn() -> bool>,
) -> Result<(), MeshingError> {
    for axis in 0..3 {
        let first = (axis + 1) % 3;
        let second = (axis + 2) % 3;
        let axis_cells = *grid.cells.get(axis).ok_or(MeshingError::MissingValue)?;
        let first_cells = *grid.cells.get(first).ok_or(MeshingError::MissingValue)?;
        let second_cells = *grid.cells.get(second).ok_or(MeshingError::MissingValue)?;
        for side in [0, axis_cells] {
            for a in 0..=first_cells {
                for b in 0..=second_cells {
                    if cancel.is_some_and(|poll| poll()) {
                        return Err(MeshingError::Cancelled);
                    }
                    let mut node = [0_u32; 3];
                    *node.get_mut(axis).ok_or(MeshingError::MissingValue)? = side;
                    *node.get_mut(first).ok_or(MeshingError::MissingValue)? = a;
                    *node.get_mut(second).ok_or(MeshingError::MissingValue)? = b;
                    let point = grid.point(node).ok_or(MeshingError::MissingValue)?;
                    if field.evaluate(point)?.distance <= 0.0 {
                        return Err(MeshingError::SurfaceTouchesBounds);
                    }
                }
            }
        }
    }
    Ok(())
}

/// Mesh a complete sampled field. Cells are visited in fixed 64-cell X slabs,
/// then z/y/x order inside each slab. Shared grid edges and exact grid-node
/// crossings use canonical maps, so slab boundaries never duplicate vertices.
/// Every boundary node must have a strictly positive exact field distance.
///
/// # Errors
/// Boundary contact, cancellation, missing samples, invalid loops, non-finite
/// output and budget excess discard the complete candidate. No partial mesh is
/// returned.
pub fn marching_cubes(
    field: &PreparedField,
    sampled: &SampledGrid,
    limits: MeshingLimits,
    cancel: &super::CancellationToken,
) -> Result<MeshResult, MeshingError> {
    let poll = || cancel.is_cancelled();
    marching_cubes_with_cancellation(field, sampled, limits, Some(&poll))
}

/// Callback-driven cancellation variant of [`marching_cubes`].
///
/// # Errors
/// Same atomic topology/geometry/budget/cancellation contract as
/// [`marching_cubes`].
pub fn marching_cubes_with_cancellation(
    field: &PreparedField,
    sampled: &SampledGrid,
    limits: MeshingLimits,
    cancel: Option<&dyn Fn() -> bool>,
) -> Result<MeshResult, MeshingError> {
    preflight_boundary(field, sampled.grid(), cancel)?;
    march_sampled(field, sampled, limits, cancel)
}

fn march_sampled(
    field: &PreparedField,
    sampled: &SampledGrid,
    limits: MeshingLimits,
    cancel: Option<&dyn Fn() -> bool>,
) -> Result<MeshResult, MeshingError> {
    let grid = sampled.grid();
    let cells = grid
        .cells
        .into_iter()
        .map(u64::from)
        .try_fold(1_u64, |total, count| {
            total.checked_mul(count).ok_or(MeshingError::CellBudget)
        })?;
    let normal_step = grid.spacing.into_iter().fold(f64::INFINITY, f64::min) * 1e-3;
    if !normal_step.is_finite() || normal_step <= 0.0 {
        return Err(MeshingError::NormalUnavailable);
    }
    let mut builder = MeshBuilder {
        field,
        sampled,
        limits: MeshingLimits {
            max_triangles: limits.max_triangles.min(2_000_000),
        },
        cancel,
        normal_step,
        vertices: Vec::new(),
        edge_vertices: BTreeMap::new(),
        node_vertices: BTreeMap::new(),
        triangles: Vec::new(),
        materials: Vec::new(),
        stats: MeshingStats {
            cells,
            active_cells: 0,
            triangles: 0,
            degenerate_triangles: 0,
            slab_cells: SLAB_CELLS,
        },
    };
    builder.build()?;
    builder.finish()
}

impl MeshBuilder<'_> {
    fn build(&mut self) -> Result<(), MeshingError> {
        let cells = self.sampled.grid().cells;
        let mut slab = 0;
        while slab < cells[0] {
            let end = slab.saturating_add(SLAB_CELLS).min(cells[0]);
            for z in 0..cells[2] {
                for y in 0..cells[1] {
                    for x in slab..end {
                        if self.cancel.is_some_and(|poll| poll()) {
                            return Err(MeshingError::Cancelled);
                        }
                        self.process_cell([x, y, z])?;
                    }
                }
            }
            slab = end;
        }
        Ok(())
    }

    fn process_cell(&mut self, cell: [u32; 3]) -> Result<(), MeshingError> {
        let mut corner_nodes = [[0_u32; 3]; 8];
        let mut values = [0.0; 8];
        for ((node, value), offset) in corner_nodes.iter_mut().zip(&mut values).zip(CORNERS) {
            *node = [
                cell[0] + offset[0],
                cell[1] + offset[1],
                cell[2] + offset[2],
            ];
            *value = self
                .sampled
                .value(*node)
                .ok_or(MeshingError::MissingValue)?;
            if !value.is_finite() {
                return Err(MeshingError::MissingValue);
            }
        }
        // Exact iso nodes join the interior for crossing topology. Their edge
        // intersections still land on the node; this avoids chamfering sharp
        // grid-aligned edges merely because the authored zero is not negative.
        let inside = values.map(|value| value <= 0.0);
        if inside.iter().all(|value| *value) || inside.iter().all(|value| !*value) {
            return Ok(());
        }
        self.stats.active_cells = self
            .stats
            .active_cells
            .checked_add(1)
            .ok_or(MeshingError::CellBudget)?;
        let mut crossed = Vec::new();
        for edge in 0..EDGES.len() {
            if edge_crosses(&inside, edge)? {
                crossed.push(edge);
            }
        }
        let mut adjacency = BTreeMap::<usize, Vec<usize>>::new();
        for face in FACES {
            self.connect_face(face, corner_nodes, &inside, &mut adjacency)?;
        }
        for neighbors in adjacency.values_mut() {
            neighbors.sort_unstable();
            if neighbors.len() != 2 {
                return Err(MeshingError::InvalidTopology);
            }
        }
        self.emit_loops(cell, &crossed, &adjacency)
    }

    fn connect_face(
        &self,
        face: Face,
        corner_nodes: [[u32; 3]; 8],
        inside: &[bool],
        adjacency: &mut BTreeMap<usize, Vec<usize>>,
    ) -> Result<(), MeshingError> {
        let mut crossed = Vec::new();
        for edge in face.edges {
            if edge_crosses(inside, edge)? {
                crossed.push(edge);
            }
        }
        match crossed.as_slice() {
            [] => {}
            [a, b] => connect(*a, *b, adjacency),
            [a, b, c, d] => {
                let center = face_center(self.sampled, corner_nodes, face.corners)?;
                if self.field.evaluate(center)?.distance < 0.0 {
                    connect(*a, *b, adjacency);
                    connect(*c, *d, adjacency);
                } else {
                    connect(*a, *d, adjacency);
                    connect(*b, *c, adjacency);
                }
            }
            _ => return Err(MeshingError::InvalidTopology),
        }
        Ok(())
    }

    fn emit_loops(
        &mut self,
        cell: [u32; 3],
        crossed: &[usize],
        adjacency: &BTreeMap<usize, Vec<usize>>,
    ) -> Result<(), MeshingError> {
        let mut visited = BTreeSet::new();
        for &start in crossed {
            if visited.contains(&start) {
                continue;
            }
            let mut loop_edges = Vec::new();
            let mut current = start;
            let mut previous = None;
            loop {
                if !visited.insert(current) {
                    return Err(MeshingError::InvalidTopology);
                }
                loop_edges.push(current);
                let neighbors = adjacency
                    .get(&current)
                    .ok_or(MeshingError::InvalidTopology)?;
                let next = match neighbors.as_slice() {
                    [a, b] if previous == Some(*a) => *b,
                    [a, _] => *a,
                    _ => return Err(MeshingError::InvalidTopology),
                };
                if next == start {
                    break;
                }
                if visited.contains(&next) || loop_edges.len() > crossed.len() {
                    return Err(MeshingError::InvalidTopology);
                }
                previous = Some(current);
                current = next;
            }
            // Multiple crossed cube edges can meet at one exact grid node.
            // Collapse consecutive copies before fanning; emitting and then
            // deleting degenerate fan triangles would leave nonmanifold holes.
            let mut canonical_edges = Vec::new();
            let mut canonical_vertices = Vec::new();
            for edge in loop_edges {
                let position = self.edge_position(cell, edge)?;
                let vertex = self.edge_vertex(position.key)?;
                if canonical_vertices.last() == Some(&vertex) {
                    continue;
                }
                canonical_edges.push(edge);
                canonical_vertices.push(vertex);
            }
            if canonical_vertices.len() > 1
                && canonical_vertices.first() == canonical_vertices.last()
            {
                canonical_edges.pop();
                canonical_vertices.pop();
            }
            let Some(&anchor) = canonical_edges.first() else {
                continue;
            };
            let pairs = canonical_edges
                .get(1..)
                .ok_or(MeshingError::InvalidTopology)?;
            for pair in pairs.windows(2) {
                let [b, c] = pair else {
                    return Err(MeshingError::InvalidTopology);
                };
                self.emit_triangle(cell, [anchor, *b, *c])?;
            }
        }
        Ok(())
    }

    fn emit_triangle(&mut self, cell: [u32; 3], edges: [usize; 3]) -> Result<(), MeshingError> {
        let mut points = edges
            .map(|edge| self.edge_position(cell, edge))
            .try_transpose()?;
        let normal = cross(
            sub(points[1].position, points[0].position),
            sub(points[2].position, points[0].position),
        );
        if !finite(normal) {
            return Err(MeshingError::NonFinite);
        }
        if length(normal) == 0.0 || points[0].key == points[1].key || points[1].key == points[2].key
        {
            self.stats.degenerate_triangles = self
                .stats
                .degenerate_triangles
                .checked_add(1)
                .ok_or(MeshingError::CellBudget)?;
            return Ok(());
        }
        let centroid = centroid(points.map(|point| point.position));
        if !finite(centroid) {
            return Err(MeshingError::NonFinite);
        }
        let sample = self.field.evaluate(centroid)?;
        let gradient = self.field.gradient(centroid, self.normal_step)?;
        let alignment = dot(normal, gradient);
        if alignment < 0.0 {
            points.swap(1, 2);
        } else if alignment == 0.0 {
            let direction = scale(normal, 1.0 / length(normal));
            let plus = self
                .field
                .evaluate(add(centroid, scale(direction, self.normal_step)))?
                .distance;
            let minus = self
                .field
                .evaluate(sub(centroid, scale(direction, self.normal_step)))?
                .distance;
            if plus < minus {
                points.swap(1, 2);
            }
            // At a non-differentiable iso centroid, equal directional samples
            // have no orientation vote. Keep the canonical crossing-loop
            // winding rather than introducing a tolerance or an arbitrary flip.
        }
        let mut indices = [0_u32; 3];
        for (target, point) in indices.iter_mut().zip(points) {
            *target = self.edge_vertex(point.key)?;
        }
        if indices[0] == indices[1] || indices[1] == indices[2] || indices[0] == indices[2] {
            self.stats.degenerate_triangles = self
                .stats
                .degenerate_triangles
                .checked_add(1)
                .ok_or(MeshingError::CellBudget)?;
            return Ok(());
        }
        if self.stats.triangles >= self.limits.max_triangles {
            return Err(MeshingError::TriangleBudget);
        }
        self.triangles.extend(indices);
        self.materials.push(sample.material_id);
        self.stats.triangles = self
            .stats
            .triangles
            .checked_add(1)
            .ok_or(MeshingError::TriangleBudget)?;
        Ok(())
    }

    fn edge_position(&self, cell: [u32; 3], edge: usize) -> Result<EdgePosition, MeshingError> {
        let endpoints = EDGES.get(edge).ok_or(MeshingError::InvalidTopology)?;
        let [a, b] = *endpoints;
        let offset_a = CORNERS.get(a).ok_or(MeshingError::InvalidTopology)?;
        let offset_b = CORNERS.get(b).ok_or(MeshingError::InvalidTopology)?;
        let node_a = [
            cell[0] + offset_a[0],
            cell[1] + offset_a[1],
            cell[2] + offset_a[2],
        ];
        let node_b = [
            cell[0] + offset_b[0],
            cell[1] + offset_b[1],
            cell[2] + offset_b[2],
        ];
        let key = if node_a <= node_b {
            GridEdge {
                a: node_a,
                b: node_b,
            }
        } else {
            GridEdge {
                a: node_b,
                b: node_a,
            }
        };
        let value_a = self
            .sampled
            .value(key.a)
            .ok_or(MeshingError::MissingValue)?;
        let value_b = self
            .sampled
            .value(key.b)
            .ok_or(MeshingError::MissingValue)?;
        let amount = (-value_a) / (value_b - value_a);
        if !amount.is_finite() {
            return Err(MeshingError::MissingValue);
        }
        let amount = amount.clamp(0.0, 1.0);
        let grid = self.sampled.grid();
        let a = grid.point(key.a).ok_or(MeshingError::MissingValue)?;
        let b = grid.point(key.b).ok_or(MeshingError::MissingValue)?;
        let position = if amount <= 0.0 {
            a
        } else if amount >= 1.0 {
            b
        } else {
            add(a, scale(sub(b, a), amount))
        };
        if finite(position) {
            Ok(EdgePosition { key, position })
        } else {
            Err(MeshingError::NonFinite)
        }
    }

    fn edge_vertex(&mut self, key: GridEdge) -> Result<u32, MeshingError> {
        if let Some(index) = self.edge_vertices.get(&key) {
            return Ok(*index);
        }
        let grid = self.sampled.grid();
        let value_a = self
            .sampled
            .value(key.a)
            .ok_or(MeshingError::MissingValue)?;
        let value_b = self
            .sampled
            .value(key.b)
            .ok_or(MeshingError::MissingValue)?;
        let amount = ((-value_a) / (value_b - value_a)).clamp(0.0, 1.0);
        let index = if amount <= 0.0 {
            self.node_vertex(key.a)?
        } else if amount >= 1.0 {
            self.node_vertex(key.b)?
        } else {
            let a = grid.point(key.a).ok_or(MeshingError::MissingValue)?;
            let b = grid.point(key.b).ok_or(MeshingError::MissingValue)?;
            self.push_vertex(add(a, scale(sub(b, a), amount)))?
        };
        self.edge_vertices.insert(key, index);
        Ok(index)
    }

    fn node_vertex(&mut self, node: [u32; 3]) -> Result<u32, MeshingError> {
        if let Some(index) = self.node_vertices.get(&node) {
            return Ok(*index);
        }
        let point = self
            .sampled
            .grid()
            .point(node)
            .ok_or(MeshingError::MissingValue)?;
        let index = self.push_vertex(point)?;
        self.node_vertices.insert(node, index);
        Ok(index)
    }

    fn push_vertex(&mut self, position: Vec3) -> Result<u32, MeshingError> {
        if !finite(position) {
            return Err(MeshingError::NonFinite);
        }
        let index = u32::try_from(self.vertices.len()).map_err(|_| MeshingError::VertexBudget)?;
        self.vertices.push(position);
        Ok(index)
    }

    fn component_alignment(&self, component: &[usize]) -> Result<f64, MeshingError> {
        let mut alignment = 0.0;
        for (index, &triangle) in component.iter().enumerate() {
            if index % CANCEL_POLL_INTERVAL == 0 && self.cancel.is_some_and(|poll| poll()) {
                return Err(MeshingError::Cancelled);
            }
            let start = triangle
                .checked_mul(3)
                .ok_or(MeshingError::InvalidTopology)?;
            let corners = self
                .triangles
                .get(start..start + 3)
                .ok_or(MeshingError::InvalidTopology)?;
            let [a, b, c] = corners else {
                return Err(MeshingError::InvalidTopology);
            };
            let point = |index| -> Result<Vec3, MeshingError> {
                self.vertices
                    .get(usize::try_from(index).map_err(|_| MeshingError::VertexBudget)?)
                    .copied()
                    .ok_or(MeshingError::InvalidTopology)
            };
            let a = point(*a)?;
            let b = point(*b)?;
            let c = point(*c)?;
            let normal = cross(sub(b, a), sub(c, a));
            let center = centroid([a, b, c]);
            alignment += dot(normal, self.field.gradient(center, self.normal_step)?);
        }
        if alignment.is_finite() {
            Ok(alignment)
        } else {
            Err(MeshingError::NonFinite)
        }
    }

    fn orient(&mut self) -> Result<(), MeshingError> {
        let triangle_count = self.triangles.len() / 3;
        let mut incident = BTreeMap::<(u32, u32), Vec<usize>>::new();
        for triangle in 0..triangle_count {
            if triangle % CANCEL_POLL_INTERVAL == 0 && self.cancel.is_some_and(|poll| poll()) {
                return Err(MeshingError::Cancelled);
            }
            for edge in triangle_edges(&self.triangles, triangle)? {
                incident.entry(edge).or_default().push(triangle);
            }
        }
        for (index, triangles) in incident.values().enumerate() {
            if index % CANCEL_POLL_INTERVAL == 0 && self.cancel.is_some_and(|poll| poll()) {
                return Err(MeshingError::Cancelled);
            }
            if triangles.len() != 2 {
                return Err(MeshingError::InvalidTopology);
            }
        }

        let mut visited = vec![false; triangle_count];
        for seed in 0..triangle_count {
            if visited.get(seed).copied().unwrap_or(false) {
                continue;
            }
            if self.cancel.is_some_and(|poll| poll()) {
                return Err(MeshingError::Cancelled);
            }
            *visited.get_mut(seed).ok_or(MeshingError::InvalidTopology)? = true;
            let mut pending = vec![seed];
            let mut component = Vec::new();
            while let Some(current) = pending.pop() {
                if component.len() % CANCEL_POLL_INTERVAL == 0
                    && self.cancel.is_some_and(|poll| poll())
                {
                    return Err(MeshingError::Cancelled);
                }
                component.push(current);
                for edge in triangle_edges(&self.triangles, current)? {
                    let pair = incident.get(&edge).ok_or(MeshingError::InvalidTopology)?;
                    let first = *pair.first().ok_or(MeshingError::InvalidTopology)?;
                    let second = *pair.get(1).ok_or(MeshingError::InvalidTopology)?;
                    let neighbor = if first == current { second } else { first };
                    if visited.get(neighbor).copied().unwrap_or(false) {
                        continue;
                    }
                    if triangle_edge_direction(&self.triangles, current, edge)?
                        == triangle_edge_direction(&self.triangles, neighbor, edge)?
                    {
                        reverse_triangle(&mut self.triangles, neighbor)?;
                    }
                    *visited
                        .get_mut(neighbor)
                        .ok_or(MeshingError::InvalidTopology)? = true;
                    pending.push(neighbor);
                }
            }
            // Positive signed volume is not a valid component orientation test:
            // an enclosed cavity must face the void and has negative volume.
            // Field alignment points out of the solid for both shells and voids.
            let alignment = self.component_alignment(&component)?;
            if alignment < 0.0 {
                for (index, triangle) in component.into_iter().enumerate() {
                    if index % CANCEL_POLL_INTERVAL == 0 && self.cancel.is_some_and(|poll| poll()) {
                        return Err(MeshingError::Cancelled);
                    }
                    reverse_triangle(&mut self.triangles, triangle)?;
                }
            } else if alignment == 0.0 {
                return Err(MeshingError::NormalUnavailable);
            }
        }
        Ok(())
    }

    fn finish(mut self) -> Result<MeshResult, MeshingError> {
        if self.triangles.is_empty() {
            return Err(MeshingError::EmptySurface);
        }
        self.orient()?;
        let mut positions = Vec::with_capacity(self.vertices.len().saturating_mul(3));
        let mut normals = Vec::with_capacity(self.vertices.len().saturating_mul(3));
        for &vertex in &self.vertices {
            if self.cancel.is_some_and(|poll| poll()) {
                return Err(MeshingError::Cancelled);
            }
            let gradient = self.field.gradient(vertex, self.normal_step)?;
            let magnitude = length(gradient);
            if !magnitude.is_finite() || magnitude <= 0.0 {
                return Err(MeshingError::NormalUnavailable);
            }
            positions.extend([vertex.x, vertex.y, vertex.z]);
            push_normal(&mut normals, scale(gradient, 1.0 / magnitude));
        }
        let result = MeshResult {
            content_hash: hash_mesh(&positions, &normals, &self.triangles, &self.materials)?,
            stats: self.stats,
            sampling: self.sampled.stats,
            positions,
            normals,
            triangles: self.triangles,
            tri_material: self.materials,
        };
        Ok(result)
    }
}

trait TryTranspose<T, E> {
    fn try_transpose(self) -> Result<[T; 3], E>;
}

impl<T, E> TryTranspose<T, E> for [Result<T, E>; 3] {
    fn try_transpose(self) -> Result<[T; 3], E> {
        let [a, b, c] = self;
        Ok([a?, b?, c?])
    }
}

fn edge_key(a: u32, b: u32) -> (u32, u32) {
    if a < b { (a, b) } else { (b, a) }
}

fn triangle_edges(triangles: &[u32], triangle: usize) -> Result<[(u32, u32); 3], MeshingError> {
    let start = triangle
        .checked_mul(3)
        .ok_or(MeshingError::InvalidTopology)?;
    let corners = triangles
        .get(start..start + 3)
        .ok_or(MeshingError::InvalidTopology)?;
    let [a, b, c] = corners else {
        return Err(MeshingError::InvalidTopology);
    };
    Ok([edge_key(*a, *b), edge_key(*b, *c), edge_key(*c, *a)])
}

fn triangle_edge_direction(
    triangles: &[u32],
    triangle: usize,
    edge: (u32, u32),
) -> Result<i32, MeshingError> {
    let start = triangle
        .checked_mul(3)
        .ok_or(MeshingError::InvalidTopology)?;
    let corners = triangles
        .get(start..start + 3)
        .ok_or(MeshingError::InvalidTopology)?;
    let [a, b, c] = corners else {
        return Err(MeshingError::InvalidTopology);
    };
    for (from, to) in [(*a, *b), (*b, *c), (*c, *a)] {
        if edge_key(from, to) == edge {
            return Ok(if from < to { 1 } else { -1 });
        }
    }
    Err(MeshingError::InvalidTopology)
}

fn reverse_triangle(triangles: &mut [u32], triangle: usize) -> Result<(), MeshingError> {
    let start = triangle
        .checked_mul(3)
        .ok_or(MeshingError::InvalidTopology)?;
    let corners = triangles
        .get_mut(start..start + 3)
        .ok_or(MeshingError::InvalidTopology)?;
    corners.swap(1, 2);
    Ok(())
}

fn edge_crosses(inside: &[bool], edge: usize) -> Result<bool, MeshingError> {
    let [a, b] = *EDGES.get(edge).ok_or(MeshingError::InvalidTopology)?;
    let a = *inside.get(a).ok_or(MeshingError::InvalidTopology)?;
    let b = *inside.get(b).ok_or(MeshingError::InvalidTopology)?;
    Ok(a != b)
}

fn connect(edge_a: usize, edge_b: usize, adjacency: &mut BTreeMap<usize, Vec<usize>>) {
    adjacency.entry(edge_a).or_default().push(edge_b);
    adjacency.entry(edge_b).or_default().push(edge_a);
}

fn face_center(
    sampled: &SampledGrid,
    corner_nodes: [[u32; 3]; 8],
    corners: [usize; 4],
) -> Result<Vec3, MeshingError> {
    let grid = sampled.grid();
    let mut points = [Vec3 {
        x: 0.0,
        y: 0.0,
        z: 0.0,
    }; 4];
    for (target, corner) in targets_mut(&mut points).zip(corners) {
        let node = *corner_nodes
            .get(corner)
            .ok_or(MeshingError::InvalidTopology)?;
        *target = grid.point(node).ok_or(MeshingError::MissingValue)?;
    }
    let [a, b, c, d] = points;
    Ok(midpoint(midpoint(a, b), midpoint(c, d)))
}

fn targets_mut(values: &mut [Vec3; 4]) -> impl Iterator<Item = &mut Vec3> {
    values.iter_mut()
}

fn centroid(points: [Vec3; 3]) -> Vec3 {
    let [a, b, c] = points;
    add(a, scale(add(sub(b, a), sub(c, a)), 1.0 / 3.0))
}

fn midpoint(a: Vec3, b: Vec3) -> Vec3 {
    Vec3 {
        x: f64::midpoint(a.x, b.x),
        y: f64::midpoint(a.y, b.y),
        z: f64::midpoint(a.z, b.z),
    }
}

fn add(a: Vec3, b: Vec3) -> Vec3 {
    Vec3 {
        x: a.x + b.x,
        y: a.y + b.y,
        z: a.z + b.z,
    }
}

fn sub(a: Vec3, b: Vec3) -> Vec3 {
    Vec3 {
        x: a.x - b.x,
        y: a.y - b.y,
        z: a.z - b.z,
    }
}

fn scale(value: Vec3, factor: f64) -> Vec3 {
    Vec3 {
        x: value.x * factor,
        y: value.y * factor,
        z: value.z * factor,
    }
}

fn cross(a: Vec3, b: Vec3) -> Vec3 {
    Vec3 {
        x: a.y * b.z - a.z * b.y,
        y: a.z * b.x - a.x * b.z,
        z: a.x * b.y - a.y * b.x,
    }
}

fn dot(a: Vec3, b: Vec3) -> f64 {
    a.x * b.x + a.y * b.y + a.z * b.z
}

fn length(value: Vec3) -> f64 {
    dot(value, value).sqrt()
}

fn finite(value: Vec3) -> bool {
    [value.x, value.y, value.z].into_iter().all(f64::is_finite)
}

#[expect(
    clippy::cast_possible_truncation,
    reason = "normalized f64 field gradients are intentionally narrowed to render-normal f32"
)]
fn push_normal(output: &mut Vec<f32>, normal: Vec3) {
    output.extend([normal.x as f32, normal.y as f32, normal.z as f32]);
}

fn hash_mesh(
    positions: &[f64],
    normals: &[f32],
    triangles: &[u32],
    materials: &[i32],
) -> Result<u64, MeshingError> {
    let vertex_count = positions.len() / 3;
    let triangle_count = triangles.len() / 3;
    let mut hash = 1_469_598_103_934_665_603_u64;
    hash_count(
        &mut hash,
        u64::try_from(vertex_count).map_err(|_| MeshingError::VertexBudget)?,
    );
    for value in positions {
        hash_bytes(&mut hash, &value.to_bits().to_le_bytes());
    }
    hash_count(
        &mut hash,
        u64::try_from(normals.len()).map_err(|_| MeshingError::VertexBudget)?,
    );
    for value in normals {
        hash_bytes(&mut hash, &value.to_bits().to_le_bytes());
    }
    hash_count(
        &mut hash,
        u64::try_from(triangle_count).map_err(|_| MeshingError::TriangleBudget)?,
    );
    for value in triangles {
        hash_bytes(&mut hash, &value.to_le_bytes());
    }
    for value in materials {
        hash_bytes(&mut hash, &value.to_le_bytes());
    }
    for index in 0..vertex_count {
        let index = u32::try_from(index).map_err(|_| MeshingError::VertexBudget)?;
        hash_bytes(&mut hash, &index.to_le_bytes());
    }
    for index in 0..triangle_count {
        let index = u32::try_from(index).map_err(|_| MeshingError::TriangleBudget)?;
        hash_bytes(&mut hash, &index.to_le_bytes());
    }
    Ok(hash)
}

fn hash_count(hash: &mut u64, count: u64) {
    hash_bytes(hash, &count.to_le_bytes());
}

fn hash_bytes(hash: &mut u64, bytes: &[u8]) {
    for byte in bytes {
        *hash ^= u64::from(*byte);
        *hash = hash.wrapping_mul(1_099_511_628_211);
    }
}

#[cfg(test)]
mod tests;
