//! Sky patch domes (#269 §2): Tregenza 145 and Reinhart 577.
//!
//! Frame: the **sky frame** is x = east, y = true north, z = up, right-handed.
//! The caller rotates into the model frame with the project's north angle;
//! nothing here knows about the model.
//!
//! Order (the contract's summation order, fixed so sums are bit-for-bit
//! reproducible): rows from the horizon upward; within a row, azimuth
//! clockwise from true north (N → E → S → W), the first patch centred on north;
//! the zenith cap last. A patch spans a whole number of azimuth steps, so its
//! edges are centre ± half a step.
//!
//! Each patch carries its exact integrals over its own solid angle, not just a
//! centre direction: [`Patch::solid_angle`] = ∫ dΩ and [`Patch::moment`] =
//! ∫ d dΩ. For a patch wholly in front of a plane with normal n, ∫ (n·d) dΩ =
//! n·moment exactly, which is why an unobstructed horizontal plane sums to π.

use std::f64::consts::{FRAC_PI_2, PI, TAU};

/// The two patch sets of the contract.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum PatchSet {
    /// Tregenza (1987): seven 12° rows of 30, 30, 24, 24, 18, 12, 6 patches
    /// and a zenith cap above 84°: 145 patches.
    Tregenza,
    /// Reinhart (2001) with subdivision 2: each Tregenza row split into two
    /// 6° rows of twice the patches, and a zenith cap above 87°: 577 patches.
    Reinhart,
}

impl PatchSet {
    /// Patch count, zenith cap included.
    #[must_use]
    pub fn count(self) -> usize {
        match self {
            Self::Tregenza => 145,
            Self::Reinhart => 577,
        }
    }

    /// Stable name used in a result's sky identity.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            Self::Tregenza => "tregenza-145",
            Self::Reinhart => "reinhart-577",
        }
    }

    /// Rows as (patches in the row, row height in degrees), horizon first.
    fn rows(self) -> Vec<(u32, f64)> {
        const TREGENZA: [u32; 7] = [30, 30, 24, 24, 18, 12, 6];
        match self {
            Self::Tregenza => TREGENZA.iter().map(|&n| (n, 12.0)).collect(),
            Self::Reinhart => TREGENZA
                .iter()
                .flat_map(|&n| [(n * 2, 6.0), (n * 2, 6.0)])
                .collect(),
        }
    }
}

/// One sky patch, with its exact integrals.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Patch {
    /// Lower and upper altitude, radians.
    pub altitude: [f64; 2],
    /// Azimuth span clockwise from true north, radians; `[0, TAU]` for the cap.
    /// The first patch of a row spans negative to positive.
    pub azimuth: [f64; 2],
    /// ∫ dΩ over the patch, steradians.
    pub solid_angle: f64,
    /// ∫ d dΩ over the patch in the sky frame, steradians.
    pub moment: [f64; 3],
    /// The unit direction of [`Self::moment`]: the patch's centroid direction,
    /// where a sky distribution is evaluated.
    pub direction: [f64; 3],
    /// How far [`Dome::rotated`] turned the patch from the sky frame, radians
    /// counter-clockwise; 0 in the sky frame.
    pub frame_angle: f64,
}

/// A patch set, built once.
#[derive(Clone, Debug, PartialEq)]
pub struct Dome {
    set: PatchSet,
    patches: Vec<Patch>,
}

impl Dome {
    /// Build `set`'s patches in the contract's order.
    #[must_use]
    pub fn new(set: PatchSet) -> Self {
        let mut patches = Vec::with_capacity(set.count());
        let mut low = 0.0_f64;
        for (count, height) in set.rows() {
            let high = low + height.to_radians();
            let step = TAU / f64::from(count);
            for k in 0..count {
                let centre = step * f64::from(k);
                patches.push(patch(
                    [low, high],
                    [centre - 0.5 * step, centre + 0.5 * step],
                ));
            }
            low = high;
        }
        patches.push(patch([low, FRAC_PI_2], [0.0, TAU]));
        Self { set, patches }
    }

    /// This dome turned about z by `angle` radians, counter-clockwise seen from
    /// above: sky-frame north (0, 1, 0) lands on (−sin angle, cos angle).
    /// Moments and directions turn; altitude and azimuth spans keep their
    /// sky-frame values. Rotation about z keeps every horizontal integral.
    #[must_use]
    pub fn rotated(&self, angle: f64) -> Self {
        let (s, c) = angle.sin_cos();
        let turn = |v: [f64; 3]| [c * v[0] - s * v[1], s * v[0] + c * v[1], v[2]];
        Self {
            set: self.set,
            patches: self
                .patches
                .iter()
                .map(|p| Patch {
                    moment: turn(p.moment),
                    direction: turn(p.direction),
                    frame_angle: p.frame_angle + angle,
                    ..*p
                })
                .collect(),
        }
    }

    /// The set this dome was built from.
    #[must_use]
    pub fn set(&self) -> PatchSet {
        self.set
    }

