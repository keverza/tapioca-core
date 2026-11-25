//! Daily and monthly summaries of a weather file, and a growing season read
//! from its temperatures (#278 planting).
//!
//! ⚠️ THE SEASON IS A DEFAULT, NOT A HORTICULTURAL FACT. It is the longest run
//! of days whose centred running mean of the daily mean dry-bulb temperature
//! stays at or above a threshold (5 °C by convention). A typical year stitches
//! months from different years, so the smoothing window matters; the caller
//! states both numbers wherever the dates are shown.

use super::{Field, WeatherFile};

/// One calendar day of the data period.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DaySummary {
    /// 1–12.
    pub month: u8,
    /// 1–31.
    pub day: u8,
    /// Mean of the day's valid dry-bulb values, °C; NaN when none.
    pub mean_dry_bulb_c: f64,
    /// Global horizontal energy over the valid intervals, Wh/m².
    pub global_horizontal_wh_m2: f64,
    /// Intervals with no valid global horizontal value.
    pub global_missing: usize,
}

/// One calendar month present in the data period.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct MonthSummary {
    /// Records in the month.
    pub records: usize,
    /// Mean dry bulb over valid records, °C; NaN when none.
    pub mean_dry_bulb_c: f64,
    /// Lowest valid dry bulb, °C; NaN when none.
    pub min_dry_bulb_c: f64,
    /// Highest valid dry bulb, °C; NaN when none.
    pub max_dry_bulb_c: f64,
    /// Global horizontal energy, kWh/m², valid intervals only.
    pub global_horizontal_kwh_m2: f64,
    /// Diffuse horizontal energy, kWh/m², valid intervals only.
    pub diffuse_horizontal_kwh_m2: f64,
    /// Liquid precipitation, mm, valid records only.
    pub precipitation_mm: f64,
    /// Records with no valid global horizontal value.
    pub global_missing: usize,
}

fn mean(sum: f64, count: usize) -> f64 {
    if count == 0 {
        f64::NAN
    } else {
        #[expect(clippy::cast_precision_loss, reason = "a count of records")]
        let n = count as f64;
        sum / n
    }
}

/// One summary per day of the data period, in file order.
#[must_use]
pub fn daily(file: &WeatherFile) -> Vec<DaySummary> {
    let per_day = 24 * usize::try_from(file.header.records_per_hour.max(1)).unwrap_or(1);
    let hours = file.interval_hours();
    file.records
        .chunks(per_day)
        .map(|chunk| {
            let (mut t_sum, mut t_n, mut ghi, mut missing) = (0.0, 0_usize, 0.0, 0_usize);
            for record in chunk {
                let t = record.get(Field::DryBulb);
                if t.is_finite() {
                    t_sum += t;
                    t_n += 1;
                }
                let g = record.get(Field::GlobalHorizontal);
                if g.is_finite() {
                    ghi += g * hours;
                } else {
                    missing += 1;
                }
            }
            let first = chunk.first();
            DaySummary {
                month: first.map_or(0, |r| r.month),
                day: first.map_or(0, |r| r.day),
                mean_dry_bulb_c: mean(t_sum, t_n),
                global_horizontal_wh_m2: ghi,
                global_missing: missing,
            }
        })
        .collect()
}

/// Twelve monthly summaries (January first); a month outside the data period
/// has zero records and NaN temperatures.
#[must_use]
pub fn monthly(file: &WeatherFile) -> [MonthSummary; 12] {
    let hours = file.interval_hours();
    let mut out = [MonthSummary::default(); 12];
    let mut t = [(0.0_f64, 0_usize, f64::INFINITY, f64::NEG_INFINITY); 12];
    for record in &file.records {
        let index = usize::from(record.month.saturating_sub(1));
        let (Some(month), Some(temp)) = (out.get_mut(index), t.get_mut(index)) else {
            continue;
        };
        month.records += 1;
        let dry = record.get(Field::DryBulb);
        if dry.is_finite() {
            *temp = (temp.0 + dry, temp.1 + 1, temp.2.min(dry), temp.3.max(dry));
        }
        let g = record.get(Field::GlobalHorizontal);
        if g.is_finite() {
            month.global_horizontal_kwh_m2 += g * hours / 1000.0;
        } else {
            month.global_missing += 1;
        }
        let d = record.get(Field::DiffuseHorizontal);
        if d.is_finite() {
            month.diffuse_horizontal_kwh_m2 += d * hours / 1000.0;
        }
        let p = record.get(Field::PrecipitationDepth);
        if p.is_finite() {
            month.precipitation_mm += p;
        }
    }
    for (month, (sum, n, low, high)) in out.iter_mut().zip(t) {
        month.mean_dry_bulb_c = mean(sum, n);
        month.min_dry_bulb_c = if n == 0 { f64::NAN } else { low };
        month.max_dry_bulb_c = if n == 0 { f64::NAN } else { high };
    }
    out
}

/// The season rule's inputs.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SeasonRule {
    /// Days in the centred running mean (odd, at least 1).
    pub window_days: usize,
    /// Smoothed daily mean at or above this counts as in season, °C.
    pub threshold_c: f64,
    /// Treat the days as a closed year, so a season may run over New Year
    /// (southern hemisphere).
    pub wraps: bool,
}

impl Default for SeasonRule {
    fn default() -> Self {
        Self {
            window_days: 7,
            threshold_c: 5.0,
            wraps: true,
        }
    }
}

/// Centred running mean of `values` over `window` days, ignoring NaN; NaN
/// where the window holds no valid value.
#[must_use]
pub fn running_mean(values: &[f64], window: usize, wraps: bool) -> Vec<f64> {
    let n = values.len();
    let half = window.max(1) / 2;
    (0..n)
        .map(|i| {
            let (mut sum, mut count) = (0.0, 0_usize);
            for k in 0..=2 * half {
                let j = (i + n + k).checked_sub(half);
                let j = match j {
                    Some(j) if wraps => Some(j % n),
                    Some(j) if j >= n && j - n < n => Some(j - n),
                    _ => None,
                };
                if let Some(v) = j.and_then(|j| values.get(j)).filter(|v| v.is_finite()) {
                    sum += v;
                    count += 1;
                }
            }
            mean(sum, count)
        })
        .collect()
}

/// The longest in-season run as (first, last) day indices, inclusive; `last`
/// is before `first` when the run wraps over the year end. `None` when no
/// day qualifies; `Some((0, n - 1))` when every day does.
#[must_use]
pub fn growing_season(daily_mean_c: &[f64], rule: SeasonRule) -> Option<(usize, usize)> {
    let n = daily_mean_c.len();
    let smooth = running_mean(daily_mean_c, rule.window_days, rule.wraps);
    let warm = |i: usize| smooth.get(i % n).is_some_and(|v| *v >= rule.threshold_c);
    if n == 0 || !(0..n).any(warm) {
        return None;
    }
    if (0..n).all(warm) {
        return Some((0, n - 1));
    }
    let span = if rule.wraps { 2 * n } else { n };
    let (mut best, mut best_len, mut start, mut len) = ((0, 0), 0, 0, 0);
    for i in 0..span {
        if warm(i) {
            if len == 0 {
                start = i;
            }
            len += 1;
            if len > best_len && len <= n {
                best_len = len;
                best = (start % n, i % n);
            }
        } else {
            len = 0;
        }
    }
    Some(best)
}
