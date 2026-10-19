//! Row-major 3x4 affine transform application.
//!
//! This is the initial S2b interoperability primitive: it mirrors the existing
//! C++ `geomsrv::engine::Transform`, `Vec3` application rules and
//! `geomsrv::engine::ApplyTo*` ordering so later Rust stages can consume them
//! without a second set of transform semantics. It does not implement the
//! C++ builders (`Identity`, `Rotation`, `Scaling`, `Mirroring`, `Compose`),
//! face winding changes, mesh bookkeeping or any production consumer.
//!
//! Positions and translations are world-space metres in the existing
//! right-handed, Z-up frame. Directions are not positions: they ignore the
//! translation column. Normals use the cofactor inverse-transpose (multiplied by
//! the cofactor matrix, then divided by the determinant) and are never
//! normalized or reoriented. Inputs are borrowed only for the call; results are
//! newly allocated and no input array is retained, repaired or normalized.
//!
//! Arithmetic is written in the same association order as
//! `Geometry/Transforms.cpp`. Rust does not reassociate ordinary floating-point
//! expressions and no fast-math mode is enabled, so each tested finite case
//! follows the C++ formulas. Compiled C++ differential fixtures exercise the
//! bridge; no production consumer or native algorithm execution is switched.
//! The C++ implementation can produce
//! infinities for unrepresentable intermediate values; this contract refuses
//! those cases with a typed error instead of migrating that behaviour.
//!
//! Error precedence is: coordinate shape, coordinate finiteness, transform
//! finiteness (including the determinant), inverse cofactors for normals, and
//! finally each computed output. A batch therefore has no partial result: any
//! refusal discards all newly computed values.
//!
//! Orientation is the determinant sign only, with no epsilon. The caller remains
//! responsible for reversing triangle winding when
//! [`AppliedVectors::flips_orientation`] is true. No semantic identity,
//! provenance or topology carrier is read, allocated or replaced here.

use std::fmt;
use std::num::FpCategory;

/// A three-component position, direction or normal.
///
/// The type does not distinguish those roles; [`ApplyMode`] selects the
/// transform rule. Positions are world metres; directions retain their caller
/// units and normal components are dimensionless. Values are never normalized.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Vec3 {
    /// X component (metres for a position).
    pub x: f64,
    /// Y component (metres for a position).
    pub y: f64,
    /// Z component (metres for a position).
    pub z: f64,
}

/// A row-major 3x4 affine transform.
///
/// Each row is `(linear_3x3 | translation)`, matching the C++ layout. The
/// translation column is in metres. The implicit fourth row is always
/// `(0, 0, 0, 1)`, so projective transforms cannot be represented.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Transform {
    /// Three row-major rows of `(linear 3x3 | translation)` coefficients.
    pub m: [[f64; 4]; 3],
}

/// The affine rule applied to each input vector.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ApplyMode {
    /// A position: the linear part plus the translation column.
    Point,
    /// A direction: the linear part only, without translation.
    Direction,
    /// A surface normal: the cofactor inverse-transpose divided by the
    /// determinant. Not normalized.
    Normal,
}

/// Owned batch output from [`apply_vectors`].
#[derive(Clone, Debug, PartialEq)]
pub struct AppliedVectors {
    /// Flat interleaved xyz results, in the same order as the input triples.
    /// Every value is finite.
    pub values: Vec<f64>,
    /// True exactly when the transform determinant is negative, meaning the
    /// caller must reverse triangle winding to keep face orientation.
    pub flips_orientation: bool,
}

/// Typed refusals for affine application.
///
/// The variants separate malformed caller data from unrepresentable arithmetic.
/// [`Self::message`] returns a stable diagnostic prefix and description.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// The flat input length is not a multiple of three coordinates per vector.
    InputLengthNotMultipleOfThree,
    /// An input coordinate is NaN or infinite.
    NonFiniteInput,
    /// A transform coefficient, its determinant, or a required normal cofactor
    /// is NaN or infinite (including overflow).
    NonFiniteTransform,
    /// A computed result is NaN or infinite (including overflow, or division by
    /// a very small determinant).
    NonFiniteOutput,
}

impl Error {
    /// Return the stable diagnostic prefix and fixed description.
    ///
    /// No formatting or allocation is performed, so an FFI layer can copy this
    /// text directly.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::InputLengthNotMultipleOfThree => {
                "InputLengthNotMultipleOfThree: flat xyz coordinates require a length that is a multiple of three"
            }
            Self::NonFiniteInput => {
                "NonFiniteInput: every input coordinate must be finite before any vector is computed"
            }
            Self::NonFiniteTransform => {
                "NonFiniteTransform: transform coefficients, determinant and required inverse cofactors must be finite"
            }
            Self::NonFiniteOutput => {
                "NonFiniteOutput: every computed xyz result must be finite; no partial batch is returned"
            }
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.message())
    }
}

