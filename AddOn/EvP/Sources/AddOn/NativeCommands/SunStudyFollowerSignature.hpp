#ifndef EVP_NATIVECOMMANDS_SUNSTUDYFOLLOWERSIGNATURE_HPP
#define EVP_NATIVECOMMANDS_SUNSTUDYFOLLOWERSIGNATURE_HPP

// NativeCommands/SunStudyFollowerSignature -- the follower's configuration as the
// world sees it: what it compares to decide that a study is stale (geometry, sun
// and sampling), and the StartSunStudy parameters a rerun sends. Both moved out
// of SunStudyFollowerDriver.cpp unchanged (the driver reached its size cap).

#include "NativeCommands/SunStudyFollowerDriver.hpp" // ActiveSunStudyConfig
#include "SunStudy/SunStudyFollowState.hpp"          // SunStudyDependencySignature

namespace geomsrv {
namespace sunfollow {

// geometry + sun + sampling, as the world stands. MAIN THREAD (it reads ACAPI's
// place settings).
evp::sunstudy::SunStudyDependencySignature BuildCurrentSignature (const ActiveSunStudyConfig& config);

// The StartSunStudy parameters that rerun `config`: every measurement input,
// the roles on every rerun, surface samples only (the display needs the atlas).
GS::ObjectState StartParams (const ActiveSunStudyConfig& config);

} // namespace sunfollow
} // namespace geomsrv

#endif
