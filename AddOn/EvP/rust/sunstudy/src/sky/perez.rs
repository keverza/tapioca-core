//! The Perez all-weather sky (Perez, Seals & Michalsky 1993, *Solar Energy*
//! 50(3)), its luminous efficacies and the Perez irradiance transposition
//! (Perez et al. 1990, *Solar Energy* 44(5)), from the papers' equations and
//! tables.
//!
//! The sky's parameters follow the published model as `gendaylit` applies it:
//! the sun's altitude is capped at 87°; clearness ε is held to [1, 12.01) and
//! brightness Δ to [0.01, 0.6]; for 1.065 < ε < 2.8 the luminance
//! coefficients use Δ ≥ 0.2. Every one of those holds is reported in
//! [`PerezParameters::held`], never applied silently.

use super::dome::dot;
use std::f64::consts::PI;

/// Upper bounds of the eight clearness bins; the last bin runs to the cap.
const BIN_UPPER: [f64; 7] = [1.065, 1.230, 1.500, 1.950, 2.800, 4.500, 6.200];
/// Clearness is held to `[CLEAR_LOW, CLEAR_HIGH)`.
const CLEAR_LOW: f64 = 1.0;
const CLEAR_HIGH: f64 = 12.01;
/// Brightness is held to `[BRIGHT_LOW, BRIGHT_HIGH]`.
const BRIGHT_LOW: f64 = 0.01;
const BRIGHT_HIGH: f64 = 0.6;
/// Solar constant, W/m², as the model was fitted with.
const SOLAR_CONSTANT: f64 = 1367.0;
/// κ of the clearness formula, for the zenith angle in radians.
const KAPPA: f64 = 1.041;
/// Precipitable water, cm, when no dew point is given (`gendaylit`'s value).
const DEFAULT_WATER_CM: f64 = 2.0;

/// Perez 1993, Table 1: per bin, (a1..a4, b1..b4, c1..c4, d1..d4, e1..e4).
const LUMINANCE: [[f64; 20]; 8] = [
    [
        1.3525, -0.2576, -0.2690, -1.4366, -0.7670, 0.0007, 1.2734, -0.1233, 2.8000, 0.6004,
        1.2375, 1.0000, 1.8734, 0.6297, 0.9738, 0.2809, 0.0356, -0.1246, -0.5718, 0.9938,
    ],
    [
        -1.2219, -0.7730, 1.4148, 1.1016, -0.2054, 0.0367, -3.9128, 0.9156, 6.9750, 0.1774, 6.4477,
        -0.1239, -1.5798, -0.5081, -1.7812, 0.1080, 0.2624, 0.0672, -0.2190, -0.4285,
    ],
    [
        -1.1000, -0.2515, 0.8952, 0.0156, 0.2782, -0.1812, -4.5000, 1.1766, 24.7219, -13.0812,
        -37.7000, 34.8438, -5.0000, 1.5218, 3.9229, -2.6204, -0.0156, 0.1597, 0.4199, -0.5562,
    ],
    [
        -0.5484, -0.6654, -0.2672, 0.7117, 0.7234, -0.6219, -5.6812, 2.6297, 33.3389, -18.3000,
        -62.2500, 52.0781, -3.5000, 0.0016, 1.1477, 0.1062, 0.4659, -0.3296, -0.0876, -0.0329,
    ],
    [
        -0.6000, -0.3566, -2.5000, 2.3250, 0.2937, 0.0496, -5.6812, 1.8415, 21.0000, -4.7656,
        -21.5906, 7.2492, -3.5000, -0.1554, 1.4062, 0.3988, 0.0032, 0.0766, -0.0656, -0.1294,
    ],
    [
        -1.0156, -0.3670, 1.0078, 1.4051, 0.2875, -0.5328, -3.8500, 3.3750, 14.0000, -0.9999,
        -7.1406, 7.5469, -3.4000, -0.1078, -1.0750, 1.5702, -0.0672, 0.4016, 0.3017, -0.4844,
    ],
    [
        -1.0000, 0.0211, 0.5025, -0.5119, -0.3000, 0.1922, 0.7023, -1.6317, 19.0000, -5.0000,
        1.2438, -1.9094, -4.0000, 0.0250, 0.3844, 0.2656, 1.0468, -0.3788, -2.4517, 1.4656,
    ],
    [
        -1.0500, 0.0289, 0.4260, 0.3590, -0.3250, 0.1156, 0.7781, 0.0025, 31.0625, -14.5000,
        -46.1148, 55.3750, -7.2312, 0.4050, 13.3500, 0.6234, 1.5000, -0.6426, 1.8564, 0.5636,
    ],
];

