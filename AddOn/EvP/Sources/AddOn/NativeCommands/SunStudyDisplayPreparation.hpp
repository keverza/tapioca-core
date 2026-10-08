#ifndef EVP_NATIVECOMMANDS_SUNSTUDYDISPLAYPREPARATION_HPP
#define EVP_NATIVECOMMANDS_SUNSTUDYDISPLAYPREPARATION_HPP

#include "ArchViz/SunStudyDisplayAssembler.hpp"
#include "NativeCommands/SunStudyFollowerDriver.hpp"

#include <atomic>

namespace geomsrv {

struct PreparedSunStudyDisplay {
    std::unique_ptr<archviz::SunStudyAtlasUpload> upload;
    std::string error;
};

// Worker-only shared producer for manual display and follower display. The
// caller owns publication guards and never waits for the host from this work.
void PrepareSunStudyDisplay (const std::string& id, uint64_t revision, std::shared_ptr<const Snapshot> snapshot,
                             const archviz::SunStudyDisplayOptions& options, PreparedSunStudyDisplay& result,
                             const std::atomic<bool>& cancelled);

struct ManualSunStudyDisplayRequest {
    std::string studyId;
    uint64_t revision = 0, sessionGeneration = 0, captureStamp = 0;
    uint64_t displayGeneration = 0;
    std::shared_ptr<const Snapshot> snapshot;
    archviz::SunStudyDisplayOptions options;
    sunfollow::ActiveSunStudyConfig config;
    bool follow = true;
};

struct ManualSunStudyDisplayState {
    bool preparing = false;
    std::string studyId, error;
};

bool SubmitManualSunStudyDisplay (ManualSunStudyDisplayRequest request, std::string& error);
ManualSunStudyDisplayState ManualSunStudyDisplayStatus ();
void CancelManualSunStudyDisplays ();
void ShutdownManualSunStudyDisplays ();

} // namespace geomsrv
#endif
