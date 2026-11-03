use super::*;
use crate::sdf::{
    BlendProfile, BooleanOperation, CancellationToken, CompileOptions, GridSpec, Primitive,
    SamplingLimits, SamplingStrategy, StackEntry, StackOperation, sample,
};

fn point(x: f64, y: f64, z: f64) -> Vec3 {
    Vec3 { x, y, z }
}

fn operation(id: u64, primitive: Primitive) -> StackOperation {
    StackOperation {
        id,
        primitive,
        operation: BooleanOperation::Union,
        profile: BlendProfile::Sharp,
        blend_radius: 0.0,
        material_id: i32::try_from(id).unwrap_or(i32::MAX),
        enabled: true,
    }
}

fn sphere(id: u64, center: Vec3, radius: f64) -> StackOperation {
    operation(id, Primitive::Sphere { center, radius })
}

fn compile(entries: &[StackEntry]) -> Result<PreparedField, MeshingError> {
    Ok(PreparedField::compile(entries, CompileOptions::default())?)
}

fn grid(field: &PreparedField, voxel: f64) -> Result<GridSpec, MeshingError> {
    let bounds = field.bounds().ok_or(MeshingError::MissingValue)?;
    let bounds = bounds
        .inflated(2.0 * voxel)
        .ok_or(MeshingError::CellBudget)?;
    Ok(GridSpec::new(bounds, voxel)?)
}

fn mesh_with_strategy(
    field: &PreparedField,
    voxel: f64,
    strategy: SamplingStrategy,
) -> Result<MeshResult, MeshingError> {
    build_mesh(
        field,
        voxel,
        strategy,
        BuildLimits::default(),
        &CancellationToken::new(),
    )
}

fn rock() -> Result<PreparedField, MeshingError> {
    let mut second = sphere(2, point(0.8, 0.2, 0.1), 1.2);
    second.profile = BlendProfile::Round;
    second.blend_radius = 0.35;
    compile(&[
        StackEntry::Operation(sphere(1, point(-0.5, 0.0, 0.0), 1.5)),
        StackEntry::Operation(second),
    ])
}

fn cave() -> Result<PreparedField, MeshingError> {
    let body = operation(
        1,
        Primitive::RoundedBox {
            frame: crate::sdf::Frame::IDENTITY,
            half: point(2.0, 1.5, 1.0),
            radius: 0.1,
        },
    );
    let mut tunnel = operation(
        2,
        Primitive::Capsule {
            a: point(-3.0, 0.0, 0.0),
            b: point(3.0, 0.0, 0.0),
            radius: 0.6,
        },
    );
    tunnel.operation = BooleanOperation::Subtract;
    tunnel.profile = BlendProfile::Round;
    tunnel.blend_radius = 0.1;
    compile(&[StackEntry::Operation(body), StackEntry::Operation(tunnel)])
}

fn extrusion() -> Result<PreparedField, MeshingError> {
    compile(&[StackEntry::Operation(operation(
        1,
        Primitive::Extrude {
            frame: crate::sdf::Frame::IDENTITY,
            profile: vec![[-1.5, -1.0], [1.5, -1.0], [1.5, 1.0], [-1.5, 1.0]],
            half_height: 0.75,
        },
    ))])
}

fn assert_same_geometry(actual: &MeshResult, expected: &MeshResult) {
    assert_eq!(actual.positions, expected.positions);
    assert_eq!(actual.normals, expected.normals);
    assert_eq!(actual.triangles, expected.triangles);
    assert_eq!(actual.tri_material, expected.tri_material);
    assert_eq!(actual.content_hash, expected.content_hash);
    assert_eq!(actual.stats.triangles, expected.stats.triangles);
}

fn assert_mesh_invariants(mesh: &MeshResult) -> Result<(), MeshingError> {
    assert_ne!(mesh.triangles.len(), 0);
    assert_eq!(mesh.positions.len(), mesh.normals.len());
    assert_eq!(mesh.triangles.len() / 3, mesh.tri_material.len());
    assert!(mesh.positions.iter().all(|value| value.is_finite()));
    assert!(mesh.normals.iter().all(|value| value.is_finite()));
    assert!(mesh.tri_material.iter().all(|material| *material > 0));
    for normal in mesh.normals.as_chunks::<3>().0 {
        let length = normal
            .iter()
            .map(|value| f64::from(*value) * f64::from(*value))
            .sum::<f64>()
            .sqrt();
        assert!((length - 1.0).abs() < 2e-6, "normal length {length}");
    }

    let mut edges = BTreeMap::<(u32, u32), usize>::new();
    for triangle in mesh.triangles.as_chunks::<3>().0 {
        let [a, b, c] = triangle;
        for (from, to) in [(*a, *b), (*b, *c), (*c, *a)] {
            let key = if from < to { (from, to) } else { (to, from) };
            *edges.entry(key).or_default() += 1;
        }
    }
    assert!(
        edges.values().all(|incidence| *incidence == 2),
        "open/nonmanifold edges: {:?}",
        edges.values().filter(|incidence| **incidence != 2).count()
    );
    assert!(signed_volume(mesh)? > 0.0, "mesh winding is inverted");
    Ok(())
}

