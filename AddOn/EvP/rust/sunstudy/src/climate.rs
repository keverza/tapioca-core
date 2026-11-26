//! Climate-based irradiation per sample (#155 F3) and the daily light
//! integral derived from it (#278 Stage 5).
//!
//! One pass over a weather series, interval by interval, in the order given:
//!
//! - **Direct**: DNI·max(0, n·s)·beam·Δt per sample and interval, with the
//!   beam's path traced by the caller for that interval's sun.
//! - **Diffuse**: the per-interval sky field (Perez 1993, or isotropic) is
//!   summed per month into one radiance-hours value per patch. A sample's
//!   diffuse is then `Σ_p projected_p·visible_p·F_p`: the sky is integrated once,
//!   the mask once per sample, never per interval (the mask × sky pattern).
//! - **Reflected**: the model's own horizontal total (DNI·sin α + DHI) times
//!   the ground albedo times the plane's isotropic ground view `(1 − n_z)/2`.
//!   The ground is taken as unshaded; that is stated, not hidden.
//!
//! A sample's path to the sky or the sun is **open**, through **canopy**, or
//! **blocked**. Canopy passes the month's transmittance (seasonal leaf cover);
//! everything else is opaque, as in the geometric study.
//!
//! ⚠️ ENERGY, NOT POWER. Every output is Wh/m² over the intervals presented;
//! an interval's energy is its mean W/m² times its hours.
//!
//! ⚠️ MISSING IS NOT ZERO. An interval with missing radiation adds nothing
//! and is counted per month in [`ClimateReport::missing_hours`]; a month's DLI
//! divides by the hours that were present, and its coverage is reported.
//!
//! Climatic outputs only: the geometric sky view factor lives on
//! [`SkyMasks::view_factor`] and is never mixed into these totals.

use crate::sky::perez::PerezInput;
use crate::sky::{Dome, PerezSkyRefusal, perez_sky};

/// Ground albedo when the weather gives none in (0, 1].
pub const DEFAULT_ALBEDO: f64 = 0.20;

/// A sample's path towards one sky patch or the sun.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Path {
    /// Something opaque is in the way.
    Blocked = 0,
    /// Only canopy is in the way.
    Canopy = 1,
    /// Nothing is in the way.
    Open = 2,
}

impl Path {
    /// From the wire value; anything unknown is blocked.
    #[must_use]
    pub fn from_u8(v: u8) -> Self {
        match v {
            2 => Self::Open,
            1 => Self::Canopy,
            _ => Self::Blocked,
        }
    }
}

/// Per sample and patch, the path to the sky: two bit sets, packed.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SkyMasks {
    patches: usize,
    words: usize,
    open: Vec<u64>,
    canopy: Vec<u64>,
}

impl SkyMasks {
    /// All paths blocked, for `samples` × `patches`.
    #[must_use]
    pub fn new(samples: usize, patches: usize) -> Self {
        let words = patches.div_ceil(64);
        Self {
            patches,
            words,
            open: vec![0; samples * words],
            canopy: vec![0; samples * words],
        }
    }

    /// Set one sample's paths from one wire value per patch.
    ///
    /// # Errors
    /// The sample is out of range or `paths` is not one per patch.
    pub fn set(&mut self, sample: usize, paths: &[u8]) -> Result<(), ClimateRefusal> {
        if paths.len() != self.patches {
            return Err(ClimateRefusal::Length);
        }
        let base = sample * self.words;
        let open = self
            .open
            .get_mut(base..base + self.words)
            .ok_or(ClimateRefusal::Length)?;
        open.fill(0);
        for (p, &v) in paths.iter().enumerate() {
            if Path::from_u8(v) == Path::Open
                && let Some(w) = open.get_mut(p / 64)
            {
                *w |= 1 << (p % 64);
            }
        }
        let canopy = self
            .canopy
            .get_mut(base..base + self.words)
            .ok_or(ClimateRefusal::Length)?;
        canopy.fill(0);
        for (p, &v) in paths.iter().enumerate() {
            if Path::from_u8(v) == Path::Canopy
                && let Some(w) = canopy.get_mut(p / 64)
            {
                *w |= 1 << (p % 64);
            }
        }
        Ok(())
    }

