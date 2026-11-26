//! CIE general sky (ISO 15469:2004 / CIE S 011/E:2003), types 1–15, and the
//! traditional CIE standard overcast sky (Moon & Spencer 1942, CIE 1955).
//!
//! Relative luminance `L/L_z = f(χ)·φ(Z) / (f(Z_s)·φ(0))` with the gradation
//! φ(Z) = 1 + a·exp(b / cos Z) (φ = 1 at the horizon) and the indicatrix
//! f(χ) = 1 + c·(exp(d·χ) − exp(d·π/2)) + e·cos² χ, where Z is the element's
//! zenith angle, `Z_s` the sun's and χ the angle between element and sun.

use std::f64::consts::FRAC_PI_2;

/// (a, b, c, d, e) for types 1–15, in the standard's table order.
const TYPES: [[f64; 5]; 15] = [
    [4.0, -0.70, 0.0, -1.0, 0.00],
    [4.0, -0.70, 2.0, -1.5, 0.15],
    [1.1, -0.80, 0.0, -1.0, 0.00],
    [1.1, -0.80, 2.0, -1.5, 0.15],
    [0.0, -1.00, 0.0, -1.0, 0.00],
    [0.0, -1.00, 2.0, -1.5, 0.15],
    [0.0, -1.00, 5.0, -2.5, 0.30],
    [0.0, -1.00, 10.0, -3.0, 0.45],
    [-1.0, -0.55, 2.0, -1.5, 0.15],
    [-1.0, -0.55, 5.0, -2.5, 0.30],
    [-1.0, -0.55, 10.0, -3.0, 0.45],
    [-1.0, -0.32, 10.0, -3.0, 0.45],
    [-1.0, -0.32, 16.0, -3.0, 0.30],
    [-1.0, -0.15, 16.0, -3.0, 0.30],
    [-1.0, -0.15, 24.0, -2.8, 0.15],
];

/// The gradation φ(Z) for zenith angle `z` (radians).
fn gradation(a: f64, b: f64, z: f64) -> f64 {
    let c = z.cos();
    if c <= 1e-9 {
        1.0
    } else {
        1.0 + a * (b / c).exp()
    }
}

/// The scattering indicatrix f(χ) for angle `chi` (radians) from the sun.
fn indicatrix(c: f64, d: f64, e: f64, chi: f64) -> f64 {
    1.0 + c * ((d * chi).exp() - (d * FRAC_PI_2).exp()) + e * chi.cos().powi(2)
}

/// `L/L_z` of CIE general sky `kind` (1–15) at unit `direction` with unit
/// `sun` (both in the sky frame, z up). `None` for an unknown type or a
/// direction below the horizon. A sun below the horizon is evaluated at its
/// angle; types 1, 3 and 5 do not depend on it.
#[must_use]
#[expect(clippy::many_single_char_names, reason = "the paper's notation")]
pub fn relative(kind: u8, direction: [f64; 3], sun: [f64; 3]) -> Option<f64> {
    let [a, b, c, d, e] = *TYPES.get(usize::from(kind).checked_sub(1)?)?;
    if direction[2] < 0.0 {
        return None;
    }
    let z = direction[2].clamp(-1.0, 1.0).acos();
    let zs = sun[2].clamp(-1.0, 1.0).acos();
    let chi = super::dome::dot(direction, sun).clamp(-1.0, 1.0).acos();
    let value = indicatrix(c, d, e, chi) * gradation(a, b, z)
        / (indicatrix(c, d, e, zs) * gradation(a, b, 0.0));
    Some(value.max(0.0))
}

/// `L/L_z` of the traditional CIE standard overcast sky, (1 + 2 sin α)/3 for
/// altitude α. The sky behind the BRE vertical sky component; independent of
/// the sun.
#[must_use]
pub fn traditional_overcast(direction: [f64; 3]) -> Option<f64> {
    (direction[2] >= 0.0).then(|| (1.0 + 2.0 * direction[2].min(1.0)) / 3.0)
}
