//! Month/day arithmetic for an EPW data period, which names dates without a
//! year: the file's leap flag (or a 29 February record) decides February.

/// Days in `month` (1–12); 0 for an invalid month.
#[must_use]
pub fn days_in_month(month: u8, leap: bool) -> u8 {
    match month {
        1 | 3 | 5 | 7 | 8 | 10 | 12 => 31,
        4 | 6 | 9 | 11 => 30,
        2 if leap => 29,
        2 => 28,
        _ => 0,
    }
}

/// The day after (`month`, `day`), wrapping 31 December to 1 January.
#[must_use]
pub fn next_day(month: u8, day: u8, leap: bool) -> (u8, u8) {
    if day < days_in_month(month, leap) {
        (month, day + 1)
    } else if month < 12 {
        (month + 1, 1)
    } else {
        (1, 1)
    }
}

/// Every date from `start` to `end` inclusive, wrapping the year end; empty
/// for an invalid date.
#[must_use]
pub fn period_days(start: (u8, u8), end: (u8, u8), leap: bool) -> Vec<(u8, u8)> {
    let valid = |(m, d): (u8, u8)| d >= 1 && d <= days_in_month(m, leap);
    if !valid(start) || !valid(end) {
        return Vec::new();
    }
    let mut days = vec![start];
    let mut at = start;
    while at != end && days.len() < 367 {
        at = next_day(at.0, at.1, leap);
        days.push(at);
    }
    days
}

/// Parse an EPW `M/D` (or `M/D/YYYY`) date, spaces allowed (`" 1/ 1"`).
#[must_use]
pub fn parse_month_day(text: &str) -> Option<(u8, u8)> {
    let mut parts = text.split('/').map(str::trim);
    let month = parts.next()?.parse::<u8>().ok()?;
    let day = parts.next()?.parse::<u8>().ok()?;
    (1..=12).contains(&month).then_some((month, day))
}
