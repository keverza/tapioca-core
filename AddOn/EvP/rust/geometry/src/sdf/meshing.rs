//! Deterministic shared-edge Marching Cubes over dense or sparse samples.
//!
//! This clean-room implementation constructs intersection loops from cube-face
//! crossings instead of importing a triangle lookup table. Checkerboard faces
//! use the exact field at the face center, identically on both adjacent cells.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt;

use super::{
    GridSpec, PreparedField, SampledGrid, SamplingError, SamplingLimits, SamplingStats,
    SamplingStrategy, field, sample_with_cancellation,
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
    /// Vertex indices exceed the `u32` output contract.
    VertexBudget,
    /// The triangle ceiling was reached; no partial mesh is returned.
    TriangleBudget,
    /// No nondegenerate isosurface was found.
    EmptySurface,
    /// A required sampled corner is absent or internally inconsistent.
    MissingValue,
    /// Face crossings did not form closed degree-two loops.
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
            Self::VertexBudget => "VertexBudget: mesh vertices exceed the u32 index contract",
            Self::TriangleBudget => "TriangleBudget: mesh triangles exceed their ceiling",
            Self::EmptySurface => "EmptySurface: no nondegenerate zero isosurface was found",
            Self::MissingValue => "MissingValue: a required grid corner has no sampled value",
            Self::InvalidTopology => "InvalidTopology: cube crossings do not form closed loops",
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
    let sampled = sample_with_cancellation(field, grid, strategy, limits.sampling, cancel)?;
    marching_cubes_with_cancellation(field, &sampled, limits.meshing, cancel)
}

/// Mesh a complete sampled field. Cells are visited in fixed 64-cell X slabs,
/// then z/y/x order inside each slab. Shared grid edges and exact grid-node
/// crossings use canonical maps, so slab boundaries never duplicate vertices.
///
/// # Errors
/// Cancellation, missing samples, invalid loops, non-finite output and budget
/// excess discard the complete candidate. No partial mesh is returned.
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
        let inside = values.map(|value| value < 0.0);
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
            let Some(&anchor) = loop_edges.first() else {
                return Err(MeshingError::InvalidTopology);
            };
            let pairs = loop_edges.get(1..).ok_or(MeshingError::InvalidTopology)?;
            for pair in pairs.windows(2) {
                let [b, c] = pair else {
                    return Err(MeshingError::InvalidTopology);
                };
                self.emit_triangle(cell, [anchor, *b, *c])?;
            }
        }
        Ok(())
    }

    #[expect(
        clippy::float_cmp,
        reason = "exact zero detects degenerate area and undefined directional orientation"
    )]
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
            } else if plus == minus {
                return Err(MeshingError::NormalUnavailable);
            }
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

    fn finish(self) -> Result<MeshResult, MeshingError> {
        if self.triangles.is_empty() {
            return Err(MeshingError::EmptySurface);
        }
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
