#include "SunStudy/SunSeries.hpp"

#include <algorithm>
#include <cmath>

namespace evp::sunstudy {

namespace {

// FNV-1a over the bytes of a double, so the hash covers the value exactly
// rather than a rounded rendering of it.
void HashBytes (uint64_t& hash, const void* data, size_t bytes)
{
    const unsigned char* p = static_cast<const unsigned char*> (data);
    for (size_t i = 0; i < bytes; ++i) {
        hash ^= p[i];
        hash *= 1099511628211ull;
    }
}

void HashDouble (uint64_t& hash, double value)
{
    // ⚠️ NORMALISE NEGATIVE ZERO. -0.0 and 0.0 compare equal but have different
    // bytes, so a sun vector that differs only in the sign of a zero component
    // would otherwise read as a different day and discard a valid cache.
    if (value == 0.0)
        value = 0.0;
    HashBytes (hash, &value, sizeof (value));
}

void HashInt (uint64_t& hash, int64_t value)
{
    HashBytes (hash, &value, sizeof (value));
}

} // namespace

std::vector<TimeOfDay> EnumerateTimesteps (int timestepMinutes, int hourFrom, int hourTo)
{
    std::vector<TimeOfDay> steps;

    const int lo = std::max (0, hourFrom) * 60;
    const int hi = std::min (24, hourTo) * 60;
    const int step = std::max (1, timestepMinutes);

    for (int total = lo; total < hi; total += step)
        steps.push_back (TimeOfDay { total / 60, total % 60 });

    return steps;
}

double HoursPerStep (int timestepMinutes)
{
    return static_cast<double> (timestepMinutes) / 60.0;
}

SunSeries SunSeries::FromSteps (const std::vector<SunStep>& steps, int timestepMinutes, double minAltitudeDegrees)
{
    SunSeries series;
    series.timestepMinutes_ = std::max (1, timestepMinutes);
    series.sourceStepCount_ = steps.size ();

    for (const SunStep& step : steps) {
        if (!(step.altitudeDegrees > minAltitudeDegrees))
            continue;

        // ⚠️ A ZERO-LENGTH DIRECTION IS DROPPED EVEN ABOVE THE HORIZON. The host
        // reports a zero vector for a sun it could not resolve, and normalising
        // one yields NaN — which propagates through the frustum fit and the dot
        // products and turns a whole timestep's result into silence rather than
        // an error.
        const double lengthSquared = step.direction[0] * step.direction[0] + step.direction[1] * step.direction[1] +
                                     step.direction[2] * step.direction[2];
        if (lengthSquared <= 0.0)
            continue;

        SunStep normalised = step;
        const double length = std::sqrt (lengthSquared);
        normalised.direction[0] /= length;
        normalised.direction[1] /= length;
        normalised.direction[2] /= length;
        series.steps_.push_back (normalised);
    }

    if (series.steps_.empty ())
        return series; // version stays 0: "no sun above the horizon"

    uint64_t hash = 14695981039346656037ull; // FNV-1a offset basis
    HashInt (hash, series.timestepMinutes_);
    HashInt (hash, static_cast<int64_t> (series.steps_.size ()));
    for (const SunStep& step : series.steps_) {
        HashInt (hash, step.time.hour);
        HashInt (hash, step.time.minute);
        HashDouble (hash, step.direction[0]);
        HashDouble (hash, step.direction[1]);
        HashDouble (hash, step.direction[2]);
    }

    // Never zero for a non-empty series: zero is reserved for "empty".
    series.version_ = (hash == 0) ? 1u : hash;
    return series;
}

double SunSeries::HoursPerStep () const
{
    return evp::sunstudy::HoursPerStep (timestepMinutes_);
}

double SunSeries::DaylightHours () const
{
    return static_cast<double> (steps_.size ()) * HoursPerStep ();
}

} // namespace evp::sunstudy
