//! Exact metric primitives and rigid local frames for the SDF field compiler.

use super::Bounds;
use crate::affine::Vec3;

/// A rigid local frame. `axes` are the local x/y/z axes expressed in world space.
/// They must be finite, orthonormal and right-handed when the field is compiled.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Frame {
    /// World-metre local origin.
    pub origin: Vec3,
    /// Local x, y and z unit axes in world coordinates.
    pub axes: [Vec3; 3],
}

impl Frame {
    /// World frame with an origin at zero.
    pub const IDENTITY: Self = Self {
        origin: Vec3 {
            x: 0.0,
            y: 0.0,
            z: 0.0,
        },
        axes: [
            Vec3 {
                x: 1.0,
                y: 0.0,
                z: 0.0,
            },
            Vec3 {
                x: 0.0,
                y: 1.0,
                z: 0.0,
            },
            Vec3 {
                x: 0.0,
                y: 0.0,
                z: 1.0,
            },
        ],
    };

    pub(crate) fn to_local(self, point: Vec3) -> Result<Vec3, ()> {
        let delta = sub(point, self.origin);
        let [x, y, z] = self.axes;
        let local = Vec3 {
            x: dot(delta, x),
            y: dot(delta, y),
            z: dot(delta, z),
        };
        finite(local).then_some(local).ok_or(())
    }

    pub(crate) fn direction_to_world(self, direction: Vec3) -> Result<Vec3, ()> {
        let [x, y, z] = self.axes;
        let world = add(
            add(scale(x, direction.x), scale(y, direction.y)),
            scale(z, direction.z),
        );
        finite(world).then_some(world).ok_or(())
    }

    fn validate(self) -> Result<(), ()> {
        let [x, y, z] = self.axes;
        if !finite(self.origin) || ![x, y, z].into_iter().all(finite) {
            return Err(());
        }
        let values = [
            dot(x, x),
            dot(y, y),
            dot(z, z),
            dot(x, y),
            dot(x, z),
            dot(y, z),
            dot(x, cross(y, z)),
        ];
        let expected = [1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 1.0];
        if values
            .into_iter()
            .zip(expected)
            .any(|(actual, expected)| !actual.is_finite() || (actual - expected).abs() > 1e-12)
        {
            return Err(());
        }
        Ok(())
    }

    fn local_bounds(self, min: Vec3, max: Vec3) -> Option<Bounds> {
        let local_center = Vec3 {
            x: f64::midpoint(min.x, max.x),
            y: f64::midpoint(min.y, max.y),
            z: f64::midpoint(min.z, max.z),
        };
        let local_half = Vec3 {
            x: (max.x - min.x) * 0.5,
            y: (max.y - min.y) * 0.5,
            z: (max.z - min.z) * 0.5,
        };
        let center = self.direction_to_world(local_center).ok()?;
        let center = add(center, self.origin);
        let [x, y, z] = self.axes;
        let extents = Vec3 {
            x: x.x.abs() * local_half.x + y.x.abs() * local_half.y + z.x.abs() * local_half.z,
            y: x.y.abs() * local_half.x + y.y.abs() * local_half.y + z.y.abs() * local_half.z,
            z: x.z.abs() * local_half.x + y.z.abs() * local_half.y + z.z.abs() * local_half.z,
        };
        Bounds::from_extents(center, extents)
    }
}

/// One exact signed-distance primitive. All coordinates and sizes are metres.
#[derive(Clone, Debug, PartialEq)]
pub enum Primitive {
    /// A sphere centered in world space.
    Sphere {
        /// World-metre center.
        center: Vec3,
        /// Positive radius in metres.
        radius: f64,
    },
    /// A box with outer half extents and an optional round radius.
    RoundedBox {
        /// Rigid local frame.
        frame: Frame,
        /// Positive outer half extents in metres.
        half: Vec3,
        /// Edge radius in `[0, min(half)]`.
        radius: f64,
    },
    /// A finite segment with a round radius.
    Capsule {
        /// First world-metre segment endpoint.
        a: Vec3,
        /// Second world-metre segment endpoint; it must differ from `a`.
        b: Vec3,
        /// Positive radius in metres.
        radius: f64,
    },
    /// A simple closed 2D polygon extruded symmetrically along local z.
    Extrude {
        /// Rigid local frame; profile xy and extrusion z are local metres.
        frame: Frame,
        /// Simple polygon vertices in either winding, without a repeated closing vertex.
        profile: Vec<[f64; 2]>,
        /// Positive extrusion half height in metres.
        half_height: f64,
    },
}

