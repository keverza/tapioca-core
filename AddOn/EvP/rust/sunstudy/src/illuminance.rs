//! Climate-based exterior illuminance per sample (#155 F4/P4, R7): lux-hours
//! and the hours at or above a threshold, decided interval by interval.
//!
//! No new rays: the sample's sky shares ([`crate::climate::SkyMasks`]) and the
//! beam paths the irradiation run already traced are reused. Per interval:
//!
//! - **Direct**: the direct normal illuminance × max(0, n·s) × the beam's path.
//! - **Diffuse**: the interval's sky field (Perez 1993 shape, normalised to
//!   DHI) scaled by `E_dh / DHI`, against each sample's projected, masked
//!   patches. Without DHI the file's diffuse illuminance is spread
//!   isotropically.
//! - **Reflected**: the global horizontal illuminance × albedo × `(1 − n_z)/2`,
//!   the ground unshaded, as for irradiation.
//!
//! Where the illuminance comes from is part of the result, per interval:
//!
//! - [`IntervalLux::File`]: the weather file's own direct normal and diffuse
//!   horizontal illuminance, when both are present (the default
//!   [`LuxSource::FileWhenValid`]).
//! - [`IntervalLux::Efficacy`]: irradiance × the Perez 1990 luminous
//!   efficacies for the interval's sky ([`crate::sky::perez::efficacy`]).
//! - [`IntervalLux::Unavailable`]: radiation missing and no file illuminance;
//!   it adds nothing and is counted. ⚠️ NOT ZERO LUX.
//!
//! ⚠️ WEATHER LUX ONLY. Nothing here is a vertical sky component times a design
//! sky's illuminance, and nothing is derived from sun hours; such a figure
//! must never be labelled as this one.

use crate::sky::Dome;
use crate::sky::perez::{PerezInput, efficacy, parameters};

/// Where an interval's illuminance is taken from, when there is a choice.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum LuxSource {
    /// The file's direct normal and diffuse horizontal illuminance when both
    /// are present, else the luminous efficacy.
    #[default]
    FileWhenValid,
    /// Always the luminous efficacy, whatever the file holds.
    EfficacyOnly,
}

/// The run's illuminance settings.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct LuxOptions {
    /// An interval counts towards a sample's hours above when its mean
    /// illuminance there is at or above this, lux.
    pub threshold_lux: f64,
    /// Where to take the illuminance from.
    pub source: LuxSource,
}

/// Where one interval's illuminance came from.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IntervalLux {
    /// The weather file's illuminance fields.
    File,
    /// Irradiance × the Perez 1990 luminous efficacies.
    Efficacy,
    /// Radiation missing and no file illuminance: nothing added.
    Unavailable,
}

/// One interval's illuminance inputs.
#[derive(Clone, Copy, Debug)]
pub struct LuxInterval<'a> {
    /// Month, 0 = January.
    pub month: usize,
    /// Interval length, hours.
    pub hours: f64,
    /// Unit vector to the sun, model frame.
    pub sun: [f64; 3],
    /// Mean DNI and DHI, W/m²; NaN missing.
    pub dni: f64,
    /// See `dni`.
    pub dhi: f64,
    /// Day of the year, 1–366.
    pub day_of_year: u32,
    /// Dew point, °C; NaN missing.
    pub dew_point_c: f64,
    /// Ground albedo, already defaulted.
    pub albedo: f64,
    /// The file's mean global horizontal, direct normal and diffuse
    /// horizontal illuminance, lux; NaN missing.
    pub file_lux: [f64; 3],
    /// The interval's sky field per patch, W/m²/sr, normalised to its DHI
    /// (`Σ projected_up·L = DHI`); empty when DHI was 0 or missing.
    pub field: &'a [f64],
    /// One beam path per sample when the beam was traced; empty otherwise.
    pub beam: &'a [u8],
    /// Canopy transmittance of the month.
    pub canopy_transmittance: f64,
}

/// What the illuminance pass did, per month.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct LuxReport {
    /// The threshold, lux.
    pub threshold_lux: f64,
    /// Daylight hours whose illuminance came from the file ...
    pub file_hours: [f64; 12],
    /// ... from the luminous efficacy ...
    pub efficacy_hours: [f64; 12],
    /// ... or could not be had (missing radiation): nothing added.
    pub unavailable_hours: [f64; 12],
}