/// Perez 1990, Table 4: diffuse horizontal efficacy (a, b, c, d) per bin, lm/W.
const DIFFUSE_EFFICACY: [[f64; 4]; 8] = [
    [97.24, -0.46, 12.00, -8.91],
    [107.22, 1.15, 0.59, -3.95],
    [104.97, 2.96, -5.53, -8.77],
    [102.39, 5.59, -13.95, -13.90],
    [100.71, 5.94, -22.75, -23.74],
    [106.42, 3.83, -36.15, -28.83],
    [141.88, 1.90, -53.24, -14.03],
    [152.23, 0.35, -45.27, -7.98],
];

/// Perez 1990, Table 4: direct normal efficacy (a, b, c, d) per bin, lm/W.
const DIRECT_EFFICACY: [[f64; 4]; 8] = [
    [57.20, -4.55, -2.98, 117.12],
    [98.99, -3.46, -1.21, 12.38],
    [109.83, -4.90, -1.71, -8.81],
    [110.34, -5.84, -1.99, -4.56],
    [106.36, -3.97, -1.75, -6.16],
    [107.19, -1.25, -1.51, -26.73],
    [105.75, 0.77, -1.26, -34.44],
    [101.18, 1.58, -1.10, -8.29],
];

/// Perez 1990, Table 6 (all-site composite): (f11, f12, f13, f21, f22, f23).
const TRANSPOSITION: [[f64; 6]; 8] = [
    [-0.008, 0.588, -0.062, -0.060, 0.072, -0.022],
    [0.130, 0.683, -0.151, -0.019, 0.066, -0.029],
    [0.330, 0.487, -0.221, 0.055, -0.064, -0.026],
    [0.568, 0.187, -0.295, 0.109, -0.152, -0.014],
    [0.873, -0.392, -0.362, 0.226, -0.462, 0.001],
    [1.132, -1.237, -0.412, 0.288, -0.823, 0.056],
    [1.060, -1.600, -0.359, 0.264, -1.127, 0.131],
    [0.678, -0.327, -0.250, 0.156, -1.377, 0.251],
];

/// One weather interval's sky input.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PerezInput {
    /// Unit vector to the sun, sky frame (z up). Its altitude must be above 0.
    pub sun: [f64; 3],
    /// Direct normal irradiance, mean W/m² over the interval.
    pub dni: f64,
    /// Diffuse horizontal irradiance, mean W/m² over the interval, above 0.
    pub dhi: f64,
    /// Day of the year, 1–366, for the extraterrestrial irradiance.
    pub day_of_year: u32,
    /// Dew point, °C, for precipitable water; `None` uses 2 cm.
    pub dew_point_c: Option<f64>,
}

/// Which of the model's range holds were applied.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[expect(
    clippy::struct_excessive_bools,
    reason = "independent flags, one per hold"
)]
pub struct Held {
    /// The sun's altitude was capped at 87°.
    pub altitude: bool,
    /// Clearness was moved into [1, 12.01).
    pub clearness: bool,
    /// Brightness was moved into [0.01, 0.6].
    pub brightness: bool,
    /// The luminance coefficients used Δ = 0.2 instead of a lower Δ.
    pub luminance_brightness: bool,
}