impl Primitive {
    pub(crate) fn validate(&self) -> Result<(), ()> {
        match self {
            Self::Sphere { center, radius } => {
                if finite(*center) && positive(*radius) {
                    Ok(())
                } else {
                    Err(())
                }
            }
            Self::RoundedBox {
                frame,
                half,
                radius,
            } => {
                frame.validate()?;
                if finite(*half)
                    && positive(half.x)
                    && positive(half.y)
                    && positive(half.z)
                    && radius.is_finite()
                    && *radius >= 0.0
                    && *radius <= half.x.min(half.y).min(half.z)
                {
                    Ok(())
                } else {
                    Err(())
                }
            }
            Self::Capsule { a, b, radius } => {
                let delta = sub(*b, *a);
                if finite(*a) && finite(*b) && finite(delta) && positive(*radius) {
                    let length = dot(delta, delta);
                    if length.is_finite() && length > 0.0 {
                        return Ok(());
                    }
                }
                Err(())
            }
            Self::Extrude {
                frame,
                profile,
                half_height,
            } => {
                frame.validate()?;
                if positive(*half_height) {
                    validate_profile(profile)
                } else {
                    Err(())
                }
            }
        }
    }

    pub(crate) fn evaluate(&self, point: Vec3) -> Result<f64, ()> {
        if !finite(point) {
            return Err(());
        }
        let distance = match self {
            Self::Sphere { center, radius } => length(sub(point, *center)) - radius,
            Self::RoundedBox {
                frame,
                half,
                radius,
            } => rounded_box(frame.to_local(point)?, *half, *radius),
            Self::Capsule { a, b, radius } => {
                let closest = closest_on_segment(point, *a, *b);
                length(sub(point, closest)) - radius
            }
            Self::Extrude {
                frame,
                profile,
                half_height,
            } => {
                let local = frame.to_local(point)?;
                let query = profile_query(profile, [local.x, local.y])?;
                let dz = local.z.abs() - half_height;
                combine_extrusion(query.distance, dz)
            }
        };
        distance.is_finite().then_some(distance).ok_or(())
    }

    pub(crate) fn bounds(&self) -> Option<Bounds> {
        match self {
            Self::Sphere { center, radius } => Bounds::from_extents(
                *center,
                Vec3 {
                    x: *radius,
                    y: *radius,
                    z: *radius,
                },
            ),
            Self::RoundedBox { frame, half, .. } => frame.local_bounds(
                Vec3 {
                    x: -half.x,
                    y: -half.y,
                    z: -half.z,
                },
                *half,
            ),
            Self::Capsule { a, b, radius } => {
                let min = Vec3 {
                    x: a.x.min(b.x) - radius,
                    y: a.y.min(b.y) - radius,
                    z: a.z.min(b.z) - radius,
                };
                let max = Vec3 {
                    x: a.x.max(b.x) + radius,
                    y: a.y.max(b.y) + radius,
                    z: a.z.max(b.z) + radius,
                };
                Bounds::from_extents(
                    Vec3 {
                        x: f64::midpoint(min.x, max.x),
                        y: f64::midpoint(min.y, max.y),
                        z: f64::midpoint(min.z, max.z),
                    },
                    Vec3 {
                        x: (max.x - min.x) * 0.5,
                        y: (max.y - min.y) * 0.5,
                        z: (max.z - min.z) * 0.5,
                    },
                )
            }
            Self::Extrude {
                frame,
                profile,
                half_height,
            } => {
                let (min, max) = profile_bounds(profile)?;
                frame.local_bounds(
                    Vec3 {
                        x: min[0],
                        y: min[1],
                        z: -half_height,
                    },
                    Vec3 {
                        x: max[0],
                        y: max[1],
                        z: *half_height,
                    },
                )
            }
        }
    }

