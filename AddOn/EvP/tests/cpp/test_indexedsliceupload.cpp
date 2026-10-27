#include "ArchViz/SceneCmdQueue.hpp"
#include "ArchViz/StorySliceGeometry.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <limits>

namespace av = geomsrv::archviz;
namespace {
av::SliceChain Square (double x = 0, double y = 0)
{
    return { { x, y, x + 1, y, x + 1, y + 1, x, y + 1 }, true };
}
} // namespace

TEST (IndexedSliceUpload, StoreysAppendActualIndicesAndMeasureBeforeWorldFloatPacking)
{
    std::vector<av::StorySliceFillVertex> vertices;
    std::vector<uint32_t> indices;
    std::string error;
    double area = -1;
    ASSERT_TRUE (av::AppendIndexedSliceFill ({ Square () }, 3.25, vertices, indices, area, error)) << error;
    EXPECT_EQ (vertices.size (), 4u);
    EXPECT_EQ (indices.size (), 6u);
    EXPECT_EQ (area, 1);
    const size_t first = indices.size ();
    av::SliceChain survey { { 600000, 6500000, 600001, 6500000, 600001, 6500000.125, 600000, 6500000.125 }, true };
    ASSERT_TRUE (av::AppendIndexedSliceFill ({ survey }, 6.75, vertices, indices, area, error)) << error;
    EXPECT_EQ (area, 0.125) << "GPU floats collapse this Y feature; measurement must not use them";
    EXPECT_EQ (vertices.size (), 8u);
    EXPECT_EQ (indices.size (), 12u);
    for (size_t i = first; i < indices.size (); ++i) {
        EXPECT_GE (indices[i], 4u);
        EXPECT_LT (indices[i], 8u);
        EXPECT_FLOAT_EQ (vertices[indices[i]].z, 6.75f);
    }
    double packedMin = 1e10, packedMax = -1e10;
    for (size_t i = 4; i < vertices.size (); ++i) {
        packedMin = (std::min) (packedMin, double (vertices[i].y));
        packedMax = (std::max) (packedMax, double (vertices[i].y));
    }
    EXPECT_EQ (packedMin, packedMax) << "existing world-float GPU precision is an explicit retained limitation";
}

TEST (IndexedSliceUpload, RefusalIsAtomicAndEmptyDomainsDoNotInventGeometry)
{
    std::vector<av::StorySliceFillVertex> vertices { { 1, 2, 3 } };
    std::vector<uint32_t> indices { 0 };
    std::string error;
    double area = 123;
    auto malformed = Square ();
    malformed.xy.push_back (1);
    EXPECT_FALSE (av::AppendIndexedSliceFill ({ malformed }, 0, vertices, indices, area, error));
    EXPECT_FALSE (av::AppendIndexedSliceFill ({ Square () }, 1e100, vertices, indices, area, error));
    EXPECT_EQ (area, 123);
    EXPECT_EQ (vertices.size (), 1u);
    EXPECT_EQ (indices, (std::vector<uint32_t> { 0 }));
    EXPECT_TRUE (av::AppendIndexedSliceFill ({ Square (), Square () }, 0, vertices, indices, area, error));
    EXPECT_EQ (area, 0);
    EXPECT_EQ (vertices.size (), 1u);
    EXPECT_EQ (indices.size (), 1u);
}

TEST (IndexedSliceUpload, SceneQueueCarriesSharedIndicesAndAccountsTheirCapacity)
{
    struct Consumer {
        av::SceneCmdQueue& queue = av::SceneCmdQueue::Get ();
        Consumer ()
        {
            queue.Clear ();
            queue.SetConsumer (true);
        }
        ~Consumer ()
        {
            queue.SetConsumer (false);
        }
    } consumer;
    auto upload = std::make_unique<av::StorySliceUpload> ();
    std::string error;
    ASSERT_TRUE (
        av::AppendIndexedSliceFill ({ Square () }, 3, upload->fill, upload->fillIndices, upload->areaM2, error));
    upload->storeys = 1;
    const auto expected = upload->fillIndices;
    const size_t bytes = upload->Bytes ();
    EXPECT_EQ (bytes, upload->outline.capacity () * sizeof (av::StorySliceVertex) +
                          upload->fill.capacity () * sizeof (av::StorySliceFillVertex) +
                          upload->fillIndices.capacity () * sizeof (uint32_t));
    consumer.queue.PushStorySlices (std::move (upload));
    EXPECT_EQ (consumer.queue.PendingBytes (), bytes);
    auto commands = consumer.queue.Take (1);
    ASSERT_EQ (commands.size (), 1u);
    ASSERT_NE (commands[0].storySlices, nullptr);
    EXPECT_EQ (commands[0].storySlices->fillIndices, expected);
    EXPECT_EQ (commands[0].storySlices->fill.size (), 4u);
    EXPECT_EQ (commands[0].storySlices->areaM2, 1);
    EXPECT_EQ (consumer.queue.PendingBytes (), 0u);
}
