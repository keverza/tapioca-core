//! Node-based dense and sparse sampling over one prepared SDF field.

use std::collections::BTreeMap;
use std::fmt;
use std::sync::{
    Arc,
    atomic::{AtomicBool, Ordering},
};

use super::{Bounds, PreparedField, field};

/// A node grid over closed world-metre bounds. Spacing can differ per axis
/// because bounds remain exact when an extent is not a multiple of voxel size.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct GridSpec {
    /// Exact sampled bounds.
    pub bounds: Bounds,
    /// Positive cell counts along x, y and z; node counts are one greater.
    pub cells: [u32; 3],
    /// Actual world-metre spacing along x, y and z.
    pub spacing: [f64; 3],
}

impl GridSpec {
    /// Derive a grid from ordered nonempty bounds and a positive voxel size.
    /// Each cell count is `ceil(extent / voxel_size)`.
    ///
    /// # Errors
    /// Refuses invalid bounds/voxels, a zero extent, or counts outside `u32`.
    pub fn new(bounds: Bounds, voxel_size: f64) -> Result<Self, SamplingError> {
        if !bounds.is_valid() {
            return Err(SamplingError::InvalidGrid);
        }
        if !voxel_size.is_finite() || voxel_size <= 0.0 {
            return Err(SamplingError::InvalidVoxelSize);
        }
        let extents = [
            bounds.max.x - bounds.min.x,
            bounds.max.y - bounds.min.y,
            bounds.max.z - bounds.min.z,
        ];
        if extents
            .iter()
            .any(|extent| !extent.is_finite() || *extent <= 0.0)
        {
            return Err(SamplingError::InvalidGrid);
        }
        let mut cells = [0_u32; 3];
        let mut spacing = [0.0; 3];
        for ((cell, spacing), extent) in cells.iter_mut().zip(&mut spacing).zip(extents) {
            *cell = cell_count(extent, voxel_size)?;
            *spacing = extent / f64::from(*cell);
            if !spacing.is_finite() || *spacing <= 0.0 {
                return Err(SamplingError::InvalidGrid);
            }
        }
        Ok(Self {
            bounds,
            cells,
            spacing,
        })
    }

    /// Node counts along x, y and z.
    #[must_use]
    pub fn nodes(self) -> [u32; 3] {
        let [x, y, z] = self.cells;
        [x + 1, y + 1, z + 1]
    }

    /// Checked total node count.
    ///
    /// # Errors
    /// Returns [`SamplingError::GridBudget`] on arithmetic overflow.
    pub fn total_points(self) -> Result<u64, SamplingError> {
        self.nodes()
            .into_iter()
            .map(u64::from)
            .try_fold(1_u64, |total, count| {
                total.checked_mul(count).ok_or(SamplingError::GridBudget)
            })
    }

    /// World-metre position of a node, or `None` outside the grid.
    #[must_use]
    pub fn point(self, index: [u32; 3]) -> Option<crate::affine::Vec3> {
        let [x, y, z] = index;
        let [nx, ny, nz] = self.nodes();
        if x >= nx || y >= ny || z >= nz {
            return None;
        }
        let point = crate::affine::Vec3 {
            x: self.bounds.min.x + f64::from(x) * self.spacing[0],
            y: self.bounds.min.y + f64::from(y) * self.spacing[1],
            z: self.bounds.min.z + f64::from(z) * self.spacing[2],
        };
        [point.x, point.y, point.z]
            .into_iter()
            .all(f64::is_finite)
            .then_some(point)
    }

    fn flat_index(self, index: [u32; 3]) -> Option<usize> {
        let [x, y, z] = index;
        let [nx, ny, nz] = self.nodes();
        if x >= nx || y >= ny || z >= nz {
            return None;
        }
        let flat = (u64::from(z) * u64::from(ny) + u64::from(y))
            .checked_mul(u64::from(nx))?
            .checked_add(u64::from(x))?;
        usize::try_from(flat).ok()
    }
}

/// Dense or surface-local sparse sampling schedule.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SamplingStrategy {
    /// Evaluate every grid point; retained as the correctness oracle.
    Dense,
    /// Evaluate only 8-sample blocks whose conservative region may reach zero.
    Sparse,
}

