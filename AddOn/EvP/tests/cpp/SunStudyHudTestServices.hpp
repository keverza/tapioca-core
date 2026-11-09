#ifndef TAPIOCA_TESTS_SUNSTUDYHUDTESTSERVICES_HPP
#define TAPIOCA_TESTS_SUNSTUDYHUDTESTSERVICES_HPP

// Controlled runner/capture boundaries shared by the offline production-HUD
// tests. Widgets, hover routing and the path renderer themselves are not faked.
#include "NativeCommands/SunStudyHudRunner.hpp"
#include "SunStudy/SunPathStudyData.hpp"

#include <array>

namespace tapioca::test {

void ResetSunStudyHudServices ();
geomsrv::sunstudyhud::PeriodInfo& SunStudyTestPeriod ();
geomsrv::sunstudyhud::PeriodProfile& SunStudyTestProfile ();
void SetSunStudyTestKeptDays (std::shared_ptr<const std::vector<geomsrv::sunstudyhud::KeptDay>> days);
int SunStudyTestViewRequests ();
const geomsrv::sunstudyhud::PeriodView& SunStudyTestRequestedView ();
int SunStudyTestSubmissions ();

void SetSunPathTestCapture (std::shared_ptr<geomsrv::sunstudyhud::SunPath> path);
void SetSunPathTestPeriod (std::shared_ptr<const geomsrv::sunstudyhud::SunPathPeriodData> period);
const std::vector<std::array<int, 3>>& SunPathTestCaptureDates ();

} // namespace tapioca::test

#endif
