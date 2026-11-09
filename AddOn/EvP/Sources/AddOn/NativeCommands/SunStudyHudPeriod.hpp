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

} // namespace geomsrv::sunstudyhud::period

namespace geomsrv::sunstudyhud::sunpath {
// Quit/unload: stop the capture worker and join it (SunStudySunPath.cpp).
void Shutdown ();
} // namespace geomsrv::sunstudyhud::sunpath

#endif
