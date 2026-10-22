//! Terrain field ABI v1: batch height/normal/gradient/slope/aspect queries.

use super::{BorrowContext, Failure, SliceView, call, memory_range};
use crate::pools::{self, Range};
use tapioca_geometry::terrain::field::{Field, GradientMethod};

/// A mesh and `xy` query pairs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TerrainSampleRequest {
    /// Flat xyz world metres.
    pub points: SliceView<f64>,
    /// Vertex triples.
    pub triangles: SliceView<u32>,
    /// Flat `x y` pairs, world metres.
    pub queries: SliceView<f64>,
    /// Gradient step in metres, positive.
    pub step: f64,
}

/// Flag: the query is on the surface (every other field is zero otherwise).
pub(crate) const SAMPLE_ON_SURFACE: u32 = 1;
/// Flag: `aspect` holds a bearing (the gradient is not zero).
pub(crate) const SAMPLE_HAS_ASPECT: u32 = 2;

/// One query's answer. `flags` bits 2-3 and 4-5 hold the x and y gradient
/// method: 1 central, 2 one-sided, 3 face plane.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TerrainSample {
    /// Height, metres.
    pub height: f64,
    /// Unit upward normal of the sampled face.
    pub normal: [f64; 3],
    /// `dz/dx`, `dz/dy`.
    pub gradient: [f64; 2],
    /// Radians above horizontal.
    pub slope: f64,
    /// Downslope bearing, radians clockwise from +y.
    pub aspect: f64,
    /// Original triangle index.
    pub face: u32,
    /// [`SAMPLE_ON_SURFACE`], [`SAMPLE_HAS_ASPECT`] and the method bits.
    pub flags: u32,
}

/// What the field covers.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TerrainFieldSummary {
    /// Minimum x, y, z of the upward faces' vertices.
    pub min: [f64; 3],
    /// Maximum x, y, z.
    pub max: [f64; 3],
    /// Plan area of the upward faces, square metres.
    pub area: f64,
    /// Upward faces.
    pub faces: u32,
    /// Triangles excluded as not upward.
    pub excluded: u32,
}

/// Caller-owned outputs: a summary and one sample per query.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TerrainSampleOutput {
    /// Required. Unchanged on refusal.
    pub summary: *mut TerrainFieldSummary,
    /// At least `queries.count / 2` samples.
    pub samples: *mut TerrainSample,
    /// Capacity of `samples`.
    pub sample_capacity: usize,
}

const fn method(method: GradientMethod) -> u32 {
    match method {
        GradientMethod::Central => 1,
        GradientMethod::OneSided => 2,
        GradientMethod::Face => 3,
    }
}

fn answer(field: &Field, x: f64, y: f64, step: f64) -> Result<TerrainSample, Failure> {
    let failure =
        |error: tapioca_geometry::terrain::field::FieldError| Failure::Argument(error.message());
    let (Some(sample), Some(normal), Some(gradient)) = (
        field.height_at(x, y),
        field.normal_at(x, y),
        field.gradient_at(x, y, step).map_err(failure)?,
    ) else {
        return Ok(TerrainSample::default());
    };
    let aspect = field.aspect_at(x, y, step).map_err(failure)?;
    let [mx, my] = gradient.method;
    Ok(TerrainSample {
        height: sample.height,
        normal,
        gradient: gradient.slope,
        slope: gradient.slope[0].hypot(gradient.slope[1]).atan(),
        aspect: aspect.unwrap_or(0.0),
        face: sample.face,
        flags: SAMPLE_ON_SURFACE
            | if aspect.is_some() {
                SAMPLE_HAS_ASPECT
            } else {
                0
            }
            | (method(mx) << 2)
            | (method(my) << 4),
    })
}

// SAFETY: request pools, outputs and error are preflighted disjoint first.
unsafe fn sample(
    input: *const TerrainSampleRequest,
    output: *const TerrainSampleOutput,
    error: Range,
) -> Result<(TerrainFieldSummary, Vec<TerrainSample>, TerrainSampleOutput), Failure> {
    // SAFETY: descriptors validates both POD shapes before copying them.
    let (request, target, context): (_, _, BorrowContext) =
        unsafe { pools::descriptors(input, output, error)? };
    let mut inputs = vec![memory_range(input, 1), memory_range(output, 1)];
    if let Some(r) = &request {
        inputs.extend([
            memory_range(r.points.data, r.points.count),
            memory_range(r.triangles.data, r.triangles.count),
            memory_range(r.queries.data, r.queries.count),
        ]);
    }
    let outputs = target.map_or_else(Vec::new, |t| {
        vec![
            memory_range(t.summary.cast_const(), 1),
            memory_range(t.samples.cast_const(), t.sample_capacity),
        ]
    });
    pools::preflight(&inputs, &outputs, error)?;
    let request = request.ok_or(Failure::Argument("TerrainInputDescriptor"))?;
    let target = target.ok_or(Failure::Argument("TerrainOutputDescriptor"))?;
    if !request.queries.count.is_multiple_of(2) {
        return Err(Failure::Argument(
            "TerrainSampleShape: queries are x y pairs",
        ));
    }
    if target.sample_capacity < request.queries.count / 2 {
        return Err(Failure::Argument("TerrainOutputTooSmall"));
    }
    // SAFETY: every pool was preflighted against outputs, descriptors and error.
    let (points, triangles, queries) = unsafe {
        (
            context.slice(request.points)?,
            context.slice(request.triangles)?,
            context.slice(request.queries)?,
        )
    };
    let field =
        Field::new(points, triangles).map_err(|error| Failure::Argument(error.message()))?;
    let samples = queries
        .as_chunks::<2>()
        .0
        .iter()
        .map(|&[x, y]| answer(&field, x, y, request.step))
        .collect::<Result<Vec<_>, _>>()?;
    let bounds = field.bounds();
    let coverage = field.coverage();
    let summary = TerrainFieldSummary {
        min: bounds.min,
        max: bounds.max,
        area: coverage.area,
        faces: coverage.faces,
        excluded: coverage.excluded,
    };
    Ok((summary, samples, target))
}

/// Sample a terrain field at query points.
///
/// # Safety
/// Same live/aligned/disjoint/unchanged-on-refusal contract as
/// `tapioca_terrain_triangulate`, for one summary and `queries.count / 2`
/// samples. A query step that is not finite and positive refuses.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_terrain_sample(
    input: *const TerrainSampleRequest,
    output: *const TerrainSampleOutput,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: call validates error storage and contains panics; preflight first.
    unsafe {
        call(error, capacity, || {
            let (summary, samples, target) =
                sample(input, output, memory_range(error.cast_const(), capacity)?)?;
            // SAFETY: capacity checked; ranges disjoint; POD copies cannot unwind.
            if !samples.is_empty() {
                std::ptr::copy_nonoverlapping(samples.as_ptr(), target.samples, samples.len());
            }
            target.summary.write(summary);
            Ok(())
        })
    }
}
