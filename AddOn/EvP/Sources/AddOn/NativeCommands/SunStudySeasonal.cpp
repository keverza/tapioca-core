#include "NativeCommands/SunStudySeasonal.hpp"

#include <map>

namespace geomsrv::sunstudyhud {

std::array<SeasonalPointDay, 4> SeasonalPointDays (const PeriodProfile& profile)
{
    std::array<SeasonalPointDay, 4> seasons;
    const char* labels[] = { "DJF", "MAM", "JJA", "SON" };
    struct StepTotal {
        double lit = 0.0, facing = 0.0, weight = 0.0;
    };
    std::array<std::map<uint16_t, StepTotal>, 4> steps;
    std::array<double, 4> direct {}, facing {}, up {};
    for (size_t season = 0; season < seasons.size (); ++season)
        seasons[season].label = labels[season];
    for (size_t dayIndex = 0; dayIndex < profile.dayMonth.size (); ++dayIndex) {
        const int month = profile.dayMonth[dayIndex];
        if (month < 1 || month > 12)
            continue;
        const size_t season = size_t ((month / 3) % 4); // Dec -> DJF, Jan/Feb -> DJF
        const double weight = dayIndex < profile.dayWeights.size () ? profile.dayWeights[dayIndex] : 1.0;
        auto& target = seasons[season].day;
        ++seasons[season].tracedDays;
        seasons[season].representedDays += weight;
        if (dayIndex < profile.dayLit.size ())
            direct[season] += profile.dayLit[dayIndex] * weight;
        if (dayIndex < profile.dayFacing.size ())
            facing[season] += profile.dayFacing[dayIndex] * weight;
        if (dayIndex < profile.dayUp.size ())
            up[season] += profile.dayUp[dayIndex] * weight;
        target.samples = profile.samples.size ();
        if (dayIndex < profile.dayHoursPerStep.size ())
            target.hoursPerStep = profile.dayHoursPerStep[dayIndex];
        if (dayIndex >= profile.dayMinutes.size ())
            continue;
        for (size_t step = 0; step < profile.dayMinutes[dayIndex].size (); ++step) {
            const uint16_t minute = profile.dayMinutes[dayIndex][step];
            StepTotal& total = steps[season][minute];
            if (dayIndex < profile.dayStepLit.size () && step < profile.dayStepLit[dayIndex].size ())
                total.lit += profile.dayStepLit[dayIndex][step] * weight;
            if (dayIndex < profile.dayStepFacing.size () && step < profile.dayStepFacing[dayIndex].size ())
                total.facing += profile.dayStepFacing[dayIndex][step] * weight;
            total.weight += weight;
        }
    }
    // Southern hemisphere when the traced DJF days had more sun up than JJA.
    const auto meanUp = [&] (size_t season) {
        return seasons[season].representedDays > 0.0 ? up[season] / seasons[season].representedDays : 0.0;
    };
    const bool southern = seasons[0].representedDays > 0.0 && seasons[2].representedDays > 0.0 && meanUp (0) > meanUp (2);
    const char* northNames[] = { "Winter", "Spring", "Summer", "Autumn" };
    const char* southNames[] = { "Summer", "Autumn", "Winter", "Spring" };
    const char* months[] = { "Dec-Feb", "Mar-May", "Jun-Aug", "Sep-Nov" };
    for (size_t season = 0; season < seasons.size (); ++season) {
        seasons[season].name = southern ? southNames[season] : northNames[season];
        seasons[season].months = months[season];
        auto& day = seasons[season].day;
        const double represented = seasons[season].representedDays;
        day.hasTotalHours = true;
        day.directHours = represented > 0.0 ? direct[season] / represented : 0.0;
        day.facingHours = represented > 0.0 ? facing[season] / represented : 0.0;
        day.sunUpHours = represented > 0.0 ? up[season] / represented : 0.0;
        for (const auto& [minute, total] : steps[season]) {
            day.minutes.push_back (minute);
            day.lit.push_back (total.weight > 0.0 ? total.lit / total.weight : 0.0);
            day.facing.push_back (total.weight > 0.0 ? total.facing / total.weight : 0.0);
        }
    }
    return seasons;
}

} // namespace geomsrv::sunstudyhud
