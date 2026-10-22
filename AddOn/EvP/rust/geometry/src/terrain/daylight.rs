//! S4.1 daylight locus: where a design edge's transition face meets the base
//! surface along one outward plan ray.
//!
//! After a level shoulder the face is affine in the outward distance `d`:
//! `z_cone(d) = z_edge + sense * g * max(0, d - shoulder)`, `sense` +1 for a cut
//! (rising outward) and -1 for a fill (falling). The base height along a straight
//! plan ray is piecewise affine with breaks exactly where the ray crosses a face
//! edge ([`Field`]), so on each piece `f(d) = z_cone(d) - z_base(d)` is affine and
//! a sign change has one closed-form root: no iteration, no bisection tolerance.
//!
//! Several roots are normal on undulating ground; every one is reported, because
//! each is a breakline of the built surface. The search continues past the limit
//! to tell a too-short limit ([`Locus::AtLimit`]) from a face that never meets
//! the ground ([`Locus::NotReached`]). Nothing is clamped to hide either.

use super::edit::Daylight;
use super::field::Field;

/// Which way the transition face runs from the edge.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Sense {
    /// Design below the ground: the face rises outward.
    Cut,
    /// Design above the ground: the face falls outward.
    Fill,
}

/// One outward ray from a design edge.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Ray {
    /// Edge plan position and design height, world metres.
    pub edge: [f64; 3],
    /// Outward plan direction; normalised here, must be non-zero.
    pub outward: [f64; 2],
    /// Level run in metres before the face starts, non-negative.
    pub shoulder: f64,
}

/// Where the transition meets the ground.
#[derive(Clone, Debug, PartialEq)]
pub enum Locus {
    /// Ascending outward distances (metres from the edge) within the limit at
    /// which the face meets the ground.
    Met {
        /// One or more roots; each is a daylight line.
        roots: Vec<f64>,
    },
    /// No root within the limit, but one beyond it.
    AtLimit {
        /// The first root beyond the limit, metres from the edge.
        distance: f64,
        /// `z_cone - z_base` at the limit, metres.
        residual: f64,
    },
    /// No root while the ray stays on the surface.
    NotReached {
        /// The limit, or where the ray leaves the surface if sooner, metres.
        at_limit: f64,
        /// `z_cone - z_base` there, metres.
        residual: f64,
    },
    /// A vertical face: the surface ends at the edge (a wall is separate).
    Vertical,
}

/// Daylight refusals.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DaylightError {
    /// A non-finite edge, a zero or non-finite direction, or a negative or
    /// non-finite shoulder or limit.
    Parameters,
    /// The face slope is not a valid grade.
    Slope,
    /// The edge itself is off the base surface.
    Coverage,
}

impl DaylightError {
    /// Static diagnostic beginning with the stable refusal code.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::Parameters => {
                "DaylightParameters: finite edge, non-zero direction and non-negative shoulder and limit required"
            }
            Self::Slope => "DaylightSlope: the transition slope is not a valid grade",
            Self::Coverage => "DaylightCoverage: the design edge is off the base surface",
        }
    }
}

/// Roots of the transition face against `field` along `ray`, up to
/// `face.extension` metres from the edge (unlimited when `None`).
///
/// # Errors
/// See [`DaylightError`].
pub fn daylight_roots(
    ray: &Ray,
    sense: Sense,
    face: Daylight,
    field: &Field,
) -> Result<Locus, DaylightError> {
    let [ex, ey, ez] = ray.edge;
    let [ux, uy] = ray.outward;
    let norm = ux.hypot(uy);
    let limit = face.extension.unwrap_or(f64::INFINITY);
    if ray.edge.iter().any(|value| !value.is_finite())
        || !(norm.is_finite() && norm > 0.0)
        || !(ray.shoulder.is_finite() && ray.shoulder >= 0.0)
        || limit.is_nan()
        || limit < 0.0
    {
        return Err(DaylightError::Parameters);
    }
    let gradient = face.slope.gradient().map_err(|_| DaylightError::Slope)?;
    if field.height_at(ex, ey).is_none() {
        return Err(DaylightError::Coverage);
    }
    if gradient.is_infinite() {
        return Ok(Locus::Vertical);
    }
    let direction = [ux / norm, uy / norm];
    let sign = match sense {
        Sense::Cut => 1.0,
        Sense::Fill => -1.0,
    };
    let residual = |d: f64| {
        field
            .height_at(ex + direction[0] * d, ey + direction[1] * d)
            .map(|sample| ez + sign * gradient * (d - ray.shoulder).max(0.0) - sample.height)
    };
    // Search to where the ray leaves the field's bounds, and past the limit.
    let bounds = field.bounds();
    let reach = (bounds.max[0] - bounds.min[0]).hypot(bounds.max[1] - bounds.min[1])
        + (ex - bounds.min[0]).abs().max((ex - bounds.max[0]).abs())
        + (ey - bounds.min[1]).abs().max((ey - bounds.max[1]).abs());
    let length = if limit.is_finite() {
        reach.max(limit)
    } else {
        reach
    };
    let mut breaks = field.ray_breaks([ex, ey], direction, length);
    breaks.extend(
        [ray.shoulder, limit]
            .into_iter()
            .filter(|t| (0.0..=length).contains(t)),
    );
    breaks.sort_by(f64::total_cmp);
    breaks.dedup();

    let mut roots = Vec::new();
    let mut previous: Option<(f64, f64)> = None;
    let mut last = (0.0, 0.0);
    for &t in &breaks {
        let Some(value) = residual(t) else {
            break; // Off the surface: the search ends at the last covered break.
        };
        match previous {
            Some((t0, f0)) if f0 * value < 0.0 => roots.push(t0 + (t - t0) * f0 / (f0 - value)),
            _ if value == 0.0 => roots.push(t),
            _ => {}
        }
        previous = Some((t, value));
        last = (t, value);
    }
    let within: Vec<f64> = roots
        .iter()
        .copied()
        .filter(|&root| root <= limit)
        .collect();
    if !within.is_empty() {
        return Ok(Locus::Met { roots: within });
    }
    let at_limit = limit.min(last.0);
    let residual = residual(at_limit).unwrap_or(last.1);
    Ok(roots
        .first()
        .map_or(Locus::NotReached { at_limit, residual }, |&distance| {
            Locus::AtLimit { distance, residual }
        }))
}

#[cfg(test)]
mod tests;
