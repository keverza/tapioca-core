//! Neutral S2b plane values and batch projection. Finite-input parity oracle:
//! `EditSnap.cpp::PlaneAxes/ProjectToPlane`, with `EditTypes.cpp::Normalize`.
//! This owns pure math only, not editor state or `WorkingPlane` provenance.
//! World metres, right-handed Z-up. Raw plane values round-trip without repair.

use crate::affine::Vec3;

/// Legacy plane value, mirroring origin/normal/optional authored u exactly.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Plane {
    /// World-metre plane origin.
    pub origin: Vec3,
    /// Dimensionless direction; finite length below 1e-12 uses world Z for math.
    pub normal: Vec3,
    /// Optional dimensionless authored tangent; zero/short/parallel uses seed axes.
    pub u: Vec3,
}

/// Complete frame plus exact retained source plane, not a normalized replacement.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PlaneFrame {
    /// Unmodified input fields; no authored IDs or revision metadata are inferred.
    pub plane: Plane,
    /// Derived in-plane unit axis.
    pub u: Vec3,
    /// Derived in-plane perpendicular axis, following the C++ cross-product rule.
    pub v: Vec3,
}

/// Transactional output from [`project_to_plane`].
#[derive(Debug, PartialEq)]
pub struct PlaneProjection {
    /// Exact source plane and derived axes, including for an empty point batch.
    pub frame: PlaneFrame,
    /// Flat xyz projected world-metre points in input order.
    pub points: Vec<f64>,
}

/// Plane input/arithmetic refusals, distinct from ABI memory failures.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// Input scalar count is not a multiple of three.
    Shape,
    /// A source plane or point coordinate is NaN/infinite.
    NonFiniteInput,
    /// A squared norm, product, difference, dot sum or result is not finite.
    ArithmeticRange,
}
impl Error {
    /// Allocation-free ABI diagnostic prefix.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::Shape => "PlaneShape: flat points must be whole xyz triples",
            Self::NonFiniteInput => "PlaneNonFinite: plane and points must be finite",
            Self::ArithmeticRange => "PlaneArithmeticRange: unrepresentable frame or projection",
        }
    }
}

fn components(value: Vec3) -> [f64; 3] {
    [value.x, value.y, value.z]
}
fn finite(value: Vec3) -> bool {
    components(value).iter().all(|v| v.is_finite())
}
fn vector([x, y, z]: [f64; 3]) -> Vec3 {
    Vec3 { x, y, z }
}
fn sub(a: Vec3, b: Vec3) -> Result<Vec3, Error> {
    let out = Vec3 {
        x: a.x - b.x,
        y: a.y - b.y,
        z: a.z - b.z,
    };
    if finite(out) {
        Ok(out)
    } else {
        Err(Error::ArithmeticRange)
    }
}
fn scale(a: Vec3, by: f64) -> Result<Vec3, Error> {
    let out = Vec3 {
        x: a.x * by,
        y: a.y * by,
        z: a.z * by,
    };
    if finite(out) {
        Ok(out)
    } else {
        Err(Error::ArithmeticRange)
    }
}
fn dot(a: Vec3, b: Vec3) -> Result<f64, Error> {
    let first = a.x * b.x + a.y * b.y;
    let out = first + a.z * b.z;
    if first.is_finite() && out.is_finite() {
        Ok(out)
    } else {
        Err(Error::ArithmeticRange)
    }
}
fn cross(a: Vec3, b: Vec3) -> Result<Vec3, Error> {
    let out = Vec3 {
        x: a.y * b.z - a.z * b.y,
        y: a.z * b.x - a.x * b.z,
        z: a.x * b.y - a.y * b.x,
    };
    if finite(out) {
        Ok(out)
    } else {
        Err(Error::ArithmeticRange)
    }
}

fn normalized(value: Vec3) -> Result<Option<Vec3>, Error> {
    let length = dot(value, value)?.sqrt();
    // Exact legacy threshold, not a world-metre tolerance or global epsilon.
    if length < 1e-12 {
        return Ok(None);
    }
    // C++ multiplies by the reciprocal, unlike Curves/GLM's component division.
    Ok(Some(scale(value, 1.0 / length)?))
}

fn frame(plane: &Plane) -> Result<(PlaneFrame, Vec3), Error> {
    let normal = normalized(plane.normal)?.unwrap_or(Vec3 {
        x: 0.0,
        y: 0.0,
        z: 1.0,
    });
    let projected = sub(plane.u, scale(normal, dot(plane.u, normal)?)?)?;
    let (u, v) = if let Some(u) = normalized(projected)? {
        (u, cross(normal, u)?)
    } else {
        let seed = if normal.x.abs() < 0.9 {
            Vec3 {
                x: 1.0,
                y: 0.0,
                z: 0.0,
            }
        } else {
            Vec3 {
                x: 0.0,
                y: 1.0,
                z: 0.0,
            }
        };
        let v = normalized(cross(normal, seed)?)?.ok_or(Error::ArithmeticRange)?;
        let u = normalized(cross(v, normal)?)?.ok_or(Error::ArithmeticRange)?;
        (u, v)
    };
    Ok((
        PlaneFrame {
            plane: *plane,
            u,
            v,
        },
        normal,
    ))
}

/// Derive a frame and project a borrowed batch, including empty/frame-only input.
/// Plane fields round-trip exactly (including signed zero); only derived math
/// uses normalized/fallback directions. No input mutation, welding, retained
/// borrow, identity generation or production editor switch. O(points) work and
/// one owned point vector; worker-classified until D3 budgets are measured.
///
/// # Errors
/// Malformed xyz, non-finite input or non-finite intermediate arithmetic refuse
/// without a partial result. Finite tiny normal/tangent fallbacks are preserved;
/// C++ accidental non-finite-arithmetic fallback/success is not reproduced.
pub fn project_to_plane(plane: &Plane, points: &[f64]) -> Result<PlaneProjection, Error> {
    let (triples, remainder) = points.as_chunks::<3>();
    if !remainder.is_empty() {
        return Err(Error::Shape);
    }
    if !finite(plane.origin)
        || !finite(plane.normal)
        || !finite(plane.u)
        || !points.iter().all(|v| v.is_finite())
    {
        return Err(Error::NonFiniteInput);
    }
    let (frame, normal) = frame(plane)?;
    let mut projected = Vec::with_capacity(points.len());
    for &xyz in triples {
        let point = vector(xyz);
        let distance = dot(sub(point, plane.origin)?, normal)?;
        let out = sub(point, scale(normal, distance)?)?;
        projected.extend(components(out));
    }
    Ok(PlaneProjection {
        frame,
        points: projected,
    })
}

#[cfg(test)]
mod tests;