/// The interval's sky condition.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PerezParameters {
    /// Sun zenith angle used, radians (≥ 3°).
    pub zenith: f64,
    /// Sky clearness ε, after holding.
    pub clearness: f64,
    /// Sky brightness Δ, after holding.
    pub brightness: f64,
    /// Clearness bin, 0–7.
    pub bin: usize,
    /// Precipitable water, cm.
    pub water_cm: f64,
    /// The holds applied.
    pub held: Held,
}

/// Why an interval has no Perez sky.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PerezRefusal {
    /// The sun is at or below the horizon.
    SunDown,
    /// DHI is missing, not positive, or DNI is missing or negative.
    Irradiance,
    /// The day of the year is outside 1–366.
    Day,
}

/// The extraterrestrial irradiance correction (Spencer 1971), (R0/R)².
fn eccentricity(day_of_year: u32) -> f64 {
    let g = 2.0 * PI * f64::from(day_of_year.saturating_sub(1)) / 365.0;
    1.000_11
        + 0.034_221 * g.cos()
        + 0.001_28 * g.sin()
        + 0.000_719 * (2.0 * g).cos()
        + 0.000_077 * (2.0 * g).sin()
}

/// Relative optical air mass (Kasten 1966), zenith in radians.
fn air_mass(zenith: f64) -> f64 {
    let degrees = zenith.to_degrees().min(90.0);
    1.0 / (zenith.cos().max(0.0) + 0.15 * (93.885 - degrees).powf(-1.253))
}

/// The clearness bin, 0–7.
fn bin_of(clearness: f64) -> usize {
    BIN_UPPER.iter().take_while(|&&u| clearness >= u).count()
}

/// The interval's clearness, brightness and bin.
///
/// # Errors
/// A sun at or below the horizon, unusable irradiance, or a day outside 1–366.
pub fn parameters(input: &PerezInput) -> Result<PerezParameters, PerezRefusal> {
    if !(1..=366).contains(&input.day_of_year) {
        return Err(PerezRefusal::Day);
    }
    if input.sun[2].is_nan() || input.sun[2] <= 0.0 {
        return Err(PerezRefusal::SunDown);
    }
    if input.dhi.is_nan() || input.dhi <= 0.0 || input.dni.is_nan() || input.dni < 0.0 {
        return Err(PerezRefusal::Irradiance);
    }
    let mut held = Held::default();
    let mut zenith = input.sun[2].min(1.0).acos();
    let cap = 3.0_f64.to_radians();
    if zenith < cap {
        zenith = cap;
        held.altitude = true;
    }
    let z3 = KAPPA * zenith.powi(3);
    let mut clearness = ((input.dhi + input.dni) / input.dhi + z3) / (1.0 + z3);
    let mut brightness =
        input.dhi * air_mass(zenith) / (SOLAR_CONSTANT * eccentricity(input.day_of_year));
    if clearness < CLEAR_LOW || clearness >= CLEAR_HIGH {
        clearness = clearness.clamp(CLEAR_LOW, CLEAR_HIGH - 0.001);
        held.clearness = true;
    }
    if !(BRIGHT_LOW..=BRIGHT_HIGH).contains(&brightness) {
        brightness = brightness.clamp(BRIGHT_LOW, BRIGHT_HIGH);
        held.brightness = true;
    }
    let water_cm = input
        .dew_point_c
        .filter(|t| t.is_finite())
        .map_or(DEFAULT_WATER_CM, |t| (0.07 * t - 0.075).exp());
    Ok(PerezParameters {
        zenith,
        clearness,
        brightness,
        bin: bin_of(clearness),
        water_cm,
        held,
    })
}

