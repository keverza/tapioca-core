#ifndef EVP_NATIVECOMMANDS_SUNSTUDYFOLLOWERSIGNATURE_HPP
#define EVP_NATIVECOMMANDS_SUNSTUDYFOLLOWERSIGNATURE_HPP

// NativeCommands/SunStudyFollowerSignature -- what the follower compares to decide
// that a study is stale: geometry, sun and sampling, as the world stands. Moved
// out of SunStudyFollowerDriver.cpp unchanged (the driver passed its size cap).

#include "NativeCommands/SunStudyFollowerDriver.hpp" // ActiveSunStudyConfig
#include "SunStudy/SunStudyFollower.hpp"             // SunStudyDependencySignature

namespace geomsrv {
namespace sunfollow {

// geometry + sun + sampling, as the world stands. MAIN THREAD (it reads ACAPI's
// place settings).
evp::sunstudy::SunStudyDependencySignature BuildCurrentSignature (const ActiveSunStudyConfig& config);

} // namespace sunfollow
} // namespace geomsrv

#endif