/// Per-job sampling ceilings. Values above the SDF-0 hard ceilings are capped.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SamplingLimits {
    /// Maximum dense grid points.
    pub max_dense_points: u64,
    /// Maximum actually stored sparse points, excluding sign-only blocks.
    pub max_sparse_stored_points: u64,
}

impl Default for SamplingLimits {
    fn default() -> Self {
        Self {
            max_dense_points: 64_000_000,
            max_sparse_stored_points: 8_000_000,
        }
    }
}

/// Cloneable Rust-owned cooperative cancellation handle. No foreign atomic or
/// detached worker is involved; callers may cancel from their job controller.
#[derive(Clone, Debug, Default)]
pub struct CancellationToken(Arc<AtomicBool>);

impl CancellationToken {
    /// Create an uncancelled token.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Request cancellation. Existing partial samples remain internal and are
    /// discarded by the sampling operation.
    pub fn cancel(&self) {
        self.0.store(true, Ordering::Release);
    }

    /// Whether cancellation has been requested.
    #[must_use]
    pub fn is_cancelled(&self) -> bool {
        self.0.load(Ordering::Acquire)
    }
}

/// Sampling counters and capability diagnostics for UI/reporting.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SamplingStats {
    /// Total points in the node grid.
    pub total_points: u64,
    /// Points for which exact field evaluation ran.
    pub evaluated_points: u64,
    /// Actual sparse values allocated; zero for sentinel-only regions and dense.
    pub stored_sparse_points: u64,
    /// Block-tree regions visited, including the root and leaves.
    pub regions: u64,
    /// Regions pruned without exact point evaluation.
    pub skipped_regions: u64,
    /// Selected schedule.
    pub strategy: SamplingStrategy,
    /// Whether finite prepared field bounds were available.
    pub bounded: bool,
    /// Conservative field Lipschitz value; infinity means unknown.
    pub lipschitz: f64,
    /// Requested voxel size is represented by the grid's actual spacing.
    pub actual_spacing: [f64; 3],
}

/// Dense values in x-fastest, then y, then z node order.
#[derive(Clone, Debug, PartialEq)]
struct DenseGrid {
    grid: GridSpec,
    values: Vec<f64>,
}

#[derive(Clone, Debug, PartialEq)]
enum SparseBlock {
    Above,
    Below,
    Values {
        origin: [u32; 3],
        dimensions: [u32; 3],
        values: Vec<f64>,
    },
}

/// Sparse blocks keyed by their 8-sample block coordinates. Skipped blocks keep
/// only an unambiguous sign; active leaf blocks own exact evaluated values.
#[derive(Clone, Debug, PartialEq)]
struct SparseGrid {
    blocks: BTreeMap<[u32; 3], SparseBlock>,
}

/// Complete sampled grid. Failure returns no partial value of this type.
#[derive(Clone, Debug, PartialEq)]
pub struct SampledGrid {
    grid: GridSpec,
    /// Sampling diagnostics.
    pub stats: SamplingStats,
    storage: Storage,
}

#[derive(Clone, Debug, PartialEq)]
enum Storage {
    Dense(DenseGrid),
    Sparse(SparseGrid),
}

impl SampledGrid {
    /// Grid specification shared by every storage strategy.
    #[must_use]
    pub const fn grid(&self) -> GridSpec {
        self.grid
    }

    /// Distance at one node. Skipped sparse nodes return `-1` or `+1`, which is
    /// deliberately only a sign sentinel, not an exact distance. Out-of-grid or
    /// internally incomplete nodes return `None` and must be refused by meshing.
    #[must_use]
    pub fn value(&self, index: [u32; 3]) -> Option<f64> {
        match &self.storage {
            Storage::Dense(dense) => dense
                .grid
                .flat_index(index)
                .and_then(|flat| dense.values.get(flat).copied()),
            Storage::Sparse(sparse) => sparse.value(index),
        }
    }

    /// Whether this node belongs to an exactly evaluated sparse block. Dense
    /// grids return true for every in-grid node.
    #[must_use]
    pub fn is_evaluated(&self, index: [u32; 3]) -> bool {
        match &self.storage {
            Storage::Dense(dense) => dense.grid.flat_index(index).is_some(),
            Storage::Sparse(sparse) => sparse.is_evaluated(index),
        }
    }
}

