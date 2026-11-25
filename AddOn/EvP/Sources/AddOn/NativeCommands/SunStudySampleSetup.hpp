#ifndef EVP_NATIVECOMMANDS_SUNSTUDYSAMPLESETUP_HPP
#define EVP_NATIVECOMMANDS_SUNSTUDYSAMPLESETUP_HPP

// NativeCommands/SunStudySampleSetup -- "where do we ask?" for Tapioca.StartSunStudy.
//
// The three sample sources (explicit points, the model's own surfaces in the
// triangle or patch domain, the padded ground grid), the display atlas each
// one needs, and the grid version the core's sync keys on. Extracted from
// SunStudyCommands.cpp unchanged in behaviour; the command keeps the roles,
// the occluders, the record's identity and the response.

#include "NativeCommands/NativeCommandResult.hpp"
#include "NativeCommands/SunStudyPreparation.hpp"
#include "SunStudy/SunStudyBounds.hpp"
#include "SunStudy/SunStudyLimits.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace geomsrv {

struct SunStudySampleRequest {
    std::string sampleMode; // as asked, for the refusal text
    std::string domainName;
    bool sampleSurfaces = false;
    bool sampleExplicit = false;
    bool patchDomain = false;
    double spacing = 2.0;
    evp::sunstudy::AnalysisCylinder clip;
    evp::sunstudy::AnalysisLimits limits;
    const std::vector<uint8_t>* sampleMask = nullptr;             // per element
    const std::vector<std::vector<uint8_t>>* faceMasks = nullptr; // per mesh receiver faces
    const evp::sunstudy::StudyRecord* reuseSource = nullptr;
};

// What the response and the core's inputs need from the sampling.
struct SunStudySampleSetup {
    size_t columns = 0;
    size_t rows = 0;
    double groundZ = 0.0;
    double reportedPad = 0.0;
    uint64_t gridVersion = 0;
    size_t undersizedFaces = 0;
    size_t degenerateFaces = 0;
    size_t closedGroups = 0;
    size_t flippedGroups = 0;
    uint32_t atlasWidth = 0;
    uint32_t atlasHeight = 0;
    size_t atlasFaces = 0;
    size_t patchCount = 0;
    size_t excludedSurfaces = 0; // context faces (triangle) or surfaces (patch)
};

// Fills the record's samples, sampling layout and atlas. A failed result is
// the command's refusal, verbatim.
NativeCommandResult PrepareSunStudySamples (const GS::ObjectState& params, const Snapshot& snapshot,
                                            const SunStudySampleRequest& request, evp::sunstudy::StudyRecord& record,
                                            SunStudyPreparationTrace& trace, const std::function<bool ()>& isCancelled,
                                            SunStudySampleSetup& out);

} // namespace geomsrv

#endif
