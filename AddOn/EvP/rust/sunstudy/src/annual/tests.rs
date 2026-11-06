#![expect(
    clippy::float_cmp,
    reason = "the hand fixtures are exact binary fractions; equality is the contract"
)]

use super::*;

fn date(year: i32, month: u8, day: u8) -> CalendarDate {
    CalendarDate::new(year, month, day).unwrap_or(CalendarDate {
        year: 0,
        month: 0,
        day: 0,
    })
}

fn sun(direction: [f64; 3]) -> SunStep {
    SunStep {
        hour: 12,
        minute: 0,
        direction,
        altitude_degrees: 30.0,
    }
}

const UP: [f64; 3] = [0.0, 0.0, 1.0];
const SOUTH: [f64; 3] = [0.0, -0.6, 0.8];
const NORTH: [f64; 3] = [0.0, 0.6, 0.8];

/// Two samples: a roof (normal up) and a north facade (normal +y).
const POSITIONS: [f64; 6] = [0.0, 0.0, 3.0, 0.0, 1.0, 1.5];
const NORMALS: [f64; 6] = [0.0, 0.0, 1.0, 0.0, 1.0, 0.0];

fn samples() -> SampleSet<'static> {
    SampleSet {
        positions: &POSITIONS,
        normals: Some(&NORMALS),
        count: 2,
    }
}

fn day<'a>(month: u8, weight: f64, steps: &'a [SunStep], lit: &'a [f64]) -> DayContribution<'a> {
    DayContribution {
        month,
        weight_days: weight,
        steps,
        hours_per_step: 0.5,
        samples: samples(),
        lit_hours: lit,
    }
}

#[test]
fn leap_years_and_month_lengths() {
    assert!(is_leap(2024) && is_leap(2000) && !is_leap(1900) && !is_leap(2026));
    assert_eq!(days_in_month(2024, 2), Some(29));
    assert_eq!(days_in_month(2026, 2), Some(28));
    assert_eq!(days_in_month(2026, 13), None);
    assert_eq!(days_in_month(2026, 0), None);
    assert_eq!(DateRange::whole_year(2024).map(|r| r.day_count()), Ok(366));
    assert_eq!(DateRange::whole_year(2026).map(|r| r.day_count()), Ok(365));
}

#[test]
fn dates_and_ranges_are_validated() {
    assert_eq!(
        CalendarDate::new(2026, 2, 29),
        Err(AnnualError::InvalidDate)
    );
    assert_eq!(
        CalendarDate::new(2026, 4, 31),
        Err(AnnualError::InvalidDate)
    );
    assert_eq!(CalendarDate::new(2026, 1, 0), Err(AnnualError::InvalidDate));
    assert_eq!(CalendarDate::new(0, 1, 1), Err(AnnualError::InvalidDate));
    assert!(CalendarDate::new(2024, 2, 29).is_ok());
    assert_eq!(
        DateRange::new(date(2026, 5, 2), date(2026, 5, 1)),
        Err(AnnualError::InvalidRange)
    );
    assert_eq!(
        DateRange::new(date(2026, 12, 1), date(2027, 1, 31)),
        Err(AnnualError::InvalidRange)
    );
    let one = DateRange::new(date(2026, 3, 21), date(2026, 3, 21));
    assert_eq!(one.map(|r| r.day_count()), Ok(1));
}

#[test]
fn preview_weights_sum_to_the_covered_days_and_stay_in_their_month() -> Result<(), AnnualError> {
    for year in [2024, 2026] {
        let range = DateRange::whole_year(year)?;
        let days = schedule(&range, AnnualSampling::Preview);
        assert_eq!(days.len(), 12);
        let total: f64 = days.iter().map(|d| d.weight_days).sum();
        assert_eq!(total, f64::from(range.day_count()));
        for d in &days {
            assert_eq!(
                d.weight_days,
                f64::from(days_in_month(year, d.date.month).unwrap_or(0))
            );
        }
        // February's middle day: 1 + (28 - 1) / 2 = 14, in both years.
        assert_eq!(
            days.get(1).map(|d| d.date.day),
            Some(if year == 2024 { 15 } else { 14 })
        );
    }
    Ok(())
}