/// Apply the transform to one position.
///
/// The translation column is added after the three linear products, in the
/// written order. The transform is validated first, so a non-finite determinant
/// or coefficient is refused even when the result itself would be finite.
///
/// # Errors
///
/// [`Error::NonFiniteInput`] for a non-finite component,
/// [`Error::NonFiniteTransform`] for non-finite coefficients or determinant, or
/// [`Error::NonFiniteOutput`] when the result is unrepresentable.
pub fn apply_to_point(transform: &Transform, point: Vec3) -> Result<Vec3, Error> {
    apply_one(transform, ApplyMode::Point, point)
}

/// Apply the transform to one direction, ignoring the translation column.
///
/// # Errors
///
/// [`Error::NonFiniteInput`] for a non-finite component,
/// [`Error::NonFiniteTransform`] for non-finite coefficients or determinant, or
/// [`Error::NonFiniteOutput`] when the result is unrepresentable.
pub fn apply_to_direction(transform: &Transform, direction: Vec3) -> Result<Vec3, Error> {
    apply_one(transform, ApplyMode::Direction, direction)
}

/// Apply the transform to one normal using the cofactor inverse-transpose.
///
/// The result is scaled by the inverse determinant and is not normalized. As in
/// the documented C++ legacy path, an exactly singular linear part (determinant
/// classified as zero, including negative zero) returns the input normal
/// unchanged. This is a preserved compatibility behaviour, not a fallback
/// policy: no epsilon or alternate inverse is introduced.
///
/// # Errors
///
/// [`Error::NonFiniteInput`] for a non-finite component,
/// [`Error::NonFiniteTransform`] for non-finite coefficients, determinant or
/// inverse cofactors, or [`Error::NonFiniteOutput`] when the result is
/// unrepresentable.
pub fn apply_to_normal(transform: &Transform, normal: Vec3) -> Result<Vec3, Error> {
    apply_one(transform, ApplyMode::Normal, normal)
}

/// Apply one transform rule to a flat interleaved xyz array.
///
/// The input must contain exactly three finite scalar components per vector;
/// the empty slice is valid. Orientation is always reported, including for empty
/// input. The input is borrowed only for the call and is never modified, and no
/// view or repaired copy is retained. All twelve coefficients and the
/// determinant are validated even for point and direction modes, so an
/// unrepresentable determinant refuses the call regardless of mode.
///
/// On success the returned [`AppliedVectors::values`] is newly owned and has the
/// same length as the input, in input order. Any malformed, non-finite or
/// unrepresentable vector refuses the whole call, so no partial output is
/// observable.
///
/// Face winding is not changed. A caller that transforms mesh triangles must
/// reverse their order when [`AppliedVectors::flips_orientation`] is true. This
/// function reads and writes no semantic IDs or topology/provenance carriers.
///
/// # Errors
///
/// [`Error::InputLengthNotMultipleOfThree`] for a malformed shape,
/// [`Error::NonFiniteInput`] for a non-finite coordinate,
/// [`Error::NonFiniteTransform`] for non-finite coefficients, determinant or
/// required normal cofactors, or [`Error::NonFiniteOutput`] when any result is
/// unrepresentable.
pub fn apply_vectors(
    transform: &Transform,
    mode: ApplyMode,
    coordinates: &[f64],
) -> Result<AppliedVectors, Error> {
    let (vectors, remainder) = coordinates.as_chunks::<3>();
    if !remainder.is_empty() {
        return Err(Error::InputLengthNotMultipleOfThree);
    }
    if coordinates.iter().any(|value| !value.is_finite()) {
        return Err(Error::NonFiniteInput);
    }

    let validated = Validated::new(transform)?;
    let operator = Operator::for_mode(&validated, mode)?;
    let mut values = Vec::with_capacity(coordinates.len());
    for &[x, y, z] in vectors {
        let result = operator.apply(&validated, Vec3 { x, y, z });
        if !is_finite(result) {
            return Err(Error::NonFiniteOutput);
        }
        values.extend([result.x, result.y, result.z]);
    }

    Ok(AppliedVectors {
        values,
        flips_orientation: validated.determinant < 0.0,
    })
}

