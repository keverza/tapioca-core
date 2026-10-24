//! Result statistics: count, min/mean/max hours, fully lit and fully shaded.
//!
//! Port of `SunStudy/SunStudyResultSummary.hpp`. The sum runs in sample order
//! and min/max use C++ `std::min`/`std::max`, so every bit matches.

use crate::raster::{cpp_max, cpp_min};
use crate::series::count_to_f64;

/// Summary of one study's sun hours.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SunStudyResultSummary {
    /// Samples summarised.
    pub count: usize,
    /// Samples at the full daylight (within 1e-9 h).
    pub fully_lit: usize,
    /// Samples with no sun (within 1e-9 h).
    pub fully_shaded: usize,
    /// Least hours.
    pub min_hours: f64,
    /// Mean hours (sample-order sum / count).
    pub mean_hours: f64,
    /// Most hours.
    pub max_hours: f64,
    /// The daylight the study could reach.
    pub daylight_hours: f64,
}

/// Summarise per-sample hours against the day's possible daylight.
#[must_use]
pub fn summarize_sun_hours(hours: &[f64], daylight_hours: f64) -> SunStudyResultSummary {
    let mut summary = SunStudyResultSummary {
        count: hours.len(),
        daylight_hours,
        ..SunStudyResultSummary::default()
    };
    let Some(&first) = hours.first() else {
        return summary;
    };
    summary.min_hours = first;
    summary.max_hours = first;
    let mut sum = 0.0;
    for &value in hours {
        summary.min_hours = cpp_min(summary.min_hours, value);
        summary.max_hours = cpp_max(summary.max_hours, value);
        sum += value;
        summary.fully_lit += usize::from(value >= daylight_hours - 1e-9);
        summary.fully_shaded += usize::from(value <= 1e-9);
    }
    summary.mean_hours = sum / count_to_f64(hours.len());
    summary
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn empty_is_zero_with_the_daylight_kept() {
        let s = summarize_sun_hours(&[], 12.0);
        assert_eq!((s.count, s.fully_lit, s.fully_shaded), (0, 0, 0));
        assert!((s.daylight_hours - 12.0).abs() < f64::EPSILON);
    }

    #[test]
    fn statistics_and_thresholds() {
        let s = summarize_sun_hours(&[0.0, 6.0, 12.0 - 1e-10, 3.0], 12.0);
        assert_eq!((s.count, s.fully_lit, s.fully_shaded), (4, 1, 1));
        assert!(s.min_hours.abs() < f64::EPSILON && (s.max_hours - (12.0 - 1e-10)).abs() < 1e-12);
        assert!((s.mean_hours - (21.0 - 1e-10) / 4.0).abs() < 1e-12);
    }
}