    /// Analytic gradient, or `None` at a non-differentiable/degenerate point.
    pub(crate) fn gradient(&self, point: Vec3) -> Result<Option<Vec3>, ()> {
        let gradient = match self {
            Self::Sphere { center, .. } => normalized_or_none(sub(point, *center)),
            Self::RoundedBox {
                frame,
                half,
                radius,
            } => {
                let local = frame.to_local(point)?;
                let Some(gradient) = rounded_box_gradient(local, *half, *radius) else {
                    return Ok(None);
                };
                Some(frame.direction_to_world(gradient)?)
            }
            Self::Capsule { a, b, .. } => {
                let closest = closest_on_segment(point, *a, *b);
                normalized_or_none(sub(point, closest))
            }
            Self::Extrude {
                frame,
                profile,
                half_height,
            } => {
                let local = frame.to_local(point)?;
                let query = profile_query(profile, [local.x, local.y])?;
                let dz = local.z.abs() - half_height;
                let Some(gradient) = extrusion_gradient(&query, local.z, dz) else {
                    return Ok(None);
                };
                Some(frame.direction_to_world(gradient)?)
            }
        };
        match gradient {
            Some(value) if finite(value) => Ok(Some(value)),
            Some(_) => Err(()),
            None => Ok(None),
        }
    }
}

#[derive(Clone, Copy)]
struct ProfileQuery {
    distance: f64,
    gradient: Option<[f64; 2]>,
}

fn closest_on_segment(point: Vec3, a: Vec3, b: Vec3) -> Vec3 {
    let delta = sub(b, a);
    let amount = (dot(sub(point, a), delta) / dot(delta, delta)).clamp(0.0, 1.0);
    add(a, scale(delta, amount))
}

fn rounded_box(point: Vec3, half: Vec3, radius: f64) -> f64 {
    let q = Vec3 {
        x: point.x.abs() - (half.x - radius),
        y: point.y.abs() - (half.y - radius),
        z: point.z.abs() - (half.z - radius),
    };
    let outside = Vec3 {
        x: q.x.max(0.0),
        y: q.y.max(0.0),
        z: q.z.max(0.0),
    };
    length(outside) + q.x.max(q.y).max(q.z).min(0.0) - radius
}

#[expect(
    clippy::float_cmp,
    reason = "exact equality identifies non-differentiable box-edge tie cases"
)]
fn rounded_box_gradient(point: Vec3, half: Vec3, radius: f64) -> Option<Vec3> {
    let q = Vec3 {
        x: point.x.abs() - (half.x - radius),
        y: point.y.abs() - (half.y - radius),
        z: point.z.abs() - (half.z - radius),
    };
    let outside = Vec3 {
        x: q.x.max(0.0),
        y: q.y.max(0.0),
        z: q.z.max(0.0),
    };
    let outside_length = length(outside);
    if outside_length > 0.0 {
        return Some(Vec3 {
            x: outside.x / outside_length * point.x.signum(),
            y: outside.y / outside_length * point.y.signum(),
            z: outside.z / outside_length * point.z.signum(),
        });
    }
    let maximum = q.x.max(q.y).max(q.z);
    if maximum < 0.0 {
        let direction = if q.x == maximum {
            Vec3 {
                x: point.x.signum(),
                y: 0.0,
                z: 0.0,
            }
        } else if q.y == maximum {
            Vec3 {
                x: 0.0,
                y: point.y.signum(),
                z: 0.0,
            }
        } else {
            Vec3 {
                x: 0.0,
                y: 0.0,
                z: point.z.signum(),
            }
        };
        return Some(direction);
    }
    None
}

fn combine_extrusion(d2: f64, dz: f64) -> f64 {
    let x = d2.max(0.0);
    let z = dz.max(0.0);
    d2.max(dz).min(0.0) + (x * x + z * z).sqrt()
}

