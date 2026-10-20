//! S2b world-metre, right-handed Z-up polyline measurement and batch sampling.
//!
//! Mirrors `Curves.cpp`: cumulative 3D lengths in vertex order, finite parameters
//! clamped to [0,1], the incoming segment at exact joins, and +X tangents for
//! zero-length segments/curves. No closure is inferred, no vertices are welded,
//! and no identities, topology or provenance are changed. This is not an arc or
//! polygon operation. Non-finite input or unrepresentable arithmetic refuses,
//! rather than reproducing the C++ oracle's accidental NaN/inf successes.
//! Input borrows end at return; all output is provisional until the batch succeeds.
//! Work is O(vertices + samples * log(vertices)); bulk calls belong on workers
//! until D3 latency/copy/allocation measurements justify synchronous use.

/// Owned finite output from [`sample_polyline`]. No semantic IDs are allocated.
#[derive(Debug, PartialEq)]
pub struct PolylineSamples {
    /// Cumulative world-metre distance at each input vertex, starting at zero.
    pub cumulative: Vec<f64>,
    /// Same ordered reduction as the last cumulative entry, in metres.
    pub total_length: f64,
    /// Interleaved xyz positions in parameter order, in world metres.
    pub points: Vec<f64>,
    /// Interleaved dimensionless xyz directions normalized with the C++ squared
    /// norm (including its underflow behavior), one per sample.
    pub tangents: Vec<f64>,
}

/// Typed polyline input/arithmetic refusals, separate from ABI memory errors.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// Not whole xyz triples, or fewer than two points.
    Shape,
    /// A coordinate or parameter is NaN/infinite.
    NonFiniteInput,
    /// A difference, squared norm, cumulative length or sample is non-finite.
    ArithmeticRange,
    /// Segment count must be in the existing C++ range 1..=100000.
    DivideCount,
}

impl Error {
    /// Stable diagnostic prefixes for the shared status/error boundary.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::Shape => "PolylineShape: expected at least two whole xyz points",
            Self::NonFiniteInput => "PolylineNonFinite: coordinates and parameters must be finite",
            Self::ArithmeticRange => "PolylineArithmeticRange: unrepresentable length or sample",
            Self::DivideCount => "PolylineDivideCount: expected between 1 and 100000 segments",
        }
    }
}

fn delta(a: &[f64; 3], b: &[f64; 3]) -> [f64; 3] {
    let &[ax, ay, az] = a;
    let &[bx, by, bz] = b;
    [bx - ax, by - ay, bz - az]
}

fn length([x, y, z]: [f64; 3]) -> Result<f64, Error> {
    // Preserve C++ Length/GLM sum association; no hypot/fast-math substitution.
    let value = (x * x + y * y + z * z).sqrt();
    if value.is_finite() {
        Ok(value)
    } else {
        Err(Error::ArithmeticRange)
    }
}

fn sample_capacity(count: usize) -> Result<usize, Error> {
    count
        .checked_mul(3)
        .filter(|&scalars| scalars <= isize::MAX.unsigned_abs() / size_of::<f64>())
        .ok_or(Error::ArithmeticRange)
}

fn sample(
    vertices: &[[f64; 3]],
    cumulative: &[f64],
    total: f64,
    t: f64,
) -> Result<([f64; 3], [f64; 3]), Error> {
    if total <= 0.0 {
        return Ok((*vertices.first().ok_or(Error::Shape)?, [1.0, 0.0, 0.0]));
    }
    let target = t.clamp(0.0, 1.0) * total;
    let ends = cumulative.get(1..).ok_or(Error::Shape)?;
    // First end >= target, including repeated cumulative entries. C++ chooses
    // the incoming segment at joins, even when it has zero length at t=0.
    let index = ends
        .partition_point(|&end| end < target)
        .min(ends.len() - 1);
    let Some([a, b]) = vertices.get(index..=index + 1) else {
        return Err(Error::Shape);
    };
    let Some([start, end]) = cumulative.get(index..=index + 1) else {
        return Err(Error::Shape);
    };
    let segment = end - start;
    let along = if segment <= 0.0 {
        0.0
    } else {
        (target - start) / segment
    };
    let difference = delta(a, b);
    let norm = length(difference)?;
    let mut point = [0.0; 3];
    let mut tangent = [1.0, 0.0, 0.0];
    for ((out, &origin), &direction) in point.iter_mut().zip(a).zip(&difference) {
        *out = origin + direction * along;
    }
    if norm > 0.0 {
        for (out, direction) in tangent.iter_mut().zip(difference) {
            *out = direction / norm;
        }
    }
    if point.iter().chain(&tangent).all(|value| value.is_finite()) {
        Ok((point, tangent))
    } else {
        Err(Error::ArithmeticRange)
    }
}