/// The five luminance coefficients (a, b, c, d, e) for the interval.
/// Also returns whether Δ was raised to 0.2 for them.
#[must_use]
pub fn coefficients(p: &PerezParameters) -> ([f64; 5], bool) {
    let mut delta = p.brightness;
    let mut raised = false;
    if p.clearness > 1.065 && p.clearness < 2.8 && delta < 0.2 {
        delta = 0.2;
        raised = true;
    }
    let z = p.zenith;
    let row = LUMINANCE.get(p.bin).copied().unwrap_or([0.0; 20]);
    let term = |i: usize| {
        let x = |j: usize| row.get(4 * i + j).copied().unwrap_or(0.0);
        (x(0), x(1), x(2), x(3))
    };
    let linear = |i: usize| {
        let (x0, x1, x2, x3) = term(i);
        x0 + x1 * z + delta * (x2 + x3 * z)
    };
    let mut out = [linear(0), linear(1), linear(2), linear(3), linear(4)];
    if p.bin == 0 {
        // The first bin's c and d have their own forms (Perez 1993, eq. 6).
        let (c0, c1, c2, c3) = term(2);
        let (d0, d1, d2, d3) = term(3);
        out[2] = (delta * (c0 + c1 * z)).powf(c2).exp() - c3;
        out[3] = -(delta * (d0 + d1 * z)).exp() + d2 + delta * d3;
    }
    (out, raised)
}

/// Relative luminance of the all-weather sky at unit `direction` (above the
/// horizon), for the coefficients and unit `sun`. May be negative near the
/// horizon; the caller clamps and reports it.
#[must_use]
#[expect(clippy::many_single_char_names, reason = "the paper's notation")]
pub fn relative(coefficients: &[f64; 5], direction: [f64; 3], sun: [f64; 3]) -> f64 {
    let [a, b, c, d, e] = *coefficients;
    let cos_zeta = direction[2].max(0.01);
    let cos_gamma = dot(direction, sun).clamp(-1.0, 1.0);
    let gamma = cos_gamma.acos();
    (1.0 + a * (b / cos_zeta).exp()) * (1.0 + c * (d * gamma).exp() + e * cos_gamma * cos_gamma)
}

/// Luminous efficacies (diffuse horizontal, direct normal), lm/W.
#[must_use]
#[expect(clippy::many_single_char_names, reason = "the paper's notation")]
pub fn efficacy(p: &PerezParameters) -> (f64, f64) {
    let w = p.water_cm;
    let z = p.zenith;
    let [a, b, c, d] = DIFFUSE_EFFICACY.get(p.bin).copied().unwrap_or([0.0; 4]);
    let diffuse = a + b * w + c * z.cos() + d * p.brightness.ln();
    let [a, b, c, d] = DIRECT_EFFICACY.get(p.bin).copied().unwrap_or([0.0; 4]);
    let direct = a + b * w + c * (5.73 * z - 5.0).exp() + d * p.brightness;
    (diffuse.max(0.0), direct.max(0.0))
}

/// Diffuse irradiance on an unobstructed plane with unit `normal` (sky frame)
/// by the Perez 1990 transposition, W/m². The cross-check for the
/// patch-integrated sky, never the production path. Uses the unheld Δ.
///
/// # Errors
/// As [`parameters`].
pub fn transposed_diffuse(input: &PerezInput, normal: [f64; 3]) -> Result<f64, PerezRefusal> {
    let p = parameters(input)?;
    let zenith = input.sun[2].min(1.0).acos();
    let delta = input.dhi * air_mass(zenith) / (SOLAR_CONSTANT * eccentricity(input.day_of_year));
    let [f11, f12, f13, f21, f22, f23] = TRANSPOSITION.get(p.bin).copied().unwrap_or([0.0; 6]);
    let f1 = (f11 + f12 * delta + zenith * f13).max(0.0);
    let f2 = f21 + f22 * delta + zenith * f23;
    let cos_tilt = normal[2].clamp(-1.0, 1.0);
    let sin_tilt = (1.0 - cos_tilt * cos_tilt).max(0.0).sqrt();
    let a = dot(normal, input.sun).max(0.0);
    let b = zenith.cos().max(85.0_f64.to_radians().cos());
    Ok(input.dhi * ((1.0 - f1) * 0.5 * (1.0 + cos_tilt) + f1 * a / b + f2 * sin_tilt))
}