fn extrusion_gradient(query: &ProfileQuery, z: f64, dz: f64) -> Option<Vec3> {
    let outside_profile = query.distance.max(0.0);
    let outside_z = dz.max(0.0);
    let outside = (outside_profile * outside_profile + outside_z * outside_z).sqrt();
    if outside > 0.0 {
        let profile_weight = outside_profile / outside;
        let [gx, gy] = query.gradient?;
        return Some(Vec3 {
            x: gx * profile_weight,
            y: gy * profile_weight,
            z: z.signum() * outside_z / outside,
        });
    }
    if query.distance > dz {
        let [gx, gy] = query.gradient?;
        return Some(Vec3 {
            x: gx,
            y: gy,
            z: 0.0,
        });
    }
    if dz > query.distance {
        return Some(Vec3 {
            x: 0.0,
            y: 0.0,
            z: z.signum(),
        });
    }
    None
}

#[expect(
    clippy::float_cmp,
    reason = "duplicate profile vertices are an exact authored-data defect, not a tolerance query"
)]
fn validate_profile(profile: &[[f64; 2]]) -> Result<(), ()> {
    if profile.len() < 3 || profile.iter().flatten().any(|v| !v.is_finite()) {
        return Err(());
    }
    let mut twice_area = 0.0;
    for (&a, &b) in edges(profile) {
        if a == b {
            return Err(());
        }
        twice_area += a[0] * b[1] - a[1] * b[0];
    }
    if !twice_area.is_finite() || twice_area == 0.0 {
        return Err(());
    }
    for (index, (&a, &b)) in edges(profile).enumerate() {
        for (other, (&c, &d)) in edges(profile).enumerate().skip(index + 1) {
            let adjacent =
                other == index || other == index + 1 || (index == 0 && other + 1 == profile.len());
            if !adjacent && segments_intersect(a, b, c, d) {
                return Err(());
            }
        }
    }
    Ok(())
}

fn segments_intersect(a: [f64; 2], b: [f64; 2], c: [f64; 2], d: [f64; 2]) -> bool {
    let ab_c = cross2(sub2(b, a), sub2(c, a));
    let ab_d = cross2(sub2(b, a), sub2(d, a));
    let cd_a = cross2(sub2(d, c), sub2(a, c));
    let cd_b = cross2(sub2(d, c), sub2(b, c));
    if !ab_c.is_finite() || !ab_d.is_finite() || !cd_a.is_finite() || !cd_b.is_finite() {
        return true;
    }
    if ((ab_c > 0.0 && ab_d < 0.0) || (ab_c < 0.0 && ab_d > 0.0))
        && ((cd_a > 0.0 && cd_b < 0.0) || (cd_a < 0.0 && cd_b > 0.0))
    {
        return true;
    }
    (ab_c == 0.0 && on_segment(a, b, c))
        || (ab_d == 0.0 && on_segment(a, b, d))
        || (cd_a == 0.0 && on_segment(c, d, a))
        || (cd_b == 0.0 && on_segment(c, d, b))
}

fn on_segment(a: [f64; 2], b: [f64; 2], point: [f64; 2]) -> bool {
    point[0] >= a[0].min(b[0])
        && point[0] <= a[0].max(b[0])
        && point[1] >= a[1].min(b[1])
        && point[1] <= a[1].max(b[1])
}

