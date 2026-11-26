//! The analytical sky contract (#269): named, versioned sky distributions
//! over a fixed patch dome, normalised to a stated horizontal quantity.
//!
//! - [`dome`]: the Tregenza 145 and Reinhart 577 patch sets, their order,
//!   solid angles and exact moments.
//! - [`cie`]: CIE general skies 1–15 and the traditional standard overcast.
//! - [`perez`]: the Perez 1993 all-weather sky, the 1990 efficacies and the
//!   1990 transposition (the cross-check, not the production path).
//!
//! A sky here is a **diffuse** field: per patch, a radiance (W/m²/sr) or a
//! luminance (cd/m²) such that `Σ_p L_p·(moment_p)_z` equals the horizontal
//! diffuse quantity it was normalised to. The sun's beam is never folded into
//! a patch; direct is DNI along the sun's own direction, traced separately.
//!
//! ⚠️ NEGATIVE RADIANCE IS CLAMPED, NOT HIDDEN. The Perez distribution can go
//! below zero near the horizon. Such patches are set to zero, the rest are
//! rescaled so the horizontal integral is still conserved, and the share
//! removed is returned in [`SkyField::clamped_share`].
//!
//! Like the rest of this crate, no solar position is computed here: the caller
//! passes the sun direction the host gave.

pub mod cie;
pub mod dome;
pub mod perez;

pub use dome::{Cell, Dome, Patch, PatchSet};

/// Version of the sky registry: bumped when a distribution, a coefficient, a
/// patch order or a normalisation rule changes, so a stored result can tell.
pub const REGISTRY_VERSION: u32 = 1;

/// A named sky distribution.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum SkyModel {
    /// CIE general sky type 1–15 (ISO 15469:2004 / CIE S 011/E:2003).
    CieGeneral(u8),
    /// Traditional CIE standard overcast, (1 + 2 sin α)/3 (Moon & Spencer).
    CieTraditionalOvercast,
    /// Perez 1993 all-weather, from DNI and DHI.
    Perez1993,
}

impl SkyModel {
    /// Stable identifier used in a result's sky identity, e.g. `cie-s011-01`.
    #[must_use]
    pub fn id(self) -> String {
        match self {
            Self::CieGeneral(kind) => format!("cie-s011-{kind:02}"),
            Self::CieTraditionalOvercast => "cie-overcast-1955".into(),
            Self::Perez1993 => "perez-1993-all-weather".into(),
        }
    }
}

/// What a result says about its sky (#269 §5).
#[derive(Clone, Debug, PartialEq)]
pub struct SkyIdentity {
    /// [`SkyModel::id`].
    pub model: String,
    /// [`REGISTRY_VERSION`].
    pub version: u32,
    /// [`PatchSet::name`].
    pub patch_set: &'static str,
    /// The quantity the field was normalised to, e.g. `DHI W/m2`.
    pub normalised_to: &'static str,
}

impl SkyIdentity {
    /// The identity of `model` on `set`, normalised to `normalised_to`.
    #[must_use]
    pub fn new(model: SkyModel, set: PatchSet, normalised_to: &'static str) -> Self {
        Self {
            model: model.id(),
            version: REGISTRY_VERSION,
            patch_set: set.name(),
            normalised_to,
        }
    }
}

/// A diffuse sky over a dome.
#[derive(Clone, Debug, PartialEq)]
pub struct SkyField {
    /// Per patch, in the dome's order: radiance or luminance, never negative.
    pub values: Vec<f64>,
    /// The share of the unclamped horizontal integral that came from negative
    /// patches and was removed before rescaling, 0–1.
    pub clamped_share: f64,
}

/// Why a field could not be built.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FieldRefusal {
    /// `relative` does not have one value per patch.
    Length,
    /// The distribution integrates to nothing above the horizon, or a value is
    /// not finite.
    Degenerate,
}

