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
#include "SunStudy/SunStudyOccluders.hpp"
#include "SunStudy/SunStudyReceptor.hpp"

#include <cstdint>
#include <functional>
#include <memory>
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
    // Planting receptor layer (#278): triangle-domain surfaces only. The
    // occluders answer the overhead test before the record holds them.
    bool receptorRequested = false;
    evp::sunstudy::ReceptorOptions receptor;
    std::shared_ptr<const evp::sunstudy::SunStudyOccluders> occluders;
};

// The receptor layer as the response reports it, read before the record moves
// into the store. `requested` false for an ordinary study.
struct SunStudyReceptorReport {
    bool requested = false;
    double height = 0.0;
    size_t count = 0, available = 0, steepHost = 0, blocked = 0, invalidHost = 0;
    double availableArea = 0.0, unavailableArea = 0.0; // m2, plan or surface per the request
};
SunStudyReceptorReport SunStudyReceptorReportOf (const evp::sunstudy::StudyRecord& record);

// Reads receptorHeight / receptorNormal / receptorArea / receptorMinHostUp.
// `requested` is false when receptorHeight is absent (the others are then
// refused rather than ignored). False, with `error`, for an invalid value.
bool ReadSunStudyReceptor (const GS::ObjectState& params, bool& requested, evp::sunstudy::ReceptorOptions& options,
                           GS::UniString& error);

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