    /// One sample's path to one patch.
    #[must_use]
    pub fn path(&self, sample: usize, patch: usize) -> Path {
        let at = sample * self.words + patch / 64;
        let bit = 1_u64 << (patch % 64);
        if self.open.get(at).is_some_and(|w| w & bit != 0) {
            Path::Open
        } else if self.canopy.get(at).is_some_and(|w| w & bit != 0) {
            Path::Canopy
        } else {
            Path::Blocked
        }
    }

    /// Geometric sky view factor of `sample` with unit `normal`: open patches
    /// count 1, canopy `canopy_transmittance`, in patch order. Weather-free.
    #[must_use]
    pub fn view_factor(
        &self,
        dome: &Dome,
        sample: usize,
        normal: [f64; 3],
        canopy_transmittance: f64,
    ) -> f64 {
        let mut sum = 0.0;
        for (p, patch) in dome.patches().iter().enumerate() {
            let pass = match self.path(sample, p) {
                Path::Open => 1.0,
                Path::Canopy => canopy_transmittance,
                Path::Blocked => 0.0,
            };
            if pass > 0.0 {
                sum += pass * patch.projected(normal);
            }
        }
        sum / std::f64::consts::PI
    }
}

/// The diffuse sky distribution used per interval.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DiffuseSky {
    /// Perez 1993 all-weather; an interval the model refuses (sun below the
    /// horizon at the interval's midpoint) falls back to isotropic, counted.
    Perez1993,
    /// Uniform radiance: the cross-check, and the only choice without DNI.
    Isotropic,
}

/// How the run is set up.
#[derive(Clone, Debug, PartialEq)]
pub struct ClimateSetup {
    /// Patch dome in the model frame (see [`Dome::rotated`]).
    pub dome: Dome,
    /// Unit normal per sample, model frame, z up.
    pub normals: Vec<[f64; 3]>,
    /// Per sample and patch, the path to the sky.
    pub masks: SkyMasks,
    /// Diffuse distribution.
    pub sky: DiffuseSky,
    /// Canopy transmittance per month (January first), 0–1.
    pub canopy_transmittance: [f64; 12],
}

/// One weather interval, already placed on the project's clock.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ClimateInterval {
    /// Month, 0 = January.
    pub month: u8,
    /// Interval length, hours.
    pub hours: f64,
    /// Day of the year, 1–366.
    pub day_of_year: u32,
    /// Unit vector to the sun at the interval's midpoint, model frame.
    pub sun: [f64; 3],
    /// Mean direct normal irradiance, W/m²; NaN when missing.
    pub dni: f64,
    /// Mean diffuse horizontal irradiance, W/m²; NaN when missing.
    pub dhi: f64,
    /// Ground albedo; outside (0, 1] uses [`DEFAULT_ALBEDO`].
    pub albedo: f64,
    /// Dew point, °C; NaN when missing.
    pub dew_point_c: f64,
}

impl ClimateInterval {
    /// Whether the beam needs tracing: sun up and DNI above zero.
    #[must_use]
    pub fn needs_beam(&self) -> bool {
        self.sun[2] > 0.0 && self.dni > 0.0
    }
}

/// Why an interval or a setup is refused.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ClimateRefusal {
    /// Masks, normals or the beam paths do not match the sample or patch count.
    Length,
    /// The month is outside 0–11 or the hours are not positive.
    Interval,
    /// The beam's paths were needed (sun up, DNI > 0) and not given.
    Beam,
}

/// What the run did, per month where it matters.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ClimateReport {
    /// Hours with usable radiation (night hours included).
    pub valid_hours: [f64; 12],
    /// Hours with missing DNI or DHI: added nothing.
    pub missing_hours: [f64; 12],
    /// Intervals whose diffuse sky fell back to isotropic.
    pub isotropic_intervals: u32,
    /// DNI energy, Wh/m², in intervals whose midpoint sun was down (lost).
    pub beam_below_horizon: f64,
    /// The model's horizontal total Σ(DNI·sin α + DHI)·Δt per month, Wh/m².
    pub open_horizontal: [f64; 12],
    /// Largest negative-radiance share clamped in one interval.
    pub max_clamped_share: f64,
    /// Intervals where the Perez parameters were held to their ranges.
    pub held_intervals: u32,
}

/// Per sample, per month, Wh/m².
#[derive(Clone, Debug, PartialEq)]
pub struct ClimateTotals {
    /// Beam.
    pub direct: Vec<[f64; 12]>,
    /// Sky.
    pub diffuse: Vec<[f64; 12]>,
    /// Ground-reflected.
    pub reflected: Vec<[f64; 12]>,
    /// The run's report.
    pub report: ClimateReport,
}

