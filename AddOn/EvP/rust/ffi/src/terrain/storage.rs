//! Full-capacity and nested provenance pool preflight before foreign borrowing.
use super::{
    BorrowContext, Failure, ProvenanceRequest, TerrainOutput, TerrainRequest, TerrainSplitRequest,
    memory_range,
};
use crate::boundary::overlaps;

pub(super) type Range = Option<(usize, usize)>;
type Checked = Result<Range, Failure>;

fn preflight(inputs: &[Checked], outputs: &[Checked], error: Range) -> Result<(), Failure> {
    if inputs
        .iter()
        .chain(outputs)
        .flatten()
        .any(|&range| overlaps(range, error))
    {
        return Err(Failure::Overlap);
    }
    for &output in outputs.iter().flatten() {
        if inputs
            .iter()
            .flatten()
            .any(|&input| overlaps(input, output))
        {
            return Err(Failure::Overlap);
        }
    }
    for (index, &left) in outputs.iter().flatten().enumerate() {
        if outputs
            .iter()
            .flatten()
            .skip(index + 1)
            .any(|&right| overlaps(left, right))
        {
            return Err(Failure::Overlap);
        }
    }
    // Overlaps of all recognized ranges win over a malformed peer.
    if inputs.iter().chain(outputs).any(Result::is_err) {
        return Err(Failure::Argument(
            "TerrainStorage: null, misaligned or overflowing range",
        ));
    }
    Ok(())
}

fn provenance(request: &ProvenanceRequest) -> Vec<Checked> {
    let t = request.table;
    vec![
        memory_range(t.sources.data, t.sources.count),
        memory_range(t.operations.data, t.operations.count),
        memory_range(t.objects.data, t.objects.count),
        memory_range(t.bindings.data, t.bindings.count),
        memory_range(t.binding_refs.data, t.binding_refs.count),
        memory_range(t.source_ids.data, t.source_ids.count),
        memory_range(t.operation_ids.data, t.operation_ids.count),
        memory_range(t.predecessors.data, t.predecessors.count),
        memory_range(t.mappings.data, t.mappings.count),
        memory_range(t.role_bytes.data, t.role_bytes.count),
        memory_range(t.support_tables.data, t.support_tables.count),
    ]
}

fn outputs(target: TerrainOutput) -> [Checked; 4] {
    [
        memory_range(target.counts.cast_const(), 1),
        memory_range(target.points.cast_const(), target.point_capacity),
        memory_range(target.triangles.cast_const(), target.triangle_capacity),
        memory_range(target.mappings.cast_const(), target.mapping_capacity),
    ]
}

// SAFETY: descriptors must be live initialized immutable POD. Inputs may alias
// each other, but no writable count/array/error may alias either descriptor.
unsafe fn descriptors<I: Copy>(
    input: *const I,
    output: *const TerrainOutput,
    error: Range,
) -> Result<(Option<I>, Option<TerrainOutput>, BorrowContext), Failure> {
    let input_range = memory_range(input, 1);
    let output_range = memory_range(output, 1);
    if [&input_range, &output_range]
        .into_iter()
        .flatten()
        .any(|&range| overlaps(range, error))
    {
        return Err(Failure::Overlap);
    }
    let context = BorrowContext {
        error_range: error,
        output_range: None,
    };
    // Defer either unreadable descriptor until ALL recognizable peer pools have
    // been scanned. Otherwise diagnostics could corrupt valid peer storage.
    let request = if input_range.is_ok() {
        // SAFETY: input descriptor shape/disjointness accepted; caller owns its allocation.
        Some(unsafe { context.record(input)? })
    } else {
        None
    };
    let target = if output_range.is_ok() {
        // SAFETY: output descriptor shape/disjointness accepted; caller owns its allocation.
        Some(unsafe { context.record(output)? })
    } else {
        None
    };
    Ok((request, target, context))
}

fn finish(
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
