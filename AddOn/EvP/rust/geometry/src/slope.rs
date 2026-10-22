//! One slope value type for every grade input (LD-07): angle, percent, rise:run
//! or a height over a run. A slope is carried as the unit the user chose and
//! converted to a dimensionless gradient (rise per metre of plan run) only at the
//! point of use; it is never stored as a bare float.
//!
//! A slope is non-negative. Vertical (90 degrees, or a rise over a zero run) is
//! an infinite gradient, which a cut face may legitimately request.

/// A non-negative slope in the unit it was authored in.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Slope {
    /// Angle above horizontal in radians, `0..=pi/2`.
    Angle {
        /// Radians; `pi/2` is vertical.
        radians: f64,
    },
    /// Rise per hundred units of run (100 percent is 45 degrees).
    Percent(f64),
    /// A ratio such as 1:3 (`rise` 1, `run` 3); both dimensionless.
    RiseRun {
        /// Rise part of the ratio.
        rise: f64,
        /// Run part of the ratio; zero with a positive rise is vertical.
        run: f64,
    },
    /// A height change over a plan length, both in metres.
    Height {
        /// Rise in metres.
        rise_metres: f64,
        /// Plan run in metres; zero with a positive rise is vertical.
        run_metres: f64,
    },
}

/// A slope that does not describe a non-negative grade.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SlopeError {
    /// NaN, a negative part, an angle above vertical, or `0/0`.
    Invalid,
}

impl SlopeError {
    /// Static diagnostic beginning with the stable refusal code.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::Invalid => {
                "SlopeInvalid: slope must be a non-negative angle (0..90 degrees), percent, ratio or height"
            }
        }
    }
}

fn ratio(rise: f64, run: f64) -> Result<f64, SlopeError> {
    if !(rise >= 0.0 && run >= 0.0) || rise.is_infinite() || run.is_infinite() {
        return Err(SlopeError::Invalid);
    }
    if run > 0.0 {
        Ok(rise / run)
    } else if rise > 0.0 {
        Ok(f64::INFINITY)
    } else {
        Err(SlopeError::Invalid)
    }
}

impl Slope {
    /// Dimensionless gradient: plan-run metres to rise metres, `tan(angle)`.
    /// Vertical is `f64::INFINITY`.
    ///
    /// # Errors
    /// [`SlopeError::Invalid`] for NaN, negative values, angles above vertical
    /// or an undefined `0/0` ratio.
    pub fn gradient(self) -> Result<f64, SlopeError> {
        match self {
            Self::Angle { radians } => {
                if !(0.0..=std::f64::consts::FRAC_PI_2).contains(&radians) {
                    return Err(SlopeError::Invalid);
                }
                // tan(pi/2) is finite in floating point; vertical is exact here.
                if (std::f64::consts::FRAC_PI_2 - radians).abs() <= f64::EPSILON {
                    Ok(f64::INFINITY)
                } else {
                    Ok(radians.tan())
                }
            }
            Self::Percent(percent) => {
                if percent >= 0.0 && percent.is_finite() {
                    Ok(percent / 100.0)
                } else {
                    Err(SlopeError::Invalid)
                }
            }
            Self::RiseRun { rise, run } => ratio(rise, run),
            Self::Height {
                rise_metres,
                run_metres,
            } => ratio(rise_metres, run_metres),
        }
    }

    /// The same grade as an angle above horizontal in radians.
    ///
    /// # Errors
    /// As [`Slope::gradient`].
    pub fn radians(self) -> Result<f64, SlopeError> {
        self.gradient().map(f64::atan)
    }

    /// The same grade in percent; vertical is `f64::INFINITY`.
    ///
    /// # Errors
    /// As [`Slope::gradient`].
    pub fn percent(self) -> Result<f64, SlopeError> {
        self.gradient().map(|gradient| gradient * 100.0)
    }
}

#[cfg(test)]
mod tests {
    use super::{Slope, SlopeError};
    use std::f64::consts::{FRAC_PI_2, FRAC_PI_4};

    fn close(a: f64, b: f64) -> bool {
        (a - b).abs() <= 1e-12 * a.abs().max(b.abs()).max(1.0)
    }

    #[test]
    fn every_unit_names_the_same_forty_five_degree_grade() {
        for slope in [
            Slope::Angle { radians: FRAC_PI_4 },
            Slope::Percent(100.0),
            Slope::RiseRun {
                rise: 1.0,
                run: 1.0,
            },
            Slope::Height {
                rise_metres: 2.5,
                run_metres: 2.5,
            },
        ] {
            assert!(
                close(slope.gradient().unwrap_or(f64::NAN), 1.0),
                "{slope:?}"
            );
            assert!(
                close(slope.radians().unwrap_or(f64::NAN), FRAC_PI_4),
                "{slope:?}"
            );
            assert!(
                close(slope.percent().unwrap_or(f64::NAN), 100.0),
                "{slope:?}"
            );
        }
    }

    #[test]
    fn units_round_trip_through_the_gradient() {
        let ratio = Slope::RiseRun {
            rise: 1.0,
            run: 3.0,
        };
        let gradient = ratio.gradient().unwrap_or(f64::NAN);
        let angle = Slope::Angle {
            radians: ratio.radians().unwrap_or(f64::NAN),
        };
        let percent = Slope::Percent(ratio.percent().unwrap_or(f64::NAN));
        assert!(close(angle.gradient().unwrap_or(f64::NAN), gradient));
        assert!(close(percent.gradient().unwrap_or(f64::NAN), gradient));
        assert!(close(gradient, 1.0 / 3.0));
    }

    #[test]
    fn vertical_is_an_infinite_gradient_in_every_unit() {
        assert_eq!(
            Slope::Angle { radians: FRAC_PI_2 }.gradient(),
            Ok(f64::INFINITY)
        );
        assert_eq!(
            Slope::RiseRun {
                rise: 1.0,
                run: 0.0
            }
            .gradient(),
            Ok(f64::INFINITY)
        );
        assert_eq!(
            Slope::Height {
                rise_metres: 0.4,
                run_metres: 0.0
            }
            .gradient(),
            Ok(f64::INFINITY)
        );
        assert_eq!(Slope::Angle { radians: 0.0 }.gradient(), Ok(0.0));
    }

    #[test]
    fn invalid_slopes_are_refused_not_clamped() {
        for slope in [
            Slope::Angle { radians: -0.1 },
            Slope::Angle { radians: 1.6 },
            Slope::Angle { radians: f64::NAN },
            Slope::Percent(-1.0),
            Slope::Percent(f64::INFINITY),
            Slope::RiseRun {
                rise: 0.0,
                run: 0.0,
            },
            Slope::RiseRun {
                rise: -1.0,
                run: 2.0,
            },
            Slope::Height {
                rise_metres: 1.0,
                run_metres: f64::NAN,
            },
        ] {
            assert_eq!(slope.gradient(), Err(SlopeError::Invalid), "{slope:?}");
        }
        assert!(SlopeError::Invalid.message().starts_with("SlopeInvalid:"));
    }
}