impl ClimateTotals {
    /// `direct + diffuse + reflected` for one sample and month.
    #[must_use]
    pub fn total(&self, sample: usize, month: usize) -> f64 {
        let get = |v: &Vec<[f64; 12]>| {
            v.get(sample)
                .and_then(|m| m.get(month))
                .copied()
                .unwrap_or(0.0)
        };
        get(&self.direct) + get(&self.diffuse) + get(&self.reflected)
    }
}

/// The accumulator.
#[derive(Clone, Debug)]
pub struct Climate {
    setup: ClimateSetup,
    direct: Vec<[f64; 12]>,
    /// Per month, per patch: radiance-hours, Wh/m²/sr.
    field: Vec<Vec<f64>>,
    /// Per month: Σ ρ·(DNI·sin α + DHI)·Δt, Wh/m².
    ground: [f64; 12],
    report: ClimateReport,
}

impl Climate {
    /// Start a run.
    ///
    /// # Errors
    /// Masks or normals that do not match each other or the dome.
    pub fn new(setup: ClimateSetup) -> Result<Self, ClimateRefusal> {
        let samples = setup.normals.len();
        if setup.masks.patches != setup.dome.len()
            || setup.masks.open.len() != samples * setup.masks.words
        {
            return Err(ClimateRefusal::Length);
        }
        let patches = setup.dome.len();
        Ok(Self {
            direct: vec![[0.0; 12]; samples],
            field: vec![vec![0.0; patches]; 12],
            ground: [0.0; 12],
            report: ClimateReport::default(),
            setup,
        })
    }

    /// Add one interval. `beam` holds one [`Path`] wire value per sample and
    /// is required when [`ClimateInterval::needs_beam`].
    ///
    /// # Errors
    /// A bad month or length, or a missing beam.
    pub fn add(
        &mut self,
        interval: &ClimateInterval,
        beam: Option<&[u8]>,
    ) -> Result<(), ClimateRefusal> {
        let month = usize::from(interval.month);
        if month >= 12 || interval.hours.is_nan() || interval.hours <= 0.0 {
            return Err(ClimateRefusal::Interval);
        }
        let (dni, dhi) = (interval.dni, interval.dhi);
        if !dni.is_finite() || !dhi.is_finite() {
            add_to(&mut self.report.missing_hours, month, interval.hours);
            return Ok(());
        }
        let (dni, dhi) = (dni.max(0.0), dhi.max(0.0));
        add_to(&mut self.report.valid_hours, month, interval.hours);
        let sin_alt = interval.sun[2];
        if sin_alt <= 0.0 {
            self.report.beam_below_horizon += dni * interval.hours;
        }
        let beam_horizontal = if sin_alt > 0.0 { dni * sin_alt } else { 0.0 };
        let horizontal = (beam_horizontal + dhi) * interval.hours;
        add_to(&mut self.report.open_horizontal, month, horizontal);
        let albedo = if interval.albedo > 0.0 && interval.albedo <= 1.0 {
            interval.albedo
        } else {
            DEFAULT_ALBEDO
        };
        add_to(&mut self.ground, month, albedo * horizontal);
        if dhi > 0.0 {
            self.add_sky(interval, month, dni, dhi);
        }
        if interval.needs_beam() {
            let beam = beam.ok_or(ClimateRefusal::Beam)?;
            if beam.len() != self.direct.len() {
                return Err(ClimateRefusal::Length);
            }
            let tau = self
                .setup
                .canopy_transmittance
                .get(month)
                .copied()
                .unwrap_or(0.0);
            let energy = dni * interval.hours;
            for ((sample, normal), &path) in
                self.direct.iter_mut().zip(&self.setup.normals).zip(beam)
            {
                let pass = match Path::from_u8(path) {
                    Path::Open => 1.0,
                    Path::Canopy => tau,
                    Path::Blocked => continue,
                };
                let cos = crate::sky::dome::dot(*normal, interval.sun);
                if cos > 0.0 {
                    add_to(sample, month, energy * cos * pass);
                }
            }
        }
        Ok(())
    }