impl SparseGrid {
    fn value(&self, index: [u32; 3]) -> Option<f64> {
        let [x, y, z] = index;
        let key = [x / 8, y / 8, z / 8];
        match self.blocks.get(&key)? {
            SparseBlock::Above => Some(1.0),
            SparseBlock::Below => Some(-1.0),
            SparseBlock::Values {
                origin,
                dimensions,
                values,
            } => {
                let local = [x - origin[0], y - origin[1], z - origin[2]];
                if local[0] >= dimensions[0]
                    || local[1] >= dimensions[1]
                    || local[2] >= dimensions[2]
                {
                    return None;
                }
                let flat = (local[2] * dimensions[1] + local[1]) * dimensions[0] + local[0];
                values.get(usize::try_from(flat).ok()?).copied()
            }
        }
    }

    fn is_evaluated(&self, index: [u32; 3]) -> bool {
        let key = [index[0] / 8, index[1] / 8, index[2] / 8];
        matches!(self.blocks.get(&key), Some(SparseBlock::Values { .. }))
    }
}

/// Grid, field or cancellation refusal. No partial sampled grid is returned.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum SamplingError {
    /// Bounds or derived grid dimensions are invalid/unrepresentable.
    InvalidGrid,
    /// Voxel size is not finite and positive.
    InvalidVoxelSize,
    /// Dense grid or arithmetic ceiling is exceeded.
    GridBudget,
    /// Sparse stored-point ceiling is exceeded.
    SparseBudget,
    /// Cooperative cancellation was observed.
    Cancelled,
    /// Exact field evaluation failed.
    Field(field::Error),
    /// A sparse block index or value count is inconsistent.
    Incomplete,
}

impl SamplingError {
    /// Stable allocation-free diagnostic prefix and description.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::InvalidGrid => "InvalidGrid: bounds must be finite, ordered and nonempty",
            Self::InvalidVoxelSize => "InvalidVoxelSize: voxel size must be finite and positive",
            Self::GridBudget => "GridBudget: the requested dense grid exceeds its ceiling",
            Self::SparseBudget => "SparseBudget: the active sparse blocks exceed their ceiling",
            Self::Cancelled => "Cancelled: sampling was cancelled; no partial grid is returned",
            Self::Field(error) => error.message(),
            Self::Incomplete => "Incomplete: sparse block storage is inconsistent",
        }
    }
}

impl fmt::Display for SamplingError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.message())
    }
}

impl From<field::Error> for SamplingError {
    fn from(error: field::Error) -> Self {
        Self::Field(error)
    }
}

/// Sample one prepared field with checked budgets and cooperative cancellation.
/// Sparse scheduling subdivides 8-sample blocks along their longest axis and
/// prunes only when a conservative interval is wholly above or below zero.
///
/// # Errors
/// Invalid grids, budget excess, field errors and cancellation return no partial
/// grid. Sparse storage is deterministic regardless of evaluation scheduling.
pub fn sample(
    field: &PreparedField,
    grid: GridSpec,
    strategy: SamplingStrategy,
    limits: SamplingLimits,
    cancel: &CancellationToken,
) -> Result<SampledGrid, SamplingError> {
    let poll = || cancel.is_cancelled();
    sample_with_cancellation(field, grid, strategy, limits, Some(&poll))
}

/// Callback-driven cancellation variant used by the FFI boundary. The geometry
/// crate knows only a boolean poll, never a host API or foreign atomic.
///
/// # Errors
/// Same atomic grid/budget/field/cancellation contract as [`sample`].
pub fn sample_with_cancellation(
    field: &PreparedField,
    grid: GridSpec,
    strategy: SamplingStrategy,
    limits: SamplingLimits,
    cancel: Option<&dyn Fn() -> bool>,
) -> Result<SampledGrid, SamplingError> {
    let total = grid.total_points()?;
    let dense_limit = limits.max_dense_points.min(64_000_000);
    let sparse_limit = limits.max_sparse_stored_points.min(8_000_000);
    if strategy == SamplingStrategy::Dense && total > dense_limit {
        return Err(SamplingError::GridBudget);
    }
    if cancel.is_some_and(|poll| poll()) {
        return Err(SamplingError::Cancelled);
    }
    let mut stats = SamplingStats {
        total_points: total,
        evaluated_points: 0,
        stored_sparse_points: 0,
        regions: 0,
        skipped_regions: 0,
        strategy,
        bounded: field.bounds().is_some(),
        lipschitz: field.lipschitz(),
        actual_spacing: grid.spacing,
    };
    let storage = match strategy {
        SamplingStrategy::Dense => Storage::Dense(sample_dense(field, grid, cancel, &mut stats)?),
        SamplingStrategy::Sparse => Storage::Sparse(sample_sparse(
            field,
            grid,
            sparse_limit,
            cancel,
            &mut stats,
        )?),
    };
    Ok(SampledGrid {
        grid,
        stats,
        storage,
    })
}

