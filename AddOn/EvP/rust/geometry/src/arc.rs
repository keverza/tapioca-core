//! S2b arc tessellation parity with `Curves.cpp::MakeArc`, not an analytic
//! `ArcPolyline` carrier or a production consumer switch. World metres, radians,
//! right-handed Z-up. The adaptive seed and squared-norm normalization match
//! C++ (including underflow refusal); no hypot, welding, closure snap or epsilon.
//! Transcendental results are compared with absolute tolerance, not bitwise
//! across toolchains. Work/output are bounded by 4096 segments; use a worker
//! until the D3 synchronous budget is measured. No identity is allocated.

use crate::affine::Vec3;

/// Complete arc request. The normal need not be unit length.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ArcRequest {
    /// World-metre centre, never translated to a local origin.
    pub centre: Vec3,
    /// Dimensionless plane normal, normalized with the existing squared norm.
    pub normal: Vec3,
    /// Positive finite radius in metres.
    pub radius: f64,
    /// Finite starting angle in radians, not reduced modulo a turn.
    pub start_radians: f64,
    /// Finite nonzero signed sweep in radians; multiple turns are allowed.
    pub sweep_radians: f64,
    /// From 2 through 4096 inclusive; output has segments + 1 points.
    pub segments: i32,
}

/// Arc domain refusals, distinct from ABI memory errors.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// Non-finite coordinates, radius or angles.
    NonFiniteInput,
    /// Nonpositive radius or zero sweep.
    Degenerate,
    /// Outside the C++ segment range.
    Segments,
    /// Zero, underflowed or overflowing squared normal length.
    Normal,
    /// An angle, scaled axis term, partial sum or output is not finite.
    ArithmeticRange,
}

impl Error {
    /// Stable ABI diagnostic prefix.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::NonFiniteInput => "ArcNonFinite: coordinates, radius and angles must be finite",
            Self::Degenerate => "ArcDegenerate: positive radius and nonzero sweep required",
            Self::Segments => "ArcSegments: expected 2..4096",
            Self::Normal => "ArcNormal: squared norm must be finite and positive",
            Self::ArithmeticRange => "ArcArithmeticRange: unrepresentable angle or point",
        }
    }
}

fn components(value: Vec3) -> [f64; 3] {
    [value.x, value.y, value.z]
}

fn unit([x, y, z]: [f64; 3]) -> Result<[f64; 3], Error> {
    let norm = (x * x + y * y + z * z).sqrt();
    if !norm.is_finite() || norm <= 0.0 {
        return Err(Error::Normal);
    }
    Ok([x / norm, y / norm, z / norm])
}

fn cross([ax, ay, az]: [f64; 3], [bx, by, bz]: [f64; 3]) -> [f64; 3] {
    [ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx]
}

/// Tessellate to owned, interleaved xyz points in sweep order. The final point
/// is evaluated, not snapped to the first for full circles. No partial result
/// is returned; inputs remain unchanged and no allocation crosses the C ABI.
///
/// # Errors
/// Returns a typed refusal for malformed/degenerate input, unusable squared
/// normal length or unrepresentable arithmetic. This deliberately refuses C++'s
/// accidental successful NaN/inf output; it does not repair the production oracle.
pub fn make_arc(request: &ArcRequest) -> Result<Vec<f64>, Error> {
    let centre = components(request.centre);
    let normal = components(request.normal);
    if !centre
        .iter()
        .chain(&normal)
        .chain([
            &request.radius,
            &request.start_radians,
            &request.sweep_radians,
        ])
        .all(|value| value.is_finite())
    {
        return Err(Error::NonFiniteInput);
    }
    if request.radius <= 0.0 || request.sweep_radians == 0.0 {
        return Err(Error::Degenerate);
    }
    if !(2..=4096).contains(&request.segments) {
        return Err(Error::Segments);
    }
    let normal = unit(normal)?;
    let [_, _, nz] = normal;
    let seed = if nz.abs() < 0.9 {
        [0.0, 0.0, 1.0]
    } else {
        [1.0, 0.0, 0.0]
    };
    let x_axis = unit(cross(normal, seed))?;
    let y_axis = unit(cross(normal, x_axis))?;
    let count = usize::try_from(request.segments).map_err(|_| Error::Segments)?;
    let mut points = Vec::with_capacity((count + 1) * 3);
    for index in 0..=request.segments {
        let angle = request.start_radians
            + request.sweep_radians * (f64::from(index) / f64::from(request.segments));
        if !angle.is_finite() {
            return Err(Error::ArithmeticRange);
        }
        let c = angle.cos() * request.radius;
        let s = angle.sin() * request.radius;
        for ((origin, x), y) in centre.into_iter().zip(x_axis).zip(y_axis) {
            let first = origin + x * c;
            let point = first + y * s;
            if !c.is_finite() || !s.is_finite() || !first.is_finite() || !point.is_finite() {
                return Err(Error::ArithmeticRange);
            }
            points.push(point);
        }
    }
    Ok(points)
}

#[cfg(test)]
mod tests;