/// Per sample, per month.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct LuxTotals {
    /// Σ illuminance × hours, lux-hours.
    pub lux_hours: Vec<[f64; 12]>,
    /// Hours at or above the threshold.
    pub hours_above: Vec<[f64; 12]>,
    /// The pass's report.
    pub report: LuxReport,
}

/// The accumulator, kept beside the irradiation run.
#[derive(Clone, Debug)]
pub struct Lux {
    options: LuxOptions,
    patches: usize,
    normals: Vec<[f64; 3]>,
    /// Per sample then patch: projected weight × open share.
    open: Vec<f32>,
    /// The same through canopy only; empty when no sample sees canopy.
    canopy: Vec<f32>,
    totals: LuxTotals,
}

impl Lux {
    /// Weights from the dome, the normals and each sample's shares (`open`,
    /// `canopy`: 0–1 per sample then patch; `canopy` may be all zero).
    #[must_use]
    #[expect(
        clippy::cast_possible_truncation,
        reason = "f32 sky weights and luminance: the dot product needs no more"
    )]
    pub fn new(
        options: LuxOptions,
        dome: &Dome,
        normals: &[[f64; 3]],
        share: impl Fn(usize, usize) -> (f64, f64),
    ) -> Self {
        let patches = dome.len();
        let mut open = Vec::with_capacity(normals.len() * patches);
        let mut canopy = Vec::with_capacity(normals.len() * patches);
        let mut any_canopy = false;
        for (s, n) in normals.iter().enumerate() {
            let projected = dome.projected(*n);
            for (p, w) in projected.iter().enumerate() {
                let (o, c) = share(s, p);
                open.push((w * o) as f32);
                canopy.push((w * c) as f32);
                any_canopy |= c > 0.0;
            }
        }
        if !any_canopy {
            canopy = Vec::new();
        }
        let samples = normals.len();
        Self {
            options,
            patches,
            normals: normals.to_vec(),
            open,
            canopy,
            totals: LuxTotals {
                lux_hours: vec![[0.0; 12]; samples],
                hours_above: vec![[0.0; 12]; samples],
                report: LuxReport {
                    threshold_lux: options.threshold_lux,
                    ..LuxReport::default()
                },
            },
        }
    }

    /// The totals so far.
    #[must_use]
    pub fn totals(&self) -> &LuxTotals {
        &self.totals
    }

    /// Add one interval. Night adds nothing and is not counted.
    #[expect(
        clippy::cast_possible_truncation,
        reason = "f32 sky weights and luminance: the dot product needs no more"
    )]
    pub fn add(&mut self, i: &LuxInterval<'_>) -> IntervalLux {
        let sin_alt = i.sun[2];
        if sin_alt.is_nan() || sin_alt <= 0.0 || i.month >= 12 || i.hours.is_nan() || i.hours <= 0.0
        {
            return IntervalLux::Unavailable;
        }
        let Some((source, direct, diffuse)) = self.illuminance(i) else {
            add_to(&mut self.totals.report.unavailable_hours, i.month, i.hours);
            return IntervalLux::Unavailable;
        };
        let slot = match source {
            IntervalLux::File => &mut self.totals.report.file_hours,
            _ => &mut self.totals.report.efficacy_hours,
        };
        add_to(slot, i.month, i.hours);
        let global = match (source, i.file_lux[0]) {
            (IntervalLux::File, g) if g.is_finite() && g >= 0.0 => g,
            _ => direct * sin_alt + diffuse,
        };
        // Per-patch sky luminance, cd/m² (lm/m²/sr).
        let luminance: Vec<f64> = if i.dhi > 0.0 && i.field.len() == self.patches {
            let scale = diffuse / i.dhi;
            i.field.iter().map(|l| l * scale).collect()
        } else {
            vec![diffuse / std::f64::consts::PI; self.patches]
        };
        let luminance32: Vec<f32> = luminance.iter().map(|&l| l as f32).collect();
        let beam_given = i.beam.len() == self.normals.len();
        let tau = i.canopy_transmittance;
        let threshold = self.options.threshold_lux;
        let reflected = global * i.albedo;
        let patches = self.patches;
        let month = i.month;
        let hours = i.hours;
        let work = |s: usize, normal: &[f64; 3], lux_h: &mut [f64; 12], above: &mut [f64; 12]| {
            let range = s * patches..(s + 1) * patches;
            let mut sky = 0.0_f32;
            if let Some(w) = self.open.get(range.clone()) {
                sky += dot32(w, &luminance32);
            }
            if let Some(w) = self.canopy.get(range) {
                sky += tau as f32 * dot32(w, &luminance32);
            }
            let mut e = f64::from(sky) + reflected * 0.5 * (1.0 - normal[2].clamp(-1.0, 1.0));
            if beam_given && direct > 0.0 {
                let pass = match i.beam.get(s).copied().unwrap_or(0) {
                    2 => 1.0,
                    1 => tau,
                    _ => 0.0,
                };
                let cos = normal[0] * i.sun[0] + normal[1] * i.sun[1] + normal[2] * i.sun[2];
                if cos > 0.0 {
                    e += direct * cos * pass;
                }
            }
            add_to(lux_h, month, e * hours);
            if e >= threshold {
                add_to(above, month, hours);
            }
        };
        let samples = self.normals.len();
        let threads = std::thread::available_parallelism()
            .map_or(1, std::num::NonZeroUsize::get)
            .min(samples / 2048 + 1);
        let chunk = samples.div_ceil(threads.max(1)).max(1);
        let normals = &self.normals;
        let (lux_hours, hours_above) = (&mut self.totals.lux_hours, &mut self.totals.hours_above);
        std::thread::scope(|scope| {
            for (k, ((lux_chunk, above_chunk), normal_chunk)) in lux_hours
                .chunks_mut(chunk)
                .zip(hours_above.chunks_mut(chunk))
                .zip(normals.chunks(chunk))
                .enumerate()
            {
                let work = &work;
                scope.spawn(move || {
                    for (j, ((lux_h, above), normal)) in lux_chunk
                        .iter_mut()
                        .zip(above_chunk.iter_mut())
                        .zip(normal_chunk)
                        .enumerate()
                    {
                        work(k * chunk + j, normal, lux_h, above);
                    }
                });
            }
        });
        source
    }

    /// The interval's source, direct normal and diffuse horizontal
    /// illuminance, lux; `None` when unavailable.
    fn illuminance(&self, i: &LuxInterval<'_>) -> Option<(IntervalLux, f64, f64)> {
        let [_, file_direct, file_diffuse] = i.file_lux;
        let file_ok = file_direct.is_finite()
            && file_direct >= 0.0
            && file_diffuse.is_finite()
            && file_diffuse >= 0.0;
        if self.options.source == LuxSource::FileWhenValid && file_ok {
            return Some((IntervalLux::File, file_direct, file_diffuse));
        }
        if !i.dni.is_finite() || !i.dhi.is_finite() {
            return None;
        }
        let (dni, dhi) = (i.dni.max(0.0), i.dhi.max(0.0));
        if dhi <= 0.0 {
            // No diffuse light to classify the sky: none from the sky, and a
            // beam without a sky is not credible enough to convert.
            return Some((IntervalLux::Efficacy, 0.0, 0.0));
        }
        let p = parameters(&PerezInput {
            sun: i.sun,
            dni,
            dhi,
            day_of_year: i.day_of_year,
            dew_point_c: i.dew_point_c.is_finite().then_some(i.dew_point_c),
        })
        .ok()?;
        let (kd, kb) = efficacy(&p);
        Some((IntervalLux::Efficacy, dni * kb, dhi * kd))
    }
}

fn dot32(a: &[f32], b: &[f32]) -> f32 {
    a.iter().zip(b).map(|(x, y)| x * y).sum()
}

fn add_to(values: &mut [f64; 12], i: usize, v: f64) {
    if let Some(x) = values.get_mut(i) {
        *x += v;
    }
}

#[cfg(test)]
mod tests;