    /// The patches, in the contract's order.
    #[must_use]
    pub fn patches(&self) -> &[Patch] {
        &self.patches
    }

    /// Number of patches.
    #[must_use]
    pub fn len(&self) -> usize {
        self.patches.len()
    }

    /// Always false: a dome has at least its zenith cap.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.patches.is_empty()
    }

    /// Each patch's projected solid angle onto a plane with unit `normal`,
    /// ∫ max(0, n·d) dΩ, steradians, in the dome's order. Exact (n·moment) for
    /// a patch wholly in front of the plane, 0 wholly behind; a patch the
    /// plane cuts is integrated on a [`CUT_CELLS`]² midpoint grid of exact
    /// sub-cell solid angles.
    #[must_use]
    pub fn projected(&self, normal: [f64; 3]) -> Vec<f64> {
        self.patches.iter().map(|p| p.projected(normal)).collect()
    }

    /// The sky view factor of a plane with unit `normal`: the visible patches'
    /// projected solid angle over π, in fixed patch order. `visible` holds one
    /// flag per patch; a missing flag counts as obstructed. An unobstructed
    /// horizontal plane gives 1, a vertical one 0.5.
    #[must_use]
    pub fn view_factor(&self, normal: [f64; 3], visible: &[bool]) -> f64 {
        let mut sum = 0.0;
        for (p, &open) in self.patches.iter().zip(visible) {
            if open {
                sum += p.projected(normal);
            }
        }
        sum / PI
    }
}

/// Sub-cells per side when a plane cuts a patch.
pub const CUT_CELLS: u32 = 16;

impl Patch {
    /// ∫ max(0, n·d) dΩ over this patch, steradians (see [`Dome::projected`]).
    #[must_use]
    pub fn projected(&self, normal: [f64; 3]) -> f64 {
        let [a0, a1] = self.altitude;
        let [p0, p1] = self.azimuth;
        let (fs, fc) = self.frame_angle.sin_cos();
        let unit = |a: f64, phi: f64| {
            let v = unit(a, phi);
            [fc * v[0] - fs * v[1], fs * v[0] + fc * v[1], v[2]]
        };
        // Corners and mid-edges decide whether the plane cuts the patch.
        let mut ahead = false;
        let mut behind = false;
        for a in [a0, f64::midpoint(a0, a1), a1] {
            for k in 0..=4 {
                let phi = p0 + (p1 - p0) * f64::from(k) / 4.0;
                let s = dot(normal, unit(a, phi));
                ahead |= s > 0.0;
                behind |= s < 0.0;
            }
        }
        if !behind {
            return dot(normal, self.moment).max(0.0);
        }
        if !ahead {
            return 0.0;
        }
        let n = f64::from(CUT_CELLS);
        let (da, dp) = ((a1 - a0) / n, (p1 - p0) / n);
        let mut sum = 0.0;
        for i in 0..CUT_CELLS {
            let lo = a0 + da * f64::from(i);
            let omega = dp * ((lo + da).sin() - lo.sin());
            let a = lo + 0.5 * da;
            for j in 0..CUT_CELLS {
                let phi = p0 + dp * (f64::from(j) + 0.5);
                sum += dot(normal, unit(a, phi)).max(0.0) * omega;
            }
        }
        sum
    }
}

/// The unit vector at altitude `a` and azimuth `phi` (radians, clockwise from
/// north), sky frame.
fn unit(a: f64, phi: f64) -> [f64; 3] {
    let (sa, ca) = a.sin_cos();
    let (sp, cp) = phi.sin_cos();
    [sp * ca, cp * ca, sa]
}

/// Dot product of two 3-vectors.
pub(crate) fn dot(a: [f64; 3], b: [f64; 3]) -> f64 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}

/// The exact integrals of the patch between two altitudes and two azimuths.
fn patch(altitude: [f64; 2], azimuth: [f64; 2]) -> Patch {
    let [a0, a1] = altitude;
    let [p0, p1] = azimuth;
    let span = p1 - p0;
    // dΩ = cos a da dφ; x = sin φ cos a, y = cos φ cos a, z = sin a.
    let solid_angle = span * (a1.sin() - a0.sin());
    let cos2 = |a: f64| 0.5 * a + 0.25 * (2.0 * a).sin(); // ∫ cos² a da
    let horizontal = cos2(a1) - cos2(a0);
    let moment = [
        horizontal * (p0.cos() - p1.cos()),
        horizontal * (p1.sin() - p0.sin()),
        span * 0.5 * (a1.sin().powi(2) - a0.sin().powi(2)),
    ];
    let length = dot(moment, moment).sqrt();
    let direction = if length > 0.0 {
        [moment[0] / length, moment[1] / length, moment[2] / length]
    } else {
        [0.0, 0.0, 1.0]
    };
    Patch {
        altitude,
        azimuth,
        solid_angle,
        moment,
        direction,
        frame_angle: 0.0,
    }
}
