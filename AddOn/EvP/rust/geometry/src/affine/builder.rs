//! Allocation-free builders following `Transforms.cpp`, including ordered
//! pivot conjugation and squared-norm normalization. No local-origin shift,
//! hypot, epsilon, angle reduction or topology/identity operation.

use super::{Transform, Validated, Vec3, is_finite};

/// Typed builder inputs; all positions/translations are world metres.
#[derive(Clone, Copy, Debug)]
pub enum TransformBuilder {
    /// World-metre displacement, no pivot.
    Translation(Vec3),
    /// Right-handed rotation through a world-metre origin, angle in radians.
    Rotation {
        /// Pivot in world metres.
        origin: Vec3,
        /// Dimensionless direction; normalized via the existing squared norm.
        axis: Vec3,
        /// Signed angle in radians, without modulo reduction.
        radians: f64,
    },
    /// Nonzero dimensionless per-axis factors, about a world-metre origin.
    Scaling {
        /// Pivot in world metres.
        origin: Vec3,
        /// Finite nonzero dimensionless per-axis factors.
        factors: Vec3,
    },
    /// Reflection through an origin with a dimensionless, not necessarily unit normal.
    Mirroring {
        /// A point on the plane, in world metres.
        origin: Vec3,
        /// Dimensionless direction; normalized via the existing squared norm.
        normal: Vec3,
    },
}

/// Builder-specific refusals, distinct from application and ABI memory errors.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BuildError {
    /// An input position, direction, factor or angle is not finite.
    NonFiniteInput,
    /// Squared axis/normal length is zero, underflowed or non-finite.
    Direction,
    /// At least one scale factor is exactly zero, including negative zero.
    ZeroScale,
    /// A coefficient, ordered pivot sum or determinant is not finite.
    ArithmeticRange,
}
impl BuildError {
    /// Stable allocation-free ABI diagnostic prefix.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::NonFiniteInput => "TransformBuildNonFinite: all used inputs must be finite",
            Self::Direction => {
                "TransformBuildDirection: squared length must be finite and positive"
            }
            Self::ZeroScale => "TransformBuildZeroScale: scale factors must be nonzero",
            Self::ArithmeticRange => {
                "TransformBuildArithmeticRange: coefficient, pivot sum or determinant overflow"
            }
        }
    }
}

fn components(value: Vec3) -> [f64; 3] {
    [value.x, value.y, value.z]
}

fn unit(value: Vec3) -> Result<[f64; 3], BuildError> {
    let Vec3 { x, y, z } = value;
    let norm = (x * x + y * y + z * z).sqrt();
    if !norm.is_finite() || norm <= 0.0 {
        return Err(BuildError::Direction);
    }
    Ok([x / norm, y / norm, z / norm])
}

fn conjugate(linear: [[f64; 3]; 3], origin: Vec3) -> Result<Transform, BuildError> {
    let centre = components(origin);
    let mut result = Transform::IDENTITY;
    for ((target, row), start) in result.m.iter_mut().zip(linear).zip(centre) {
        let mut shifted = start;
        for ((out, value), coordinate) in target.iter_mut().zip(row).zip(centre) {
            *out = value;
            shifted -= value * coordinate;
            // Preserve the ordered C++ subtraction, not origin - Apply(linear, origin).
            if !shifted.is_finite() {
                return Err(BuildError::ArithmeticRange);
            }
        }
        let [_, _, _, translation] = target;
        *translation = shifted;
    }
    Ok(result)
}

fn rotation(origin: Vec3, axis: Vec3, radians: f64) -> Result<Transform, BuildError> {
    if !is_finite(origin) || !is_finite(axis) || !radians.is_finite() {
        return Err(BuildError::NonFiniteInput);
    }
    let [x, y, z] = unit(axis)?;
    let cosine = radians.cos();
    let sine = radians.sin();
    let complement = 1.0 - cosine;
    conjugate(
        [
            [
                complement * x * x + cosine,
                complement * x * y - sine * z,
                complement * x * z + sine * y,
            ],
            [
                complement * x * y + sine * z,
                complement * y * y + cosine,
                complement * y * z - sine * x,
            ],
            [
                complement * x * z - sine * y,
                complement * y * z + sine * x,
                complement * z * z + cosine,
            ],
        ],
        origin,
    )
}

fn scaling(origin: Vec3, factors: Vec3) -> Result<Transform, BuildError> {
    if !is_finite(origin) || !is_finite(factors) {
        return Err(BuildError::NonFiniteInput);
    }
    let factors = components(factors);
    if factors.contains(&0.0) {
        return Err(BuildError::ZeroScale);
    }
    let mut result = Transform::IDENTITY;
    for (index, ((row, scale), coordinate)) in result
        .m
        .iter_mut()
        .zip(factors)
        .zip(components(origin))
        .enumerate()
    {
        *row.get_mut(index).ok_or(BuildError::ArithmeticRange)? = scale;
        let [_, _, _, translation] = row;
        *translation = coordinate - scale * coordinate;
    }
    Ok(result)
}

fn mirroring(origin: Vec3, normal: Vec3) -> Result<Transform, BuildError> {
    if !is_finite(origin) || !is_finite(normal) {
        return Err(BuildError::NonFiniteInput);
    }
    let normal = unit(normal)?;
    let mut linear = [[0.0; 3]; 3];
    for (row_index, (row, nr)) in linear.iter_mut().zip(normal).enumerate() {
        for (column_index, (target, nc)) in row.iter_mut().zip(normal).enumerate() {
            *target = f64::from(u8::from(row_index == column_index)) - 2.0 * nr * nc;
        }
    }
    conjugate(linear, origin)
}

/// Build one 3x4 matrix with no allocation, input repair or partial result.
/// Degenerate direction and exactly zero factors follow the compiled C++ oracle;
/// finite nonzero scale factors whose determinant underflows to zero remain valid.
/// Every successful result meets the existing affine finite-coefficient/determinant
/// gate. Worker-classified until D3 timing is measured despite constant bounded work.
///
/// # Errors
/// Returns typed input/domain or unrepresentable arithmetic refusal. Accidental
/// C++ successes containing infinity/NaN are deliberately not reproduced.
pub fn build_transform(builder: TransformBuilder) -> Result<Transform, BuildError> {
    let result = match builder {
        TransformBuilder::Translation(by) => {
            if !is_finite(by) {
                return Err(BuildError::NonFiniteInput);
            }
            let mut result = Transform::IDENTITY;
            for (row, coordinate) in result.m.iter_mut().zip(components(by)) {
                let [_, _, _, translation] = row;
                *translation = coordinate;
            }
            result
        }
        TransformBuilder::Rotation {
            origin,
            axis,
            radians,
        } => rotation(origin, axis, radians)?,
        TransformBuilder::Scaling { origin, factors } => scaling(origin, factors)?,
        TransformBuilder::Mirroring { origin, normal } => mirroring(origin, normal)?,
    };
    Validated::new(&result).map_err(|_| BuildError::ArithmeticRange)?;
    Ok(result)
}

#[cfg(test)]
mod tests;
