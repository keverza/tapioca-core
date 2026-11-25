//! Plant-light receptors: a sample grid re-expressed at a height above its host.
//!
//! The sampler puts every sample a hair above its own face, along that face's
//! normal. That answers "how much sun does this surface get". A planting study
//! asks a different question: "how much sun reaches foliage `h` metres above
//! this bed". On sloping ground the two diverge, and on a steep host the normal
//! offset walks the receptor sideways, out over the neighbouring cell.
//!
//! So a receptor is a post-pass over an existing grid, never a second lattice:
//!
//! - it lifts each sample **straight up** from its host point (the grid
//!   position minus the sampler's normal offset), by the requested height, and
//!   by a small epsilon even at zero so the ray never starts on its host;
//! - with [`ReceptorNormal::Up`] it faces `+Z`, a horizontal plant-light plane;
//! - with [`ReceptorArea::Plan`] it carries the host cell's plan projection,
//!   `area * |n_z|`, so area statistics are plan m^2 (what a planting plan
//!   measures) instead of slope m^2;
//! - it keeps sample order, so face, cell column/row, group and layout all
//!   still address the host. The display is therefore a plan projection drawn
//!   on the host surface, not a surface at canopy height.
//!
//! ⚠️ AN UNAVAILABLE RECEPTOR IS NOT A DARK ONE. A host too steep to plant on,
//! or a receptor that would sit inside or above a ceiling, has no answer; it
//! must never be traced as open sky or reported as zero hours. Each sample
//! carries a [`ReceptorStatus`], and consumers exclude non-available samples
//! from hours, area denominators and classes.
//!
//! ⚠️ A FIXED LAYER DOES NOT CLAMP. The single placed study point
//! (`SunStudyLiftedPoint`) lowers itself below an overhang and reports it. A
//! layer that did the same would mix heights inside one "1.50 m" map; a blocked
//! receptor is marked [`ReceptorStatus::Blocked`] instead. The overhead distance
//! is the host's business (it owns the occluders); [`mark_overhead`] only
//! applies the same arithmetic as the point lift.

use crate::raster::Vec3;

/// The epsilon the point lift starts its overhead ray at, metres. Mirrors
/// `kTMin` in `SunStudyLiftedPoint.cpp`.
pub const OVERHEAD_START: f64 = 0.001;

/// The least lift above the host, even for a zero-height layer, metres.
/// Mirrors the point lift's `2 * kTMin`.
pub const MIN_LIFT: f64 = 2.0 * OVERHEAD_START;

/// Clearance kept below anything overhead, metres. Mirrors
/// `kLiftedPointClearance`.
pub const OVERHEAD_CLEARANCE: f64 = 0.10;

/// Highest receptor a planting layer may request, metres. A sanity bound on
/// input, not a horticultural rule.
pub const MAX_HEIGHT: f64 = 100.0;

/// Default least host-normal Z for an `Up` receptor: 0.5 is a 60 degree slope.
/// Steeper hosts (walls, soffits, anything facing down) are not planting planes.
pub const DEFAULT_MIN_HOST_UP: f64 = 0.5;

/// Which way the receptor faces.
#[repr(u8)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ReceptorNormal {
    /// Keep the host face normal.
    #[default]
    Surface = 0,
    /// `+Z`: a horizontal plant-light receptor.
    Up = 1,
}

/// What area each receptor carries.
#[repr(u8)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ReceptorArea {
    /// The host cell's surface area, as sampled.
    #[default]
    Surface = 0,
    /// The host cell's plan projection, `area * |n_z|`.
    Plan = 1,
}

/// Per-receptor validity. Discriminants are the C ABI values.
#[repr(u8)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ReceptorStatus {
    /// Traceable.
    #[default]
    Available = 0,
    /// The host normal is too steep (or faces down) for an `Up` receptor.
    SteepHost = 1,
    /// Something overhead sits below the requested height plus clearance.
    Blocked = 2,
    /// The host sample was not finite.
    InvalidHost = 3,
}

impl ReceptorStatus {
    /// Whether the receptor has an answer.
    #[must_use]
    pub fn available(self) -> bool {
        self == Self::Available
    }
}

/// Receptor settings.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ReceptorOptions {
    /// Vertical lift above the host point, metres, `0..=MAX_HEIGHT`.
    pub height: f64,
    /// Facing.
    pub normal: ReceptorNormal,
    /// Area weight.
    pub area: ReceptorArea,
    /// Least host-normal Z accepted for an `Up` receptor, `(0, 1]`.
    pub min_host_up: f64,
}

impl Default for ReceptorOptions {
    fn default() -> Self {
        Self {
            height: 0.0,
            normal: ReceptorNormal::Up,
            area: ReceptorArea::Plan,
            min_host_up: DEFAULT_MIN_HOST_UP,
        }
    }
}

/// The sampled grid a layer is built from: parallel arrays as the sampler
/// emitted them.
#[derive(Clone, Copy, Debug)]
pub struct HostSamples<'a> {
    /// xyz, lifted by `normal_offset` along `normals`.
    pub positions: &'a [f64],
    /// xyz unit host face normals.
    pub normals: &'a [f64],
    /// m^2 per sample (surface area).
    pub areas: &'a [f64],
    /// The sampler's normal offset, metres, so the host point can be recovered.
    pub normal_offset: f64,
}

/// Why a layer was refused.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ReceptorRefusal {
    /// Array lengths disagree.
    Shape,
    /// Height outside `0..=MAX_HEIGHT`, non-finite offset, or a bad tilt limit.
    Options,
}