    /// Add one interval's diffuse field to its month.
    fn add_sky(&mut self, interval: &ClimateInterval, month: usize, dni: f64, dhi: f64) {
        let dome = &self.setup.dome;
        let perez = match self.setup.sky {
            DiffuseSky::Perez1993 => Some(perez_sky(
                dome,
                &PerezInput {
                    sun: interval.sun,
                    dni,
                    dhi,
                    day_of_year: interval.day_of_year,
                    dew_point_c: interval
                        .dew_point_c
                        .is_finite()
                        .then_some(interval.dew_point_c),
                },
            )),
            DiffuseSky::Isotropic => None,
        };
        let fallback = matches!(perez, Some(Err(_)));
        let Some(field) = self.field.get_mut(month) else {
            return;
        };
        match perez {
            Some(Ok(sky)) => {
                let held = sky.parameters.held;
                if held.altitude || held.clearness || held.brightness || held.luminance_brightness {
                    self.report.held_intervals += 1;
                }
                self.report.max_clamped_share =
                    self.report.max_clamped_share.max(sky.field.clamped_share);
                for (f, l) in field.iter_mut().zip(&sky.field.values) {
                    *f += l * interval.hours;
                }
            }
            Some(Err(PerezSkyRefusal::Input(_) | PerezSkyRefusal::Field(_))) | None => {
                if fallback {
                    self.report.isotropic_intervals += 1;
                }
                // Uniform radiance with Σ L·(moment)_z = DHI: L = DHI/π.
                let l = dhi / std::f64::consts::PI * interval.hours;
                for f in field.iter_mut() {
                    *f += l;
                }
            }
        }
    }

    /// Integrate the masks against the monthly fields and return the totals.
    #[must_use]
    pub fn finish(self) -> ClimateTotals {
        let Self {
            setup,
            direct,
            field,
            ground,
            report,
        } = self;
        let mut diffuse = vec![[0.0; 12]; direct.len()];
        let mut reflected = vec![[0.0; 12]; direct.len()];
        for (s, normal) in setup.normals.iter().enumerate() {
            let projected = setup.dome.projected(*normal);
            let ground_view = 0.5 * (1.0 - normal[2].clamp(-1.0, 1.0));
            let (Some(out), Some(refl)) = (diffuse.get_mut(s), reflected.get_mut(s)) else {
                continue;
            };
            for (m, (month_field, &tau)) in
                field.iter().zip(&setup.canopy_transmittance).enumerate()
            {
                let mut sum = 0.0;
                for (p, (w, f)) in projected.iter().zip(month_field).enumerate() {
                    let pass = match setup.masks.path(s, p) {
                        Path::Open => 1.0,
                        Path::Canopy => tau,
                        Path::Blocked => 0.0,
                    };
                    sum += w * pass * f;
                }
                add_to(out, m, sum);
                add_to(refl, m, ground_view * ground.get(m).copied().unwrap_or(0.0));
            }
        }
        ClimateTotals {
            direct,
            diffuse,
            reflected,
            report,
        }
    }
}

/// `values[i] += v` when `i` is in range.
fn add_to(values: &mut [f64; 12], i: usize, v: f64) {
    if let Some(x) = values.get_mut(i) {
        *x += v;
    }
}

/// Photosynthetic photon conversion of shortwave energy.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ParConversion {
    /// Share of global shortwave energy in 400–700 nm.
    pub par_fraction: f64,
    /// µmol of photons per joule of daylight PAR.
    pub micromol_per_joule: f64,
}

impl Default for ParConversion {
    /// 0.45 of shortwave is PAR and daylight PAR carries 4.57 µmol/J
    /// (`McCree` 1972; `Thimijan` & `Heins` 1983): 2.06 µmol per joule of global
    /// shortwave. A stated assumption with about ±10 % spread across sky
    /// conditions; the DLI it gives is an estimate, not a measurement.
    fn default() -> Self {
        Self {
            par_fraction: 0.45,
            micromol_per_joule: 4.57,
        }
    }
}

/// Mean daily light integral, mol/m²/day, from `energy_wh` (Wh/m²) received
/// over `valid_hours` hours of weather. `None` without valid hours.
#[must_use]
pub fn daily_light_integral(
    energy_wh: f64,
    valid_hours: f64,
    conversion: ParConversion,
) -> Option<f64> {
    (valid_hours > 0.0).then(|| {
        let mol =
            energy_wh * 3600.0 * conversion.par_fraction * conversion.micromol_per_joule * 1e-6;
        mol * 24.0 / valid_hours
    })
}

#[cfg(test)]
mod tests;
