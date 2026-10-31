#ifndef EVP_TESTS_SUNSTUDY_CORE_FIXTURE_HPP
#define EVP_TESTS_SUNSTUDY_CORE_FIXTURE_HPP

// Attach the study core to a test record and sync it, as the start command
// does (StartSunStudyCore). The Rust core is the only one a study runs on, so
// every test that builds a StudyRecord by hand goes through here. A ground
// record (no sample grid) needs no snapshot.

#include "SunStudy/SunStudyAtlasReuse.hpp"
#include "SunStudy/SunStudyRustStudy.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <gtest/gtest.h>

#include <string>

namespace evp::sunstudy::fixture {

// The display atlas the start path builds beside the samples. The core is
// refused unless its own atlas matches it, so a record with a sample grid
// needs this before AttachTestCore.
inline void BuildTestAtlas (StudyRecord& record, uint32_t maxDimension = 8192)
{
    if (record.IsPatchDomain ()) {
        record.patchAtlas = SunStudyPatchAtlas ();
        record.patchAtlas.Fit (record.patchGrid, 1, maxDimension);
    }
    else if (record.sampleGrid.Count () != 0 && record.snapshot != nullptr)
        record.atlas = BuildStableTriangleAtlas (record.sampleGrid, *record.snapshot, record.triangleAllocations);
}

inline bool AttachTestCore (StudyRecord& record, const StudyInputs& inputs, const geomsrv::Snapshot* snapshot = nullptr,
                            const StudyRecord* previous = nullptr, uint32_t maxAtlasDimension = 4096)
{
    static const geomsrv::Snapshot none;
    std::string error;
    const bool attached =
        AttachRustStudyCore (record, snapshot != nullptr ? *snapshot : none, previous, maxAtlasDimension, error);
    EXPECT_TRUE (attached) << error;
    SyncStudy (record, inputs);
    return attached;
}

// Advance the record's core directly, outside the store, by up to `steps`.
inline size_t AdvanceTestCore (const StudyRecord& record, size_t steps, size_t maxParallel = 0)
{
    size_t advanced = 0;
    std::string error;
    EXPECT_NE (record.rustCore, nullptr);
    if (record.rustCore != nullptr)
        EXPECT_TRUE (
            record.rustCore->Advance (*record.traversal, steps, 0.001, 0.0, maxParallel, {}, {}, advanced, error))
            << error;
    return advanced;
}

} // namespace evp::sunstudy::fixture

#endif
