//! Full-capacity and nested provenance pool preflight before foreign borrowing.
use super::{
    BorrowContext, Failure, TerrainOutput, TerrainRequest, TerrainSplitRequest, memory_range,
};
use crate::pools::{Checked, descriptors, preflight, provenance};

pub(super) type Range = crate::pools::Range;

fn outputs(target: TerrainOutput) -> [Checked; 4] {
    [
        memory_range(target.counts.cast_const(), 1),
        memory_range(target.points.cast_const(), target.point_capacity),
        memory_range(target.triangles.cast_const(), target.triangle_capacity),
        memory_range(target.mappings.cast_const(), target.mapping_capacity),
    ]
}

pub(super) fn finish(
    inputs: &[Checked],
    target: Option<TerrainOutput>,
    error: Range,
) -> Result<TerrainOutput, Failure> {
    let output_ranges =
        target.map_or_else(Vec::new, |target| outputs(target).into_iter().collect());
    preflight(inputs, &output_ranges, error)?;
    let target = target.ok_or(Failure::Argument("TerrainOutputDescriptor"))?;
    if target.query_only > 1
        || (target.query_only == 1
            && (target.point_capacity != 0
                || target.triangle_capacity != 0
                || target.mapping_capacity != 0))
    {
        return Err(Failure::Argument("TerrainOutputMode"));
    }
    Ok(target)
}

// SAFETY: caller supplies immutable initialized descriptors and every nested pool,
// and exclusive output allocations. No slice is formed before finish succeeds.
pub(super) unsafe fn tin(
    input: *const TerrainRequest,
    output: *const TerrainOutput,
    error: Range,
) -> Result<(TerrainRequest, TerrainOutput, BorrowContext), Failure> {
    // SAFETY: descriptors validates POD shapes before copying them.
    let (request, target, context) = unsafe { descriptors(input, output, error)? };
    let mut inputs = vec![memory_range(input, 1), memory_range(output, 1)];
    if let Some(request) = &request {
        inputs.extend(provenance(&request.provenance));
        inputs.extend([
            memory_range(request.points.data, request.points.count),
            memory_range(request.constraints.data, request.constraints.count),
            memory_range(
                request.constraint_vertices.data,
                request.constraint_vertices.count,
            ),
        ]);
    }
    let target = finish(&inputs, target, error)?;
    let request = request.ok_or(Failure::Argument("TerrainInputDescriptor"))?;
    Ok((request, target, context))
}

// SAFETY: same complete immutable/exclusive allocation contract as tin.
pub(super) unsafe fn split(
    input: *const TerrainSplitRequest,
    output: *const TerrainOutput,
    error: Range,
) -> Result<(TerrainSplitRequest, TerrainOutput, BorrowContext), Failure> {
    // SAFETY: descriptors validates POD shapes before copying them.
    let (request, target, context) = unsafe { descriptors(input, output, error)? };
    let mut inputs = vec![memory_range(input, 1), memory_range(output, 1)];
    if let Some(request) = &request {
        inputs.extend(provenance(&request.provenance));
        inputs.extend([
            memory_range(request.points.data, request.points.count),
            memory_range(request.triangles.data, request.triangles.count),
            memory_range(request.line.data, request.line.count),
            memory_range(request.arc_angles.data, request.arc_angles.count),
        ]);
    }
    let target = finish(&inputs, target, error)?;
    let request = request.ok_or(Failure::Argument("TerrainInputDescriptor"))?;
    Ok((request, target, context))
}
