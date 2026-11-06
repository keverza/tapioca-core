//! Days at or above a daily direct-sun threshold, kept as a small per-sample
//! histogram of each represented day's hours (#152 threshold maps).
//!
//! The edges are the thresholds people ask for: half-hour steps to 4 h (the
//! range sunlight-exposure guidance uses), then wider steps to 12 h. A
//! threshold is one of these edges; "days with at least `edge` hours" is the
//! sum of the bins from that edge up. 14 `f32` a sample (56 bytes), whatever
//! the period.

/// Lower edges of every bin but the first, hours: bin 0 is `[0, 0.5)`, bin
/// `i` is `[EDGES[i-1], EDGES[i])`, the last is `[12, inf)`.
pub const DAILY_EDGES: [f64; 13] = [
    0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 5.0, 6.0, 8.0, 10.0, 12.0,
];

/// Bins per sample.
pub const DAILY_BINS: usize = DAILY_EDGES.len() + 1;

/// The bin a day of `hours` falls in. A tolerance of a millionth of an hour
/// keeps a day of exactly 2 h (eight 15-minute steps) in the 2 h bin.
#[must_use]
pub fn daily_bin(hours: f64) -> usize {
    DAILY_EDGES
        .iter()
        .take_while(|edge| hours + 1e-6 >= **edge)
        .count()
}

/// Days at or above `DAILY_EDGES[edge]` from one sample's bins.
#[must_use]
pub fn days_from(bins: &[f32], edge: usize) -> f64 {
    bins.iter()
        .skip(edge + 1)
        .map(|days| f64::from(*days))
        .sum()
}
