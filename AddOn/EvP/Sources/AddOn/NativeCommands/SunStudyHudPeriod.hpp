#ifndef EVP_NATIVECOMMANDS_SUNSTUDYHUDPERIOD_HPP
#define EVP_NATIVECOMMANDS_SUNSTUDYHUDPERIOD_HPP

// The runner's side of the retained period (SunStudyHudPeriod.cpp): what the
// period run hands over, and what Submit/Shutdown need. Not for the HUD: the
// HUD's calls are the plain-value ones in SunStudyHudRunner.hpp.

#include "SunStudy/SunStudyPointProfile.hpp"
#include "SunStudy/SunStudyStore.hpp"

#if defined(TAPIOCA_GEOMETRY_RUST)
#include "SunStudy/SunStudyRustAnnual.hpp"
#endif

#include <memory>
#include <string>
#include <vector>

namespace geomsrv::sunstudyhud::period {

#if defined(TAPIOCA_GEOMETRY_RUST)
// A finished period: its totals, its carrier day and every traced day's sun.
void Retain (std::unique_ptr<evp::sunstudy::rust::RustAnnualSun> annual,
             std::shared_ptr<const evp::sunstudy::StudyRecord> carrier, const std::string& carrierId,
             uint64_t carrierRevision, std::vector<evp::sunstudy::ProfileDay> days,
             const evp::sunstudy::rust::AnnualInfo& info, const std::string& label, bool estimate);
#endif
// A view or a profile is being prepared through the carrier.
bool WorkerBusy ();
void Release ();
// Atomically refuse an active worker or release the period and its carrier.
bool ReleaseIfIdle ();
// An accepted owning-run cancellation invalidates display publication at once;
// the worker's ordinary finish path still retires the retained analysis safely.
void InvalidateSunPath ();
// Quit/unload: drop pending requests, join the worker, release.
void Shutdown ();
// The kept period's mean direct-sun hours a day per sample, whole period
// (#278 area table). False when `carrierId` is not the kept period's carrier.
bool MeanDailyHours (const std::string& carrierId, std::vector<double>& hours, std::string& error);

} // namespace geomsrv::sunstudyhud::period

namespace geomsrv::sunstudyhud::plantinglayers {
// The runner's side of the kept planting layers (SunStudyHudPlantingLayers.cpp;
// the HUD's calls are in SunStudyHudRunner.hpp). Keep after a planting run is
// done; the oldest beyond kPlantingLayersKept is erased from the store.
// `hoursMax` is the display ceiling the study was shown with (0: its day).
void Keep (const std::string& studyId, uint64_t revision, double hoursMax);
bool Find (const std::string& studyId, uint64_t& revision, double& hoursMax);
} // namespace geomsrv::sunstudyhud::plantinglayers

namespace geomsrv::sunstudyhud::sunpath {
// Quit/unload: stop the capture worker and join it (SunStudySunPath.cpp).
void Shutdown ();
} // namespace geomsrv::sunstudyhud::sunpath

#endif