#[test]
fn partial_months_weigh_only_their_covered_days() -> Result<(), AnnualError> {
    let range = DateRange::new(date(2026, 3, 20), date(2026, 5, 10))?;
    assert_eq!(range.day_count(), 12 + 30 + 10);
    let preview = schedule(&range, AnnualSampling::Preview);
    let summary: Vec<(u8, u8, f64)> = preview
        .iter()
        .map(|d| (d.date.month, d.date.day, d.weight_days))
        .collect();
    assert_eq!(summary, vec![(3, 25, 12.0), (4, 15, 30.0), (5, 5, 10.0)]);
    let daily = schedule(&range, AnnualSampling::DailyResolved);
    assert_eq!(daily.len(), 52);
    assert_eq!(daily.first().map(|d| d.date), Some(date(2026, 3, 20)));
    assert_eq!(daily.last().map(|d| d.date), Some(date(2026, 5, 10)));
    assert!(daily.iter().all(|d| d.weight_days == 1.0));
    assert!(
        daily
            .windows(2)
            .all(|w| matches!(w, [a, b] if a.date < b.date))
    );
    Ok(())
}

#[test]
fn quantities_keep_their_order_and_back_faces_are_not_loss() -> Result<(), AnnualError> {
    let mut acc = AnnualAccumulator::new(2)?;
    // Two steps: sun overhead, sun to the south. The roof faces both; the
    // north facade faces neither (overhead is incidence 0: culled).
    let steps = [sun(UP), sun(SOUTH)];
    // The roof was shaded for one of them.
    assert_eq!(acc.add_day(&day(6, 30.0, &steps, &[0.5, 0.0])), Ok(()));
    let lit = acc.values(AnnualQuantity::Unoccluded, 6);
    let facing = acc.values(AnnualQuantity::SurfaceFacing, 6);
    let loss = acc.values(AnnualQuantity::ObstructionLoss, 6);
    assert_eq!(lit, Ok(vec![15.0, 0.0]));
    assert_eq!(facing, Ok(vec![30.0, 0.0]));
    assert_eq!(loss, Ok(vec![15.0, 0.0]));
    assert_eq!(acc.ceiling(6), Ok(30.0));
    assert_eq!(acc.ceiling(0), Ok(30.0));
    assert_eq!(acc.ceiling(7), Ok(0.0));
    assert_eq!(
        acc.values(AnnualQuantity::Unoccluded, 7),
        Ok(vec![0.0, 0.0])
    );
    Ok(())
}

#[test]
fn the_period_is_the_sum_of_its_days() -> Result<(), AnnualError> {
    let mut acc = AnnualAccumulator::new(2)?;
    let north = [sun(NORTH), sun(UP)];
    assert_eq!(acc.add_day(&day(1, 1.0, &north, &[0.5, 0.5])), Ok(()));
    assert_eq!(acc.add_day(&day(1, 1.0, &north, &[0.0, 0.5])), Ok(()));
    assert_eq!(acc.add_day(&day(2, 2.0, &north, &[0.5, 0.0])), Ok(()));
    assert_eq!(acc.add_sunless_day(12, 31.0), Ok(()));
    assert_eq!(
        acc.values(AnnualQuantity::Unoccluded, 0),
        Ok(vec![1.5, 1.0])
    );
    assert_eq!(
        acc.values(AnnualQuantity::Unoccluded, 1),
        Ok(vec![0.5, 1.0])
    );
    assert_eq!(
        acc.values(AnnualQuantity::Unoccluded, 2),
        Ok(vec![1.0, 0.0])
    );
    assert_eq!(acc.ceiling(0), Ok(4.0));
    let summary = acc.summary()?;
    assert_eq!(summary.sampled_days, 4);
    assert_eq!(summary.represented_days, 35.0);
    assert_eq!(summary.unoccluded.count, 2);
    assert_eq!(summary.unoccluded.max_hours, 1.5);
    assert_eq!(summary.unoccluded.daylight_hours, 4.0);
    assert_eq!(acc.represented_days().get(11), Some(&31.0));
    // Means a day: the period represents 35 days, two of them in January.
    assert_eq!(acc.represented_in(1), Ok(2.0));
    assert_eq!(
        acc.daily_means(AnnualQuantity::Unoccluded, 1),
        Ok(vec![0.25, 0.5])
    );
    assert_eq!(acc.daily_ceiling(1), Ok(1.0));
    assert_eq!(acc.daily_ceiling(0), Ok(4.0 / 35.0));
    assert_eq!(
        acc.daily_means(AnnualQuantity::Unoccluded, 5),
        Ok(vec![0.0, 0.0])
    );
    assert_eq!(acc.daily_ceiling(5), Ok(0.0));
    Ok(())
}