fn profile_query(profile: &[[f64; 2]], point: [f64; 2]) -> Result<ProfileQuery, ()> {
    let mut best_squared = f64::INFINITY;
    let mut nearest = [0.0; 2];
    let mut inside = false;
    for (&a, &b) in edges(profile) {
        let (squared, closest) = segment_query(a, b, point);
        if !squared.is_finite() {
            return Err(());
        }
        if squared < best_squared {
            best_squared = squared;
            nearest = closest;
        }
        if (a[1] > point[1]) != (b[1] > point[1]) {
            let crossing = a[0] + (point[1] - a[1]) * (b[0] - a[0]) / (b[1] - a[1]);
            if !crossing.is_finite() {
                return Err(());
            }
            if crossing > point[0] {
                inside = !inside;
            }
        }
    }
    let distance = best_squared.sqrt();
    let sign = if inside { -1.0 } else { 1.0 };
    let gradient = if distance > 0.0 {
        Some([
            sign * (point[0] - nearest[0]) / distance,
            sign * (point[1] - nearest[1]) / distance,
        ])
    } else {
        None
    };
    let signed = sign * distance;
    if signed.is_finite() {
        Ok(ProfileQuery {
            distance: signed,
            gradient,
        })
    } else {
        Err(())
    }
}

fn segment_query(a: [f64; 2], b: [f64; 2], point: [f64; 2]) -> (f64, [f64; 2]) {
    let delta = sub2(b, a);
    let from_a = sub2(point, a);
    let denominator = dot2(delta, delta);
    let t = (dot2(from_a, delta) / denominator).clamp(0.0, 1.0);
    let closest = [a[0] + delta[0] * t, a[1] + delta[1] * t];
    let offset = sub2(point, closest);
    (dot2(offset, offset), closest)
}

fn profile_bounds(profile: &[[f64; 2]]) -> Option<([f64; 2], [f64; 2])> {
    let first = *profile.first()?;
    let (min, max) = profile
        .iter()
        .copied()
        .fold((first, first), |(min, max), point| {
            (
                [min[0].min(point[0]), min[1].min(point[1])],
                [max[0].max(point[0]), max[1].max(point[1])],
            )
        });
    Some((min, max))
}

fn edges(profile: &[[f64; 2]]) -> impl Iterator<Item = (&[f64; 2], &[f64; 2])> {
    profile
        .iter()
        .zip(profile.iter().cycle().skip(1))
        .take(profile.len())
}

fn positive(value: f64) -> bool {
    value.is_finite() && value > 0.0
}

pub(crate) fn finite(value: Vec3) -> bool {
    [value.x, value.y, value.z].into_iter().all(f64::is_finite)
}

pub(crate) fn add(a: Vec3, b: Vec3) -> Vec3 {
    Vec3 {
        x: a.x + b.x,
        y: a.y + b.y,
        z: a.z + b.z,
    }
}

pub(crate) fn sub(a: Vec3, b: Vec3) -> Vec3 {
    Vec3 {
        x: a.x - b.x,
        y: a.y - b.y,
        z: a.z - b.z,
    }
}

pub(crate) fn negate(value: Vec3) -> Vec3 {
    Vec3 {
        x: -value.x,
        y: -value.y,
        z: -value.z,
    }
}

fn scale(value: Vec3, factor: f64) -> Vec3 {
    Vec3 {
        x: value.x * factor,
        y: value.y * factor,
        z: value.z * factor,
    }
}

fn dot(a: Vec3, b: Vec3) -> f64 {
    a.x * b.x + a.y * b.y + a.z * b.z
}

fn cross(a: Vec3, b: Vec3) -> Vec3 {
    Vec3 {
        x: a.y * b.z - a.z * b.y,
        y: a.z * b.x - a.x * b.z,
        z: a.x * b.y - a.y * b.x,
    }
}

pub(crate) fn length(value: Vec3) -> f64 {
    dot(value, value).sqrt()
}

fn normalized_or_none(value: Vec3) -> Option<Vec3> {
    let magnitude = length(value);
    if magnitude.is_finite() && magnitude > 0.0 {
        Some(scale(value, 1.0 / magnitude))
    } else {
        None
    }
}

fn sub2(a: [f64; 2], b: [f64; 2]) -> [f64; 2] {
    [a[0] - b[0], a[1] - b[1]]
}

fn dot2(a: [f64; 2], b: [f64; 2]) -> f64 {
    a[0] * b[0] + a[1] * b[1]
}

fn cross2(a: [f64; 2], b: [f64; 2]) -> f64 {
    a[0] * b[1] - a[1] * b[0]
}
