#ifndef EVP_ARCHVIZ_SUNPATHPICK_HPP
#define EVP_ARCHVIZ_SUNPATHPICK_HPP

// ArchViz/SunPathPick -- the draggable sun's date and time (#152 follow-up).
//
// The sun path's lattice (sunstudyhud::SunPath::grid) holds Archicad's own sun
// positions at every day and every few minutes of the year. A drag picks the
// lattice point nearest the cursor on screen; this file turns that pick into a
// date and a clock time and snaps it to the sun's key positions -- the
// solstices and equinoxes by date, solar noon by time -- so they are easy to
// land on. Pure: no ImGui, no host, no projection.

#include "NativeCommands/SunStudyHudRunner.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace geomsrv::archviz {

// Day of a 365-day year <-> month and day. February has 28 days: a leap year's
// 29th is not a lattice day.
inline void SunPathDateOf (int ordinal, int& month, int& day)
{
    static constexpr int kDays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int left = ordinal < 1 ? 1 : (ordinal > 365 ? 365 : ordinal);
    month = 1;
    while (left > kDays[month - 1]) {
        left -= kDays[month - 1];
        ++month;
    }
    day = left;
}

inline int SunPathOrdinalOf (int month, int day)
{
    static constexpr int kDays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    month = month < 1 ? 1 : (month > 12 ? 12 : month);
    int ordinal = 0;
    for (int m = 1; m < month; ++m)
        ordinal += kDays[m - 1];
    const int last = kDays[month - 1];
    return ordinal + (day < 1 ? 1 : (day > last ? last : day));
}

// The key dates a drag snaps to, as the sun path's own arcs draw them.
struct SunPathKeyDate {
    int month, day;
    const char* name;
};
inline constexpr SunPathKeyDate kSunPathKeyDates[] = {
    { 3, 20, "March equinox" },
    { 6, 21, "June solstice" },
    { 9, 22, "September equinox" },
    { 12, 21, "December solstice" },
};
constexpr int kSunPathDateSnapDays = 4;     // within this many days, the key date
constexpr int kSunPathNoonSnapMinutes = 30; // within this, the day's highest lattice sun

struct SunPathPick {
    bool valid = false;
    int ordinal = 0, minute = 0;
    int month = 0, day = 0;
    float direction[3] = { 0.0f, 0.0f, 1.0f };
    const char* snappedDate = nullptr; // the key date's name, or null
    bool snappedNoon = false;
};

// Snaps a lattice pick (ordinal, minute) and returns the lattice point for the
// snapped date and time, with its direction. Invalid when the lattice has no
// point that day near that time (the sun is down there).
inline SunPathPick SnapSunPathPick (const std::vector<sunstudyhud::SunPathGridPoint>& grid, int ordinal, int minute)
{
    SunPathPick pick;
    for (const SunPathKeyDate& key : kSunPathKeyDates) {
        const int keyOrdinal = SunPathOrdinalOf (key.month, key.day);
        if (std::abs (ordinal - keyOrdinal) <= kSunPathDateSnapDays) {
            ordinal = keyOrdinal;
            pick.snappedDate = key.name;
            break;
        }
    }
    // Solar noon: the snapped day's highest lattice sun.
    const sunstudyhud::SunPathGridPoint* noon = nullptr;
    for (const auto& point : grid)
        if (point.ordinal == ordinal && (noon == nullptr || point.direction[2] > noon->direction[2]))
            noon = &point;
    if (noon != nullptr && std::abs (int (noon->minute) - minute) <= kSunPathNoonSnapMinutes) {
        minute = noon->minute;
        pick.snappedNoon = true;
    }
    const sunstudyhud::SunPathGridPoint* best = nullptr;
    int bestGap = sunstudyhud::kSunPathGridMinutes;
    for (const auto& point : grid)
        if (point.ordinal == ordinal)
            if (const int gap = std::abs (int (point.minute) - minute); gap <= bestGap) {
                bestGap = gap;
                best = &point;
            }
    if (best == nullptr)
        return pick;
    pick.valid = true;
    pick.ordinal = ordinal;
    pick.minute = best->minute;
    SunPathDateOf (ordinal, pick.month, pick.day);
    for (int axis = 0; axis < 3; ++axis)
        pick.direction[axis] = best->direction[axis];
    return pick;
}

// The lattice point at a date and clock time, for a time set elsewhere (the
// shadow slider) on the dragged date. Null when the sun is down then.
inline const sunstudyhud::SunPathGridPoint* SunPathGridAt (const std::vector<sunstudyhud::SunPathGridPoint>& grid,
                                                           int ordinal, int minute)
{
    const sunstudyhud::SunPathGridPoint* best = nullptr;
    int bestGap = sunstudyhud::kSunPathGridMinutes / 2;
    for (const auto& point : grid)
        if (point.ordinal == ordinal)
            if (const int gap = std::abs (int (point.minute) - minute); gap <= bestGap) {
                bestGap = gap;
                best = &point;
            }
    return best;
}

// The viewer's sun override from a direction: model azimuth CCW from +X and
// altitude, both in degrees (DiligentScene::SetSunOverride's convention).
inline void SunPathAngles (const float direction[3], float& azimuthDegrees, float& altitudeDegrees)
{
    constexpr double kDegrees = 57.295779513082320876;
    azimuthDegrees = float (std::atan2 (double (direction[1]), double (direction[0])) * kDegrees);
    const double z = direction[2] < -1.0f ? -1.0 : (direction[2] > 1.0f ? 1.0 : double (direction[2]));
    altitudeDegrees = float (std::asin (z) * kDegrees);
}

} // namespace geomsrv::archviz

#endif