fn sample_dense(
    field: &PreparedField,
    grid: GridSpec,
    cancel: Option<&dyn Fn() -> bool>,
    stats: &mut SamplingStats,
) -> Result<DenseGrid, SamplingError> {
    let total = grid.total_points()?;
    let capacity = usize::try_from(total).map_err(|_| SamplingError::GridBudget)?;
    let mut values = Vec::with_capacity(capacity);
    let [nx, ny, nz] = grid.nodes();
    for z in 0..nz {
        for y in 0..ny {
            for x in 0..nx {
                if values.len() % 512 == 0 && cancel.is_some_and(|poll| poll()) {
                    return Err(SamplingError::Cancelled);
                }
                let point = grid.point([x, y, z]).ok_or(SamplingError::InvalidGrid)?;
                values.push(field.evaluate(point)?.distance);
            }
        }
    }
    stats.evaluated_points = total;
    Ok(DenseGrid { grid, values })
}

#[derive(Clone, Copy)]
struct BlockRegion {
    min: [u32; 3],
    max: [u32; 3],
}

#[derive(Clone, Copy)]
struct SparseContext<'a> {
    field: &'a PreparedField,
    grid: GridSpec,
    stored_limit: u64,
    cancel: Option<&'a dyn Fn() -> bool>,
}

fn sample_sparse(
    field: &PreparedField,
    grid: GridSpec,
    stored_limit: u64,
    cancel: Option<&dyn Fn() -> bool>,
    stats: &mut SamplingStats,
) -> Result<SparseGrid, SamplingError> {
    let context = SparseContext {
        field,
        grid,
        stored_limit,
        cancel,
    };
    let mut sparse = SparseGrid {
        blocks: BTreeMap::new(),
    };
    let region = BlockRegion {
        min: [0, 0, 0],
        max: grid.nodes().map(|count| count.div_ceil(8)),
    };
    sample_region(context, region, stats, &mut sparse)?;
    Ok(sparse)
}

fn sample_region(
    context: SparseContext<'_>,
    region: BlockRegion,
    stats: &mut SamplingStats,
    sparse: &mut SparseGrid,
) -> Result<(), SamplingError> {
    if context.cancel.is_some_and(|poll| poll()) {
        return Err(SamplingError::Cancelled);
    }
    stats.regions = stats
        .regions
        .checked_add(1)
        .ok_or(SamplingError::GridBudget)?;
    let bounds = region_bounds(context.grid, region)?;
    let interval = context.field.interval(bounds)?;
    if !interval.may_contain_zero() {
        let block = if interval.min > 0.0 {
            SparseBlock::Above
        } else {
            SparseBlock::Below
        };
        for z in region.min[2]..region.max[2] {
            for y in region.min[1]..region.max[1] {
                for x in region.min[0]..region.max[0] {
                    sparse.blocks.insert([x, y, z], block.clone());
                }
            }
        }
        stats.skipped_regions = stats
            .skipped_regions
            .checked_add(1)
            .ok_or(SamplingError::GridBudget)?;
        return Ok(());
    }
    if region_size(region) == 1 {
        return evaluate_block(context, region.min, stats, sparse);
    }
    let (lower, upper) = split_region(region);
    sample_region(context, lower, stats, sparse)?;
    sample_region(context, upper, stats, sparse)
}