#[test]
fn refused_days_change_nothing() -> Result<(), AnnualError> {
    let mut acc = AnnualAccumulator::new(2)?;
    let steps = [sun(UP)];
    assert_eq!(
        acc.add_day(&day(13, 1.0, &steps, &[0.0, 0.0])),
        Err(AnnualError::InvalidMonth)
    );
    assert_eq!(
        acc.add_day(&day(0, 1.0, &steps, &[0.0, 0.0])),
        Err(AnnualError::InvalidMonth)
    );
    assert_eq!(
        acc.add_day(&day(1, 0.0, &steps, &[0.0, 0.0])),
        Err(AnnualError::InvalidWeight)
    );
    assert_eq!(
        acc.add_day(&day(1, f64::NAN, &steps, &[0.0, 0.0])),
        Err(AnnualError::InvalidWeight)
    );
    assert_eq!(
        acc.add_day(&day(1, 1.0, &steps, &[0.0])),
        Err(AnnualError::InvalidHours)
    );
    assert_eq!(
        acc.add_day(&day(1, 1.0, &steps, &[f64::NAN, 0.0])),
        Err(AnnualError::InvalidHours)
    );
    assert_eq!(
        acc.add_day(&day(1, 1.0, &steps, &[-0.5, 0.0])),
        Err(AnnualError::InvalidHours)
    );
    // The north facade cannot be lit by an overhead sun it does not face.
    assert_eq!(
        acc.add_day(&day(1, 1.0, &steps, &[0.5, 0.5])),
        Err(AnnualError::InconsistentDay)
    );
    assert_eq!(acc.sampled_days(), 0);
    assert_eq!(acc.ceiling(0), Ok(0.0));
    assert_eq!(
        acc.values(AnnualQuantity::SurfaceFacing, 0),
        Ok(vec![0.0, 0.0])
    );
    Ok(())
}

#[test]
fn other_receptors_are_refused() -> Result<(), AnnualError> {
    let mut acc = AnnualAccumulator::new(2)?;
    let steps = [sun(UP)];
    assert_eq!(acc.add_day(&day(1, 1.0, &steps, &[0.5, 0.0])), Ok(()));
    let moved = [0.0, 0.0, 3.0, 0.0, 1.0, 1.75];
    let other = DayContribution {
        samples: SampleSet {
            positions: &moved,
            normals: Some(&NORMALS),
            count: 2,
        },
        ..day(1, 1.0, &steps, &[0.5, 0.0])
    };
    assert_eq!(acc.add_day(&other), Err(AnnualError::ReceptorsChanged));
    assert!(!acc.same_receptors(&other.samples));
    assert!(acc.same_receptors(&samples()));
    let unnormalled = DayContribution {
        samples: SampleSet {
            positions: &POSITIONS,
            normals: None,
            count: 2,
        },
        ..day(1, 1.0, &steps, &[0.5, 0.0])
    };
    assert_eq!(
        acc.add_day(&unnormalled),
        Err(AnnualError::ReceptorsChanged)
    );
    assert_eq!(acc.sampled_days(), 1);
    Ok(())
}

#[test]
fn ground_samples_without_normals_always_face() -> Result<(), AnnualError> {
    let positions = [0.0, 0.0, 0.0];
    let mut acc = AnnualAccumulator::new(1)?;
    let steps = [sun(NORTH), sun(SOUTH)];
    let ground = DayContribution {
        month: 3,
        weight_days: 2.0,
        steps: &steps,
        hours_per_step: 0.25,
        samples: SampleSet {
            positions: &positions,
            normals: None,
            count: 1,
        },
        lit_hours: &[0.25],
    };
    assert_eq!(acc.add_day(&ground), Ok(()));
    assert_eq!(acc.values(AnnualQuantity::SurfaceFacing, 3), Ok(vec![1.0]));
    assert_eq!(
        acc.values(AnnualQuantity::ObstructionLoss, 3),
        Ok(vec![0.5])
    );
    Ok(())
}

#[test]
fn equal_days_in_equal_order_give_equal_bits() {
    let steps = [sun(UP), sun(SOUTH)];
    let run = || {
        let mut acc = AnnualAccumulator::new(2).ok()?;
        for (month, lit) in [(1, 0.1), (2, 0.3), (3, 0.7)] {
            acc.add_day(&day(month, 1.0 / 3.0, &steps, &[lit, 0.0]))
                .ok()?;
        }
        acc.values(AnnualQuantity::Unoccluded, 0).ok()
    };
    let (a, b) = (run(), run());
    assert!(a.is_some());
    assert_eq!(
        a.map(|v| v.iter().map(|x| x.to_bits()).collect::<Vec<_>>()),
        b.map(|v| v.iter().map(|x| x.to_bits()).collect::<Vec<_>>())
    );
}