fn apply_one(transform: &Transform, mode: ApplyMode, value: Vec3) -> Result<Vec3, Error> {
    if !is_finite(value) {
        return Err(Error::NonFiniteInput);
    }

    let validated = Validated::new(transform)?;
    let operator = Operator::for_mode(&validated, mode)?;
    let result = operator.apply(&validated, value);
    if !is_finite(result) {
        return Err(Error::NonFiniteOutput);
    }
    Ok(result)
}

fn is_finite(value: Vec3) -> bool {
    value.x.is_finite() && value.y.is_finite() && value.z.is_finite()
}

struct Validated<'a> {
    matrix: &'a [[f64; 4]; 3],
    determinant: f64,
}

impl<'a> Validated<'a> {
    fn new(transform: &'a Transform) -> Result<Self, Error> {
        if transform.m.iter().flatten().any(|value| !value.is_finite()) {
            return Err(Error::NonFiniteTransform);
        }
        let determinant = determinant(&transform.m);
        if !determinant.is_finite() {
            return Err(Error::NonFiniteTransform);
        }
        Ok(Self {
            matrix: &transform.m,
            determinant,
        })
    }

    fn point(&self, value: Vec3) -> Vec3 {
        let [
            [m00, m01, m02, m03],
            [m10, m11, m12, m13],
            [m20, m21, m22, m23],
        ] = *self.matrix;
        Vec3 {
            x: m00 * value.x + m01 * value.y + m02 * value.z + m03,
            y: m10 * value.x + m11 * value.y + m12 * value.z + m13,
            z: m20 * value.x + m21 * value.y + m22 * value.z + m23,
        }
    }

    fn direction(&self, value: Vec3) -> Vec3 {
        let [[m00, m01, m02, _], [m10, m11, m12, _], [m20, m21, m22, _]] = *self.matrix;
        Vec3 {
            x: m00 * value.x + m01 * value.y + m02 * value.z,
            y: m10 * value.x + m11 * value.y + m12 * value.z,
            z: m20 * value.x + m21 * value.y + m22 * value.z,
        }
    }

    /// Return `None` for the documented exactly singular legacy path.
    fn normal_operator(&self) -> Result<Option<[f64; 9]>, Error> {
        if self.determinant.classify() == FpCategory::Zero {
            return Ok(None);
        }

        let [[m00, m01, m02, _], [m10, m11, m12, _], [m20, m21, m22, _]] = *self.matrix;
        let cofactors = [
            m11 * m22 - m12 * m21,
            m12 * m20 - m10 * m22,
            m10 * m21 - m11 * m20,
            m02 * m21 - m01 * m22,
            m00 * m22 - m02 * m20,
            m01 * m20 - m00 * m21,
            m01 * m12 - m02 * m11,
            m02 * m10 - m00 * m12,
            m00 * m11 - m01 * m10,
        ];
        if cofactors.iter().any(|value| !value.is_finite()) {
            return Err(Error::NonFiniteTransform);
        }
        Ok(Some(cofactors))
    }

    fn normal(&self, value: Vec3, cofactors: &[f64; 9]) -> Vec3 {
        let [c00, c01, c02, c10, c11, c12, c20, c21, c22] = *cofactors;
        let determinant = self.determinant;
        Vec3 {
            x: (c00 * value.x + c01 * value.y + c02 * value.z) / determinant,
            y: (c10 * value.x + c11 * value.y + c12 * value.z) / determinant,
            z: (c20 * value.x + c21 * value.y + c22 * value.z) / determinant,
        }
    }
}

fn determinant(matrix: &[[f64; 4]; 3]) -> f64 {
    let [[m00, m01, m02, _], [m10, m11, m12, _], [m20, m21, m22, _]] = *matrix;
    m00 * (m11 * m22 - m12 * m21) - m01 * (m10 * m22 - m12 * m20) + m02 * (m10 * m21 - m11 * m20)
}

enum Operator {
    Point,
    Direction,
    Normal(Option<[f64; 9]>),
}

impl Operator {
    fn for_mode(validated: &Validated<'_>, mode: ApplyMode) -> Result<Self, Error> {
        Ok(match mode {
            ApplyMode::Point => Self::Point,
            ApplyMode::Direction => Self::Direction,
            ApplyMode::Normal => Self::Normal(validated.normal_operator()?),
        })
    }

    fn apply(&self, validated: &Validated<'_>, value: Vec3) -> Vec3 {
        match self {
            Self::Point => validated.point(value),
            Self::Direction => validated.direction(value),
            Self::Normal(None) => value,
            Self::Normal(Some(cofactors)) => validated.normal(value, cofactors),
        }
    }
}

#[cfg(test)]
mod tests;