/// One height layer, parallel to the host grid.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ReceptorLayer {
    /// Host points (sample positions with the normal offset removed).
    pub hosts: Vec<f64>,
    /// Receptor positions: host + `max(height, MIN_LIFT)` up.
    pub positions: Vec<f64>,
    /// Receptor normals.
    pub normals: Vec<f64>,
    /// Receptor area weights.
    pub areas: Vec<f64>,
    /// Per receptor.
    pub status: Vec<ReceptorStatus>,
}

impl ReceptorLayer {
    /// Receptors in the layer.
    #[must_use]
    pub fn count(&self) -> usize {
        self.status.len()
    }

    /// Summed area of available receptors and of the rest, `(available, other)`.
    #[must_use]
    pub fn area_split(&self) -> (f64, f64) {
        self.status
            .iter()
            .zip(&self.areas)
            .fold((0.0, 0.0), |(ok, other), (status, area)| {
                if status.available() {
                    (ok + area, other)
                } else {
                    (ok, other + area)
                }
            })
    }
}

fn finite3(v: &Vec3) -> bool {
    v.iter().all(|c| c.is_finite())
}

fn check(samples: &HostSamples<'_>, options: &ReceptorOptions) -> Result<usize, ReceptorRefusal> {
    let count = samples.areas.len();
    let scalars = count.checked_mul(3).ok_or(ReceptorRefusal::Shape)?;
    if samples.positions.len() != scalars || samples.normals.len() != scalars {
        return Err(ReceptorRefusal::Shape);
    }
    let height_ok = options.height.is_finite() && (0.0..=MAX_HEIGHT).contains(&options.height);
    let tilt_ok = options.min_host_up > 0.0 && options.min_host_up <= 1.0;
    if !height_ok || !tilt_ok || !samples.normal_offset.is_finite() {
        return Err(ReceptorRefusal::Options);
    }
    Ok(count)
}

/// Build one height layer from a sampled grid. Sample order, and with it every
/// face/cell/group address, is unchanged.
///
/// # Errors
/// [`ReceptorRefusal`] for mismatched arrays or out-of-range options; nothing
/// partial is returned.
pub fn build_receptor_layer(
    samples: &HostSamples<'_>,
    options: &ReceptorOptions,
) -> Result<ReceptorLayer, ReceptorRefusal> {
    let count = check(samples, options)?;
    let lift = options.height.max(MIN_LIFT);
    let mut layer = ReceptorLayer {
        hosts: Vec::with_capacity(count * 3),
        positions: Vec::with_capacity(count * 3),
        normals: Vec::with_capacity(count * 3),
        areas: Vec::with_capacity(count),
        status: Vec::with_capacity(count),
    };
    let positions = samples.positions.as_chunks::<3>().0;
    let normals = samples.normals.as_chunks::<3>().0;
    for ((p, n), area) in positions.iter().zip(normals).zip(samples.areas) {
        let host = [
            p[0] - n[0] * samples.normal_offset,
            p[1] - n[1] * samples.normal_offset,
            p[2] - n[2] * samples.normal_offset,
        ];
        let mut status = ReceptorStatus::Available;
        if !finite3(&host) || !finite3(n) || !area.is_finite() {
            status = ReceptorStatus::InvalidHost;
        } else if options.normal == ReceptorNormal::Up && n[2] < options.min_host_up {
            status = ReceptorStatus::SteepHost;
        }
        let weight = match options.area {
            ReceptorArea::Surface => *area,
            ReceptorArea::Plan => area * n[2].abs(),
        };
        let normal = match options.normal {
            ReceptorNormal::Surface => *n,
            ReceptorNormal::Up => [0.0, 0.0, 1.0],
        };
        layer.hosts.extend_from_slice(&host);
        layer
            .positions
            .extend_from_slice(&[host[0], host[1], host[2] + lift]);
        layer.normals.extend_from_slice(&normal);
        layer.areas.push(weight);
        layer.status.push(status);
    }
    Ok(layer)
}

/// Whether a ceiling `ceiling` metres above `host + OVERHEAD_START` blocks a
/// receptor at `height`: the same test `LiftSunStudyPoint` clamps on. A
/// negative or non-finite distance means nothing overhead.
#[must_use]
pub fn overhead_blocks(ceiling: f64, height: f64) -> bool {
    ceiling.is_finite()
        && ceiling >= 0.0
        && ceiling + OVERHEAD_START - OVERHEAD_CLEARANCE < height.max(MIN_LIFT)
}

/// Mark receptors blocked from below by the host-measured overhead distances
/// (one per receptor, from `host + OVERHEAD_START` straight up, the host
/// surface itself excluded). Already-unavailable receptors keep their reason.
/// Returns the number newly blocked.
///
/// # Errors
/// [`ReceptorRefusal::Shape`] when `ceilings` is not one per receptor.
pub fn mark_overhead(
    layer: &mut ReceptorLayer,
    ceilings: &[f64],
    height: f64,
) -> Result<usize, ReceptorRefusal> {
    if ceilings.len() != layer.status.len() {
        return Err(ReceptorRefusal::Shape);
    }
    let mut blocked = 0;
    for (status, ceiling) in layer.status.iter_mut().zip(ceilings) {
        if status.available() && overhead_blocks(*ceiling, height) {
            *status = ReceptorStatus::Blocked;
            blocked += 1;
        }
    }
    Ok(blocked)
}

#[cfg(test)]
mod tests;