fn evaluate_block(
    context: SparseContext<'_>,
    block: [u32; 3],
    stats: &mut SamplingStats,
    sparse: &mut SparseGrid,
) -> Result<(), SamplingError> {
    let origin = block.map(|value| value * 8);
    let nodes = context.grid.nodes();
    let dimensions = [
        8_u32.min(nodes[0] - origin[0]),
        8_u32.min(nodes[1] - origin[1]),
        8_u32.min(nodes[2] - origin[2]),
    ];
    let count = dimensions
        .into_iter()
        .map(u64::from)
        .try_fold(1_u64, |total, value| {
            total.checked_mul(value).ok_or(SamplingError::GridBudget)
        })?;
    if stats.stored_sparse_points.saturating_add(count) > context.stored_limit {
        return Err(SamplingError::SparseBudget);
    }
    let capacity = usize::try_from(count).map_err(|_| SamplingError::SparseBudget)?;
    let mut values = Vec::with_capacity(capacity);
    for z in 0..dimensions[2] {
        for y in 0..dimensions[1] {
            for x in 0..dimensions[0] {
                if values.len() % 64 == 0 && context.cancel.is_some_and(|poll| poll()) {
                    return Err(SamplingError::Cancelled);
                }
                let index = [origin[0] + x, origin[1] + y, origin[2] + z];
                let point = context
                    .grid
                    .point(index)
                    .ok_or(SamplingError::InvalidGrid)?;
                values.push(context.field.evaluate(point)?.distance);
            }
        }
    }
    stats.evaluated_points = stats
        .evaluated_points
        .checked_add(count)
        .ok_or(SamplingError::GridBudget)?;
    stats.stored_sparse_points = stats
        .stored_sparse_points
        .checked_add(count)
        .ok_or(SamplingError::SparseBudget)?;
    sparse.blocks.insert(
        block,
        SparseBlock::Values {
            origin,
            dimensions,
            values,
        },
    );
    Ok(())
}

fn region_size(region: BlockRegion) -> u64 {
    region_lengths(region).into_iter().map(u64::from).product()
}

#[derive(Clone, Copy)]
enum Axis {
    X,
    Y,
    Z,
}

fn region_lengths(region: BlockRegion) -> [u32; 3] {
    let [min_x, min_y, min_z] = region.min;
    let [max_x, max_y, max_z] = region.max;
    [max_x - min_x, max_y - min_y, max_z - min_z]
}

fn split_region(region: BlockRegion) -> (BlockRegion, BlockRegion) {
    let [x, y, z] = region_lengths(region);
    let axis = if x >= y && x >= z {
        Axis::X
    } else if y >= z {
        Axis::Y
    } else {
        Axis::Z
    };
    let mut lower = region;
    let mut upper = region;
    match axis {
        Axis::X => {
            let midpoint = u32::midpoint(region.min[0], region.max[0]);
            lower.max[0] = midpoint;
            upper.min[0] = midpoint;
        }
        Axis::Y => {
            let midpoint = u32::midpoint(region.min[1], region.max[1]);
            lower.max[1] = midpoint;
            upper.min[1] = midpoint;
        }
        Axis::Z => {
            let midpoint = u32::midpoint(region.min[2], region.max[2]);
            lower.max[2] = midpoint;
            upper.min[2] = midpoint;
        }
    }
    (lower, upper)
}

fn region_bounds(grid: GridSpec, region: BlockRegion) -> Result<Bounds, SamplingError> {
    let nodes = grid.nodes();
    let min_index = region
        .min
        .map(|value| value.saturating_mul(8).saturating_sub(1));
    let max_index = [
        region.max[0].saturating_mul(8).min(nodes[0] - 1),
        region.max[1].saturating_mul(8).min(nodes[1] - 1),
        region.max[2].saturating_mul(8).min(nodes[2] - 1),
    ];
    let min = grid.point(min_index).ok_or(SamplingError::InvalidGrid)?;
    let max = grid.point(max_index).ok_or(SamplingError::InvalidGrid)?;
    let expanded = Bounds {
        min: crate::affine::Vec3 {
            x: min.x - grid.spacing[0],
            y: min.y - grid.spacing[1],
            z: min.z - grid.spacing[2],
        },
        max: crate::affine::Vec3 {
            x: max.x + grid.spacing[0],
            y: max.y + grid.spacing[1],
            z: max.z + grid.spacing[2],
        },
    };
    if expanded.is_valid() {
        Ok(expanded)
    } else {
        Err(SamplingError::InvalidGrid)
    }
}

#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "the finite float count is explicitly range-checked before conversion"
)]
fn cell_count(extent: f64, voxel_size: f64) -> Result<u32, SamplingError> {
    let count = (extent / voxel_size).ceil();
    if !count.is_finite() || count < 1.0 || count > f64::from(u32::MAX - 1) {
        return Err(SamplingError::GridBudget);
    }
    Ok(count as u32)
}

#[cfg(test)]
mod tests;
