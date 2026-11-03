use super::*;
use crate::sdf::{
    BlendProfile, BooleanOperation, CompileOptions, Primitive, StackEntry, StackOperation,
};

fn point(x: f64, y: f64, z: f64) -> crate::affine::Vec3 {
    crate::affine::Vec3 { x, y, z }
}

fn sphere(radius: f64) -> Result<PreparedField, SamplingError> {
    Ok(PreparedField::compile(
        &[StackEntry::Operation(StackOperation {
            id: 1,
            primitive: Primitive::Sphere {
                center: point(0.0, 0.0, 0.0),
                radius,
            },
            operation: BooleanOperation::Union,
            profile: BlendProfile::Sharp,
            blend_radius: 0.0,
            material_id: 7,
            enabled: true,
        })],
        CompileOptions::default(),
    )?)
}

fn padded_grid(field: &PreparedField, voxel: f64) -> Result<GridSpec, SamplingError> {
    let bounds = field.bounds().ok_or(SamplingError::InvalidGrid)?;
    let bounds = bounds.inflated(4.0).ok_or(SamplingError::InvalidGrid)?;
    GridSpec::new(bounds, voxel)
}

#[test]
fn grid_uses_node_counts_exact_bounds_and_checked_dimensions() -> Result<(), SamplingError> {
    let grid = GridSpec::new(
        Bounds {
            min: point(0.0, 0.0, 0.0),
            max: point(2.0, 2.0, 2.0),
        },
        0.6,
    )?;
    assert_eq!(grid.cells, [4, 4, 4]);
    assert_eq!(grid.nodes(), [5, 5, 5]);
    assert_eq!(grid.spacing, [0.5, 0.5, 0.5]);
    assert_eq!(grid.total_points()?, 125);
    assert_eq!(grid.point([4, 4, 4]), Some(point(2.0, 2.0, 2.0)));
    assert_eq!(grid.point([5, 0, 0]), None);
    assert!(matches!(
        GridSpec::new(grid.bounds, 0.0),
        Err(SamplingError::InvalidVoxelSize)
    ));
    Ok(())
}

#[test]
fn dense_sampling_matches_scalar_evaluation_in_canonical_order() -> Result<(), SamplingError> {
    let field = sphere(1.0)?;
    let grid = padded_grid(&field, 0.5)?;
    let sampled = sample(
        &field,
        grid,
        SamplingStrategy::Dense,
        SamplingLimits::default(),
        &CancellationToken::new(),
    )?;
    assert_eq!(sampled.stats.evaluated_points, grid.total_points()?);
    assert_eq!(sampled.stats.stored_sparse_points, 0);
    for z in 0..grid.nodes()[2] {
        for y in 0..grid.nodes()[1] {
            for x in 0..grid.nodes()[0] {
                let index = [x, y, z];
                let exact = field.evaluate(grid.point(index).ok_or(SamplingError::InvalidGrid)?)?;
                assert_eq!(
                    sampled.value(index).map(f64::to_bits),
                    Some(exact.distance.to_bits())
                );
                assert!(sampled.is_evaluated(index));
            }
        }
    }
    Ok(())
}

#[test]
fn sparse_sampling_preserves_every_dense_sign_and_active_value() -> Result<(), SamplingError> {
    let field = sphere(2.0)?;
    let grid = padded_grid(&field, 0.35)?;
    let limits = SamplingLimits::default();
    let dense = sample(
        &field,
        grid,
        SamplingStrategy::Dense,
        limits,
        &CancellationToken::new(),
    )?;
    let sparse = sample(
        &field,
        grid,
        SamplingStrategy::Sparse,
        limits,
        &CancellationToken::new(),
    )?;
    assert!(sparse.stats.evaluated_points < sparse.stats.total_points);
    assert!(sparse.stats.skipped_regions > 0);
    assert_eq!(
        sparse.stats.evaluated_points,
        sparse.stats.stored_sparse_points
    );
    for z in 0..grid.nodes()[2] {
        for y in 0..grid.nodes()[1] {
            for x in 0..grid.nodes()[0] {
                let index = [x, y, z];
                let expected = dense.value(index).ok_or(SamplingError::Incomplete)?;
                let actual = sparse.value(index).ok_or(SamplingError::Incomplete)?;
                if sparse.is_evaluated(index) {
                    assert_eq!(actual.to_bits(), expected.to_bits());
                } else {
                    assert_eq!(actual.abs(), 1.0);
                    assert_eq!(actual.signum(), expected.signum());
                }
            }
        }
    }
    let repeated = sample(
        &field,
        grid,
        SamplingStrategy::Sparse,
        limits,
        &CancellationToken::new(),
    )?;
    assert_eq!(sparse, repeated);
    Ok(())
}

#[test]
fn a_wholly_outside_sparse_region_stores_only_positive_signs() -> Result<(), SamplingError> {
    let field = sphere(1.0)?;
    let grid = GridSpec::new(
        Bounds {
            min: point(5.0, 5.0, 5.0),
            max: point(7.0, 7.0, 7.0),
        },
        0.25,
    )?;
    let sampled = sample(
        &field,
        grid,
        SamplingStrategy::Sparse,
        SamplingLimits::default(),
        &CancellationToken::new(),
    )?;
    assert_eq!(sampled.stats.evaluated_points, 0);
    assert_eq!(sampled.stats.stored_sparse_points, 0);
    assert_eq!(sampled.stats.skipped_regions, 1);
    assert_eq!(sampled.value([0, 0, 0]), Some(1.0));
    assert!(!sampled.is_evaluated([0, 0, 0]));
    Ok(())
}

#[test]
fn budgets_and_cancellation_return_no_partial_grid() -> Result<(), SamplingError> {
    let field = sphere(2.0)?;
    let grid = padded_grid(&field, 0.35)?;
    let dense_budget = SamplingLimits {
        max_dense_points: 10,
        max_sparse_stored_points: 8_000_000,
    };
    assert!(matches!(
        sample(
            &field,
            grid,
            SamplingStrategy::Dense,
            dense_budget,
            &CancellationToken::new(),
        ),
        Err(SamplingError::GridBudget)
    ));
    let sparse_budget = SamplingLimits {
        max_dense_points: 64_000_000,
        max_sparse_stored_points: 1,
    };
    assert!(matches!(
        sample(
            &field,
            grid,
            SamplingStrategy::Sparse,
            sparse_budget,
            &CancellationToken::new(),
        ),
        Err(SamplingError::SparseBudget)
    ));
    let cancel = CancellationToken::new();
    cancel.cancel();
    assert!(matches!(
        sample(
            &field,
            grid,
            SamplingStrategy::Sparse,
            SamplingLimits::default(),
            &cancel,
        ),
        Err(SamplingError::Cancelled)
    ));
    Ok(())
}
