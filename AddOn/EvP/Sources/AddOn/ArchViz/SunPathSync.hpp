#ifndef TAPIOCA_ARCHVIZ_SUNPATHSYNC_HPP
#define TAPIOCA_ARCHVIZ_SUNPATHSYNC_HPP

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/SunPathCompass.hpp"

#include <imgui.h>

namespace geomsrv::archviz {

inline void PublishSunPathHover (HudState& state, const SunPathMoment& moment)
{
    state.sunPathHover = moment;
    state.sunPathHoverFrame = ImGui::GetFrameCount ();
}

inline SunPathMoment SunPathPeriodDayMoment (const HudState& state, int month, int day, int minute)
{
    if (state.sunPathPeriod == nullptr || state.sunPathPeriod->geometry == nullptr)
        return {};
    const auto& geometry = *state.sunPathPeriod->geometry;
    for (const auto& traced : geometry.days)
        if (traced.month == month && traced.day == day)
            return SunPathNearestMoment (geometry.year, month, day, traced.steps, minute, minute, minute + 1);
    return {};
}

inline std::string SunPathHoverMonth (HudState& state, int month, int minute, int from = 0, int to = 1440)
{
    if (state.sunPathPeriod == nullptr || state.sunPathPeriod->geometry == nullptr)
        return {};
    const auto moment = SunPathMonthMoment (*state.sunPathPeriod->geometry, month, minute, from, to);
    PublishSunPathHover (state, moment);
    return moment.valid ? "\nRepresentative " + SunPathMomentCaption (moment.year, month, moment.day, moment.minute)
                        : "\nNo traced sun in this selection";
}

inline void SunPathHoverPeriodDay (HudState& state, int month, int day, int minute)
{
    if (state.sunPathPeriod == nullptr || state.sunPathPeriod->geometry == nullptr)
        return;
    for (const auto& traced : state.sunPathPeriod->geometry->days)
        if (traced.month == month && traced.day == day) {
            PublishSunPathHover (
                state, SunPathNearestMoment (state.sunPathPeriod->geometry->year, month, day, traced.steps, minute));
            break;
        }
}

struct SunPathPointControl {
    int day, step, mode, clock;
    bool playing;
};

inline SunPathPointControl SunPathPointControlOf (const HudState& state)
{
    return { state.sunPointPlayDay, state.sunPointPlayStep, state.sunPointPlayMode, state.sunPointPlayClock,
             state.sunPointPlay.playing };
}

template <class Profile> bool SunPathProfileMatches (const HudState& state, const Profile& profile)
{
    return profile.ready && profile.error.empty () && state.sunPathPeriod != nullptr &&
           state.sunPathPeriod->geometry != nullptr && profile.studyId == state.sunPathPeriod->geometry->studyId &&
           profile.revision == state.sunPathPeriod->geometry->revision && profile.samples == state.sunPointSamples;
}

template <class Profile>
void PublishSunPathPointPlayback (HudState& state, const Profile& profile, int hover, SunPathPointControl before,
                                  bool interacted = false)
{
    if (!SunPathProfileMatches (state, profile))
        return;
    const int d = state.sunPointPlayDay, s = state.sunPointPlayStep;
    if (d < 0 || size_t (d) >= profile.dayMonth.size () || size_t (d) >= profile.dayOfMonth.size () ||
        size_t (d) >= profile.dayMinutes.size ())
        return;
    const auto& minutes = profile.dayMinutes[size_t (d)];
    state.sunPathPointFrame = ImGui::GetFrameCount ();
    state.sunPathPointMoment = s >= 0 && size_t (s) < minutes.size ()
                                   ? SunPathPeriodDayMoment (state, profile.dayMonth[size_t (d)],
                                                             profile.dayOfMonth[size_t (d)], minutes[size_t (s)])
                                   : SunPathMoment {};
    if (hover >= 0 && size_t (hover) < minutes.size ())
        PublishSunPathHover (state, SunPathPeriodDayMoment (state, profile.dayMonth[size_t (d)],
                                                            profile.dayOfMonth[size_t (d)], minutes[size_t (hover)]));
    const bool changed = before.day != d || before.step != s || before.mode != state.sunPointPlayMode ||
                         before.clock != state.sunPointPlayClock;
    if (before.playing != state.sunPointPlay.playing ||
        (interacted && (changed || ImGui::IsMouseDown (0) || ImGui::IsMouseReleased (0))))
        state.sunPathSyncSource = SunPathSyncSource::PointPlayer;
}

inline void PublishSunPathMapPlayback (HudState& state, bool changed, bool wasPlaying, bool interacted = false)
{
    state.sunPathMapFrame = ImGui::GetFrameCount ();
    if (wasPlaying != state.sunPeriodPlay.playing ||
        (interacted && (changed || ImGui::IsMouseDown (0) || ImGui::IsMouseReleased (0))))
        state.sunPathSyncSource = SunPathSyncSource::MapPlayer;
}

inline void PublishSunPathDayHover (HudState& state, const evp::sunstudy::PointDay* day, int hover)
{
    if (day == nullptr || hover < 0 || size_t (hover) >= day->minutes.size () || !state.sunPathDay.available)
        return;
    const int minute = day->minutes[size_t (hover)];
    const auto& exact = state.sunPathDay;
    PublishSunPathHover (
        state, SunPathNearestMoment (exact.year, exact.month, exact.day, exact.steps, minute, minute, minute + 1));
}

} // namespace geomsrv::archviz

#endif