/// Measure a borrowed flat xyz polyline and sample all requested length fractions.
/// Empty parameters perform measurement only. Duplicate points and explicit
/// closure are retained exactly. All-coincident points are valid, not repaired.
/// No caller buffer is changed, and failure returns no partial result.
///
/// # Errors
/// [`Error::Shape`] for malformed xyz/fewer than two points;
/// [`Error::NonFiniteInput`] for any non-finite coordinate/parameter;
/// [`Error::ArithmeticRange`] for unrepresentable arithmetic or sample capacity.
pub fn sample_polyline(points: &[f64], parameters: &[f64]) -> Result<PolylineSamples, Error> {
    let (vertices, remainder) = points.as_chunks::<3>();
    if !remainder.is_empty() || vertices.len() < 2 {
        return Err(Error::Shape);
    }
    if !points
        .iter()
        .chain(parameters)
        .all(|value| value.is_finite())
    {
        return Err(Error::NonFiniteInput);
    }
    let capacity = sample_capacity(parameters.len())?;
    let mut cumulative = Vec::with_capacity(vertices.len());
    cumulative.push(0.0);
    let mut total = 0.0;
    for pair in vertices.windows(2) {
        if let [a, b] = pair {
            total += length(delta(a, b))?;
        }
        if !total.is_finite() {
            return Err(Error::ArithmeticRange);
        }
        cumulative.push(total);
    }
    let mut sampled = Vec::with_capacity(capacity);
    let mut tangents = Vec::with_capacity(capacity);
    for &t in parameters {
        let (point, tangent) = sample(vertices, &cumulative, total, t)?;
        sampled.extend(point);
        tangents.extend(tangent);
    }
    Ok(PolylineSamples {
        cumulative,
        total_length: total,
        points: sampled,
        tangents,
    })
}

/// Number of output positions for the C++ segment-count/end-inclusion contract.
/// Used by ABI ingress to check capacity before computation/allocation.
///
/// # Errors
/// [`Error::DivideCount`] outside 1..=100000 segments.
pub fn division_point_count(count: u32, include_ends: bool) -> Result<usize, Error> {
    if !(1..=100_000).contains(&count) {
        return Err(Error::DivideCount);
    }
    usize::try_from(if include_ends { count + 1 } else { count - 1 })
        .map_err(|_| Error::ArithmeticRange)
}

/// Divide into `count` equal arc-length segments, preserving `Curves.cpp` order.
/// N segments return N+1 positions with ends, N-1 without. No implicit closure,
/// identity/lineage generation, welding or polygon validity is implied.
/// Uses the same measurement/sampling implementation as [`sample_polyline`].
/// One segment without ends returns empty after shape/count/finite ingress;
/// it does not evaluate unused lengths, matching the C++ early return on finite
/// inputs. Otherwise arithmetic must remain finite, including tangent evaluation
/// even though tangents are not returned (the C++ oracle also evaluates them).
/// O(vertices + count * log(vertices)), worker-classified until measured.
///
/// # Errors
/// Malformed xyz/fewer than two vertices, invalid count, non-finite coordinates
/// or unrepresentable used arithmetic refuse without a partial result.
pub fn divide_polyline(points: &[f64], count: u32, include_ends: bool) -> Result<Vec<f64>, Error> {
    let (vertices, remainder) = points.as_chunks::<3>();
    if !remainder.is_empty() || vertices.len() < 2 {
        return Err(Error::Shape);
    }
    let samples = division_point_count(count, include_ends)?;
    if !points.iter().all(|value| value.is_finite()) {
        return Err(Error::NonFiniteInput);
    }
    let capacity = sample_capacity(samples)?;
    if samples == 0 {
        return Ok(Vec::new());
    }
    let measured = sample_polyline(points, &[])?;
    let mut output = Vec::with_capacity(capacity);
    let start = u32::from(!include_ends);
    let end = if include_ends { count + 1 } else { count };
    for index in start..end {
        let parameter = f64::from(index) / f64::from(count);
        let (point, _) = sample(
            vertices,
            &measured.cumulative,
            measured.total_length,
            parameter,
        )?;
        output.extend(point);
    }
    Ok(output)
}

#[cfg(test)]
mod divide_tests;
#[cfg(test)]
mod tests;
