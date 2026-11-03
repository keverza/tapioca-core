//! Ordered signed-distance fields for volumetric landscape details (#140).
//!
//! The durable operation stack is compiled to an immutable, host-free field.
//! Distances and positions are world metres in `f64`, negative is inside, and
//! material ownership is evaluated with the field. This module does not author
//! editor state, modify terrain, publish native geometry or create undo entries.
//! See `docs/guides/volumetric-detail-sdf.md` for the governing contract.

use crate::affine::Vec3;

pub mod field;
mod primitive;

pub use field::{
    BlendProfile, BooleanOperation, Capabilities, CompileOptions, Error, FieldSample, GroupHeader,
    PreparedField, StackEntry, StackOperation, ValueRange,
};
pub use primitive::{Frame, Primitive};

/// Conservative axis-aligned world-metre bounds.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Bounds {
    /// Minimum x, y and z.
    pub min: Vec3,
    /// Maximum x, y and z.
    pub max: Vec3,
}

impl Bounds {
    pub(crate) fn from_extents(center: Vec3, extents: Vec3) -> Option<Self> {
        let result = Self {
            min: Vec3 {
                x: center.x - extents.x,
                y: center.y - extents.y,
                z: center.z - extents.z,
            },
            max: Vec3 {
                x: center.x + extents.x,
                y: center.y + extents.y,
                z: center.z + extents.z,
            },
        };
        result.is_valid().then_some(result)
    }

    pub(crate) fn is_valid(self) -> bool {
        [
            self.min.x, self.min.y, self.min.z, self.max.x, self.max.y, self.max.z,
        ]
        .into_iter()
        .all(f64::is_finite)
            && self.min.x <= self.max.x
            && self.min.y <= self.max.y
            && self.min.z <= self.max.z
    }

    pub(crate) fn union(self, other: Self) -> Self {
        Self {
            min: Vec3 {
                x: self.min.x.min(other.min.x),
                y: self.min.y.min(other.min.y),
                z: self.min.z.min(other.min.z),
            },
            max: Vec3 {
                x: self.max.x.max(other.max.x),
                y: self.max.y.max(other.max.y),
                z: self.max.z.max(other.max.z),
            },
        }
    }

    pub(crate) fn inflated(self, amount: f64) -> Option<Self> {
        Self::from_extents(
            self.center(),
            Vec3 {
                x: (self.max.x - self.min.x) * 0.5 + amount,
                y: (self.max.y - self.min.y) * 0.5 + amount,
                z: (self.max.z - self.min.z) * 0.5 + amount,
            },
        )
    }

    pub(crate) fn center(self) -> Vec3 {
        Vec3 {
            x: f64::midpoint(self.min.x, self.max.x),
            y: f64::midpoint(self.min.y, self.max.y),
            z: f64::midpoint(self.min.z, self.max.z),
        }
    }

    pub(crate) fn half_diagonal(self) -> f64 {
        let center = self.center();
        let delta = primitive::sub(self.max, center);
        primitive::length(delta)
    }
}

#[cfg(test)]
mod tests;
