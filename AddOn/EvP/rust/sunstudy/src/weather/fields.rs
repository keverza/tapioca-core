//! The numeric EPW data fields kept by the parser, with their units, missing
//! markers and documented ranges (the `EnergyPlus` weather data dictionary).
//!
//! ⚠️ A MISSING MARKER IS NOT A VALUE. `9999` global horizontal radiation is
//! "not measured", not 9999 Wh/m²; it becomes NaN and is counted, never zero.

/// One numeric EPW field. The discriminant is the index into
/// [`super::WeatherRecord::values`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Field {
    /// Dry-bulb temperature, °C, at the timestamp.
    DryBulb,
    /// Dew-point temperature, °C, at the timestamp.
    DewPoint,
    /// Relative humidity, %, at the timestamp.
    RelativeHumidity,
    /// Station pressure, Pa, at the timestamp.
    Pressure,
    /// Extraterrestrial horizontal radiation, Wh/m² over the interval.
    ExtraterrestrialHorizontal,
    /// Extraterrestrial direct normal radiation, Wh/m² over the interval.
    ExtraterrestrialDirectNormal,
    /// Horizontal infrared radiation from the sky, Wh/m² over the interval.
    HorizontalInfrared,
    /// Global horizontal radiation, Wh/m² over the interval.
    GlobalHorizontal,
    /// Direct normal radiation, Wh/m² over the interval.
    DirectNormal,
    /// Diffuse horizontal radiation, Wh/m² over the interval.
    DiffuseHorizontal,
    /// Global horizontal illuminance, lux, averaged over the interval.
    GlobalIlluminance,
    /// Direct normal illuminance, lux, averaged over the interval.
    DirectIlluminance,
    /// Diffuse horizontal illuminance, lux, averaged over the interval.
    DiffuseIlluminance,
    /// Zenith luminance, cd/m², averaged over the interval.
    ZenithLuminance,
    /// Wind direction, degrees clockwise from north, at the timestamp.
    WindDirection,
    /// Wind speed, m/s, at the timestamp.
    WindSpeed,
    /// Total sky cover, tenths.
    TotalSkyCover,
    /// Opaque sky cover, tenths.
    OpaqueSkyCover,
    /// Visibility, km.
    Visibility,
    /// Ceiling height, m (77777 means unlimited).
    CeilingHeight,
    /// Precipitable water, mm.
    PrecipitableWater,
    /// Aerosol optical depth, thousandths.
    AerosolOpticalDepth,
    /// Snow depth, cm.
    SnowDepth,
    /// Days since the last snowfall.
    DaysSinceSnow,
    /// Ground albedo, ratio.
    Albedo,
    /// Liquid precipitation depth, mm over the interval.
    PrecipitationDepth,
    /// Liquid precipitation quantity, hours.
    PrecipitationHours,
}

/// Number of numeric fields kept per record.
pub const FIELD_COUNT: usize = 27;

/// A field's place in the file and its validity rules.
#[derive(Clone, Copy, Debug)]
pub struct FieldSpec {
    /// The field.
    pub field: Field,
    /// Zero-based CSV column in a data line.
    pub column: usize,
    /// Values at or above this are the missing marker.
    pub missing_at: f64,
    /// Smallest documented valid value.
    pub min: f64,
    /// Largest documented valid value.
    pub max: f64,
    /// True when the value covers the interval ending at the timestamp
    /// (radiation, illuminance, precipitation), false for an instant.
    pub interval: bool,
}

const fn spec(
    field: Field,
    column: usize,
    missing_at: f64,
    range: (f64, f64),
    interval: bool,
) -> FieldSpec {
    FieldSpec {
        field,
        column,
        missing_at,
        min: range.0,
        max: range.1,
        interval,
    }
}

const BIG: f64 = 1.0e12;

/// Every kept field, in [`Field`] order.
pub const FIELDS: [FieldSpec; FIELD_COUNT] = [
    spec(Field::DryBulb, 6, 99.9, (-70.0, 70.0), false),
    spec(Field::DewPoint, 7, 99.9, (-70.0, 70.0), false),
    spec(Field::RelativeHumidity, 8, 999.0, (0.0, 110.0), false),
    spec(Field::Pressure, 9, 999_999.0, (31_000.0, 120_000.0), false),
    spec(
        Field::ExtraterrestrialHorizontal,
        10,
        9999.0,
        (0.0, BIG),
        true,
    ),
    spec(
        Field::ExtraterrestrialDirectNormal,
        11,
        9999.0,
        (0.0, BIG),
        true,
    ),
    spec(Field::HorizontalInfrared, 12, 9999.0, (0.0, BIG), true),
    spec(Field::GlobalHorizontal, 13, 9999.0, (0.0, BIG), true),
    spec(Field::DirectNormal, 14, 9999.0, (0.0, BIG), true),
    spec(Field::DiffuseHorizontal, 15, 9999.0, (0.0, BIG), true),
    spec(Field::GlobalIlluminance, 16, 999_900.0, (0.0, BIG), true),
    spec(Field::DirectIlluminance, 17, 999_900.0, (0.0, BIG), true),
    spec(Field::DiffuseIlluminance, 18, 999_900.0, (0.0, BIG), true),
    spec(Field::ZenithLuminance, 19, 9999.0, (0.0, BIG), true),
    spec(Field::WindDirection, 20, 999.0, (0.0, 360.0), false),
    spec(Field::WindSpeed, 21, 999.0, (0.0, 40.0), false),
    spec(Field::TotalSkyCover, 22, 99.0, (0.0, 10.0), false),
    spec(Field::OpaqueSkyCover, 23, 99.0, (0.0, 10.0), false),
    spec(Field::Visibility, 24, 9999.0, (0.0, BIG), false),
    spec(Field::CeilingHeight, 25, 99_999.0, (0.0, BIG), false),
    spec(Field::PrecipitableWater, 28, 999.0, (0.0, BIG), false),
    spec(Field::AerosolOpticalDepth, 29, 0.999, (0.0, BIG), false),
    spec(Field::SnowDepth, 30, 999.0, (0.0, BIG), false),
    spec(Field::DaysSinceSnow, 31, 99.0, (0.0, BIG), false),
    spec(Field::Albedo, 32, 999.0, (0.0, BIG), false),
    spec(Field::PrecipitationDepth, 33, 999.0, (0.0, BIG), true),
    spec(Field::PrecipitationHours, 34, 99.0, (0.0, BIG), true),
];

/// Columns a data line must have: through the aerosol optical depth. Older
/// files stop before the snow, albedo and precipitation columns, which then
/// read as missing.
pub const MIN_COLUMNS: usize = 30;

/// What a raw number means for its field.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Reading {
    /// A valid value.
    Value(f64),
    /// The field's missing marker.
    Missing,
    /// Outside the documented range (not a marker).
    OutOfRange,
}

impl FieldSpec {
    /// Classify one parsed number. The marker test allows a rounding error.
    #[must_use]
    pub fn read(&self, raw: f64) -> Reading {
        if !raw.is_finite() || raw >= self.missing_at - 1.0e-9 {
            Reading::Missing
        } else if raw < self.min || raw > self.max {
            Reading::OutOfRange
        } else {
            Reading::Value(raw)
        }
    }
}
