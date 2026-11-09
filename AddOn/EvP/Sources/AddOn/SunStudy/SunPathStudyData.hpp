#ifndef TAPIOCA_SUNPATHSTUDYDATA_HPP
#define TAPIOCA_SUNPATHSTUDYDATA_HPP

// Immutable display data for #179. No solar calculation, host or UI types.
// Geometry identity is independent of the latest successfully applied frame.
#include "SunStudy/SunStudyPointProfile.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv::sunstudyhud {

struct SunPathStudyDay {
    bool available = false;
    std::string studyId;
    uint64_t revision = 0;
    int year = 0, month = 0, day = 0;
    int hourFrom = 0, hourTo = 24;
    int timestepMinutes = 0;
    std::vector<evp::sunstudy::SunStep> steps;
};

struct SunPathPeriodGeometry {
    std::string studyId;
    uint64_t revision = 0;
    int year = 0;
    bool estimate = false;
    std::vector<evp::sunstudy::ProfileDay> days;
};

struct SunPathAppliedFrame {
    // 0 aggregate month/period, 1 kept instant, 2 kept day total.
    int frame = 0, month = 0, day = 0, step = 0;
};

struct SunPathPeriodData {
    std::shared_ptr<const SunPathPeriodGeometry> geometry;
    SunPathAppliedFrame applied;
};

bool ReadSunPathStudyDay (const std::string& studyId, SunPathStudyDay& day);
std::shared_ptr<const SunPathPeriodData> LatestSunPathPeriod ();

} // namespace geomsrv::sunstudyhud

#endif