fn vertex(mesh: &MeshResult, index: u32) -> Result<Vec3, MeshingError> {
    let start = usize::try_from(index)
        .ok()
        .and_then(|value| value.checked_mul(3))
        .ok_or(MeshingError::MissingValue)?;
    let coordinates = mesh
        .positions
        .get(start..start + 3)
        .ok_or(MeshingError::MissingValue)?;
    let [x, y, z] = coordinates else {
        return Err(MeshingError::MissingValue);
    };
    Ok(point(*x, *y, *z))
}

fn signed_volume(mesh: &MeshResult) -> Result<f64, MeshingError> {
    let mut volume = 0.0;
    for triangle in mesh.triangles.as_chunks::<3>().0 {
        let [a, b, c] = triangle;
        let a = vertex(mesh, *a)?;
        let b = vertex(mesh, *b)?;
        let c = vertex(mesh, *c)?;
        volume += dot(a, cross(b, c)) / 6.0;
    }
    if volume.is_finite() {
        Ok(volume)
    } else {
        Err(MeshingError::NonFinite)
    }
}

#[test]
fn rock_outcrop_is_watertight_deterministic_and_sparse_dense_identical() -> Result<(), MeshingError>
{
    let field = rock()?;
    let dense = mesh_with_strategy(&field, 0.18, SamplingStrategy::Dense)?;
    let sparse = mesh_with_strategy(&field, 0.18, SamplingStrategy::Sparse)?;
    assert_mesh_invariants(&dense)?;
    assert_same_geometry(&dense, &sparse);
    assert_eq!(
        dense,
        mesh_with_strategy(&field, 0.18, SamplingStrategy::Dense)?
    );
    assert_eq!(dense.stats.slab_cells, 64);
    assert!(sparse.sampling.evaluated_points < sparse.sampling.total_points);
    Ok(())
}

#[test]
fn box_minus_capsule_cave_is_watertight_and_keeps_cut_material() -> Result<(), MeshingError> {
    let field = cave()?;
    let dense = mesh_with_strategy(&field, 0.16, SamplingStrategy::Dense)?;
    let sparse = mesh_with_strategy(&field, 0.16, SamplingStrategy::Sparse)?;
    assert_mesh_invariants(&dense)?;
    assert_same_geometry(&dense, &sparse);
    let materials = dense.tri_material.iter().copied().collect::<BTreeSet<_>>();
    assert_eq!(materials, BTreeSet::from([1, 2]));
    Ok(())
}

#[test]
fn exact_profile_extrusion_handles_grid_aligned_faces() -> Result<(), MeshingError> {
    let field = extrusion()?;
    let mesh = mesh_with_strategy(&field, 0.25, SamplingStrategy::Sparse)?;
    assert_mesh_invariants(&mesh)?;
    assert!(mesh.tri_material.iter().all(|material| *material == 1));
    Ok(())
}

#[test]
fn survey_coordinates_stay_finite_f64_and_deterministic() -> Result<(), MeshingError> {
    let field = compile(&[StackEntry::Operation(sphere(
        1,
        point(1_000_000.25, 2_000_000.5, -3_000.75),
        1.0,
    ))])?;
    let mesh = mesh_with_strategy(&field, 0.2, SamplingStrategy::Sparse)?;
    assert_mesh_invariants(&mesh)?;
    assert!(mesh.positions.iter().any(|value| value.abs() > 1_000_000.0));
    assert_same_geometry(
        &mesh,
        &mesh_with_strategy(&field, 0.2, SamplingStrategy::Dense)?,
    );
    Ok(())
}

#[test]
fn cancellation_empty_surface_and_triangle_budget_publish_nothing() -> Result<(), MeshingError> {
    let field = rock()?;
    let sampled = sample(
        &field,
        grid(&field, 0.25)?,
        SamplingStrategy::Sparse,
        SamplingLimits::default(),
        &CancellationToken::new(),
    )?;
    let cancel = CancellationToken::new();
    cancel.cancel();
    assert!(matches!(
        marching_cubes(&field, &sampled, MeshingLimits::default(), &cancel),
        Err(MeshingError::Cancelled)
    ));
    assert!(matches!(
        marching_cubes(
            &field,
            &sampled,
            MeshingLimits { max_triangles: 1 },
            &CancellationToken::new(),
        ),
        Err(MeshingError::TriangleBudget)
    ));

    let outside = GridSpec::new(
        crate::sdf::Bounds {
            min: point(10.0, 10.0, 10.0),
            max: point(12.0, 12.0, 12.0),
        },
        0.5,
    )?;
    let empty = sample(
        &field,
        outside,
        SamplingStrategy::Sparse,
        SamplingLimits::default(),
        &CancellationToken::new(),
    )?;
    assert!(matches!(
        marching_cubes(
            &field,
            &empty,
            MeshingLimits::default(),
            &CancellationToken::new(),
        ),
        Err(MeshingError::EmptySurface)
    ));
    Ok(())
}