/// Scale `relative` (one value per patch, any sign) so its horizontal integral
/// equals `horizontal`. Negative values are clamped first; their share is
/// reported. Sums run in patch order.
///
/// # Errors
/// A length mismatch, a non-finite value, or no positive integral.
pub fn normalise(dome: &Dome, relative: &[f64], horizontal: f64) -> Result<SkyField, FieldRefusal> {
    if relative.len() != dome.len() {
        return Err(FieldRefusal::Length);
    }
    let mut positive = 0.0;
    let mut negative = 0.0;
    for (p, &v) in dome.patches().iter().zip(relative) {
        if !v.is_finite() {
            return Err(FieldRefusal::Degenerate);
        }
        let weight = v * p.moment[2];
        if weight >= 0.0 {
            positive += weight;
        } else {
            negative -= weight;
        }
    }
    if positive.is_nan() || positive <= 0.0 || !horizontal.is_finite() {
        return Err(FieldRefusal::Degenerate);
    }
    let scale = horizontal / positive;
    Ok(SkyField {
        values: relative.iter().map(|&v| v.max(0.0) * scale).collect(),
        clamped_share: negative / (positive + negative),
    })
}

/// The relative distribution of a sun-independent or sun-dependent CIE sky at
/// each patch centroid. `sun` is a unit sky-frame vector (ignored by the
/// traditional overcast). `None` for an unknown CIE type.
#[must_use]
pub fn cie_relative(dome: &Dome, model: SkyModel, sun: [f64; 3]) -> Option<Vec<f64>> {
    dome.patches()
        .iter()
        .map(|p| match model {
            SkyModel::CieGeneral(kind) => cie::relative(kind, p.direction, sun),
            SkyModel::CieTraditionalOvercast => cie::traditional_overcast(p.direction),
            SkyModel::Perez1993 => None,
        })
        .collect()
}

/// One interval's Perez sky as radiance, W/m²/sr, conserving DHI.
#[derive(Clone, Debug, PartialEq)]
pub struct PerezSky {
    /// The sky condition and the holds applied.
    pub parameters: perez::PerezParameters,
    /// The luminance coefficients (a, b, c, d, e).
    pub coefficients: [f64; 5],
    /// Radiance per patch; `Σ L_p·(moment_p)_z` = DHI.
    pub field: SkyField,
}

/// Why an interval has no Perez sky.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PerezSkyRefusal {
    /// The input, as [`perez::parameters`] refuses it.
    Input(perez::PerezRefusal),
    /// The distribution could not be normalised.
    Field(FieldRefusal),
}

/// The Perez 1993 radiance field for one interval, normalised to its DHI.
///
/// # Errors
/// A sun at or below the horizon, unusable irradiance, or a degenerate field.
pub fn perez_sky(dome: &Dome, input: &perez::PerezInput) -> Result<PerezSky, PerezSkyRefusal> {
    let mut parameters = perez::parameters(input).map_err(PerezSkyRefusal::Input)?;
    let (coefficients, raised) = perez::coefficients(&parameters);
    parameters.held.luminance_brightness = raised;
    // Evaluate against the sun the coefficients assumed (altitude ≤ 87°).
    let sun = held_sun(input.sun, parameters.zenith);
    let relative: Vec<f64> = dome
        .patches()
        .iter()
        .map(|p| perez::relative(&coefficients, p.direction, sun))
        .collect();
    let field = normalise(dome, &relative, input.dhi).map_err(PerezSkyRefusal::Field)?;
    Ok(PerezSky {
        parameters,
        coefficients,
        field,
    })
}

/// `sun` moved to zenith angle `zenith` along its own azimuth (unchanged when
/// it is already there).
fn held_sun(sun: [f64; 3], zenith: f64) -> [f64; 3] {
    let horizontal = sun[0].hypot(sun[1]);
    if (sun[2].min(1.0).acos() - zenith).abs() < 1e-12 {
        return sun;
    }
    let (s, c) = zenith.sin_cos();
    if horizontal > 0.0 {
        [sun[0] / horizontal * s, sun[1] / horizontal * s, c]
    } else {
        [0.0, s, c] // straight overhead: any azimuth; take north
    }
}

#[cfg(test)]
mod radiance_tests;
#[cfg(test)]
mod tests;