#[test]
fn daily_bins_hold_their_lower_edge() {
    assert_eq!(threshold::daily_bin(0.0), 0);
    assert_eq!(threshold::daily_bin(0.49), 0);
    assert_eq!(threshold::daily_bin(0.5), 1);
    // Eight 15-minute steps, with the float error a sum can carry.
    assert_eq!(threshold::daily_bin(2.0 - 1e-9), 4);
    assert_eq!(threshold::daily_bin(4.75), 8);
    assert_eq!(threshold::daily_bin(30.0), threshold::DAILY_BINS - 1);
    assert_eq!(threshold::DAILY_EDGES.get(3), Some(&2.0));
}

#[test]
fn threshold_days_access_ratio_and_rows() -> Result<(), AnnualError> {
    let mut acc = AnnualAccumulator::new(2)?;
    let steps = [sun(UP), sun(SOUTH)];
    // The roof: 1 h on 10 days, 0.5 h on 20; the facade never faces.
    acc.add_day(&day(3, 10.0, &steps, &[1.0, 0.0]))?;
    acc.add_day(&day(4, 20.0, &steps, &[0.5, 0.0]))?;
    acc.add_sunless_day(12, 5.0)?;
    // >= 1 h: 10 days; >= 0.5 h: 30; >= 2 h: none. Sunless days are 0 h days.
    assert_eq!(acc.days_at_least(1), Ok(vec![10.0, 0.0]));
    assert_eq!(acc.days_at_least(0), Ok(vec![30.0, 0.0]));
    assert_eq!(acc.days_at_least(3), Ok(vec![0.0, 0.0]));
    assert_eq!(acc.days_at_least(13), Err(AnnualError::InvalidHours));
    // Access: lit 20 of 30 facing hours; the facade faced nothing -> 0.
    assert_eq!(
        acc.values(AnnualQuantity::AccessRatio, 0),
        Ok(vec![20.0 / 30.0, 0.0])
    );
    assert_eq!(
        acc.daily_means(AnnualQuantity::AccessRatio, 3),
        Ok(vec![1.0, 0.0])
    );
    let Some(row) = acc.sample_row(0) else {
        return Err(AnnualError::InvalidHours);
    };
    assert_eq!(row.unoccluded.get(2), Some(&10.0));
    assert_eq!(row.facing.get(3), Some(&20.0));
    assert_eq!(row.daily.first(), Some(&5.0));
    assert_eq!(row.daily.get(1), Some(&20.0));
    assert_eq!(row.daily.get(2), Some(&10.0));
    assert!(acc.sample_row(2).is_none());
    Ok(())
}

#[test]
fn the_archive_keeps_each_days_steps_and_hours() -> Result<(), AnnualError> {
    use super::archive::{DayArchive, KeptDay};
    // Two steps: the roof faces both, the north facade only the northern sun.
    let steps = [sun(SOUTH), sun(NORTH)];
    // Roof lit at both; facade shaded at its one facing step.
    let lit = [1_u8, 1, 0, 0];
    let set = samples();
    let mut archive = DayArchive::new(2);
    archive.push(&KeptDay {
        month: 3,
        day: 21,
        weight_days: 1.0,
        steps: &steps,
        hours_per_step: 0.5,
        samples: &set,
        lit: &lit,
    })?;
    archive.push_sunless(12, 21, 1.0);
    assert_eq!(archive.days().len(), 2);
    assert_eq!(archive.instant(0, 0)?, vec![1.0, 0.0]);
    assert_eq!(archive.instant(0, 5)?, vec![0.0, 0.0]); // past the day
    assert_eq!(archive.day(0, AnnualQuantity::Unoccluded)?, vec![1.0, 0.0]);
    assert_eq!(
        archive.day(0, AnnualQuantity::SurfaceFacing)?,
        vec![1.0, 0.5]
    );
    assert_eq!(
        archive.day(0, AnnualQuantity::ObstructionLoss)?,
        vec![0.0, 0.5]
    );
    assert_eq!(archive.day(0, AnnualQuantity::AccessRatio)?, vec![1.0, 0.0]);
    assert_eq!(archive.day(1, AnnualQuantity::Unoccluded)?, vec![0.0, 0.0]);
    assert_eq!(archive.sample_steps(0, 0), vec![1, 1]);
    assert_eq!(
        archive
            .days()
            .first()
            .map(super::archive::ArchivedDay::daylight),
        Some(1.0)
    );
    assert_eq!(archive.instant(2, 0), Err(AnnualError::InvalidRange));
    // A wrong shape is refused and keeps nothing.
    assert_eq!(
        archive.push(&KeptDay {
            month: 3,
            day: 22,
            weight_days: 1.0,
            steps: &steps,
            hours_per_step: 0.5,
            samples: &set,
            lit: &[1, 0, 1]
        }),
        Err(AnnualError::InvalidHours)
    );
    assert_eq!(archive.days().len(), 2);
    Ok(())
}
