// A build without the Rust archive (TAPIOCA_LINK_RUST_GEOMETRY=OFF) has no
// sun-study core (P7a). It still compiles; a study is refused with the message
// StartSunStudy reports, every store reader refuses a record, and the follower
// policy the driver holds never adopts anything. With the archive this file
// has no tests: the store and follower tests run the real core.

#include "SunStudy/SunStudyFollowerPolicy.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#if !defined(TAPIOCA_GEOMETRY_RUST)

using namespace evp::sunstudy;

TEST (SunStudyNoCore, AStudyIsRefusedWithTheBuildsReason)
{
    StudyRecord record;
    record.positions = { 0.0, 0.0, 0.0 };
    record.normals = { 0.0, 0.0, 1.0 };
    const geomsrv::Snapshot snapshot;
    std::string error;
    EXPECT_FALSE (AttachRustStudyCore (record, snapshot, nullptr, 4096, error));
    EXPECT_EQ (error, "this build has no sun-study core");
    EXPECT_EQ (record.rustCore, nullptr);
    EXPECT_TRUE (record.CurrentProgress ().empty);

    auto& store = SunStudyStore::Get ();
    store.Clear ();
    const std::string id = store.Insert (std::make_unique<StudyRecord> ());
    ASSERT_FALSE (id.empty ());
    std::vector<double> hours, positions, normals;
    EXPECT_FALSE (store.Results (id, hours, positions, normals, nullptr, error));
    EXPECT_EQ (error, "this build has no sun-study core");
    store.Clear ();
}

TEST (SunStudyNoCore, TheFollowerPolicyNeverAdopts)
{
    SunStudyFollowerPolicy follower;
    follower.Adopt ("study", { 1, 2, 3 }, 1000);
    EXPECT_EQ (follower.State (), SunStudyFollowState::NoStudy);
    EXPECT_FALSE (follower.HasRenderableCache ());
    EXPECT_FALSE (follower.Observe ({ 2, 2, 3 }, 1100));
    EXPECT_FALSE (follower.ShouldStart (5000));
    EXPECT_EQ (follower.Describe (), "this build has no sun-study core");
}

#endif
