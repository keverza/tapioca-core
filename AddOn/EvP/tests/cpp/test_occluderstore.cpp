// ArchViz/Dxgi/OccluderStore: the host model's triangles kept per element, so an element can be
// replaced or removed without re-extracting the rest -- and the flat buffers the render thread
// uploads are assembled from it.

#include "ArchViz/Dxgi/OccluderStore.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace store = geomsrv::archviz::dxgi::occluderstore;

namespace {

constexpr size_t kRoom = 1000000;

// A triangle at height `z`: three vertices, opaque.
void Triangle (store::Store& into, const std::string& guid, float z)
{
    const float xyz[9] = { 0, 0, z, 1, 0, z, 0, 1, z };
    const uint32_t indices[3] = { 0, 1, 2 };
    ASSERT_TRUE (into.Begin (guid, xyz, 3, kRoom));
    ASSERT_TRUE (into.AddOpaque (indices, 3, kRoom));
}

struct Flat {
    std::vector<float> positions;
    std::vector<uint32_t> opaque, transparent;
};

Flat Assembled (const store::Store& from)
{
    Flat flat;
    from.Assemble (flat.positions, flat.opaque, flat.transparent);
    return flat;
}

} // namespace

TEST (OccluderStore, ElementsAreFlattenedWithTheirIndicesOffsetToTheirBlock)
{
    store::Store s;
    Triangle (s, "a", 0.0f);
    Triangle (s, "b", 3.0f);
    const Flat flat = Assembled (s);
    ASSERT_EQ (flat.positions.size (), 18u);
    EXPECT_EQ (flat.opaque, (std::vector<uint32_t> { 0, 1, 2, 3, 4, 5 }));
    EXPECT_FLOAT_EQ (flat.positions[9 + 2], 3.0f) << "b's block follows a's";
}

// An element re-extracted used to be appended beside its old self, drawn twice.
TEST (OccluderStore, AnElementSentAgainReplacesItself)
{
    store::Store s;
    Triangle (s, "a", 0.0f);
    Triangle (s, "a", 5.0f);
    EXPECT_EQ (s.ElementCount (), 1u);
    EXPECT_EQ (s.VertexCount (), 3u);
    EXPECT_EQ (s.OpaqueIndexCount (), 3u);
    const Flat flat = Assembled (s);
    ASSERT_EQ (flat.positions.size (), 9u);
    EXPECT_FLOAT_EQ (flat.positions[2], 5.0f);
}

// 2026-10-02 11:45: three elements hidden, the whole model re-extracted to drop them.
TEST (OccluderStore, AHiddenElementLeavesAndTheRestKeepTheirPlaceInTheIndices)
{
    store::Store s;
    Triangle (s, "a", 0.0f);
    Triangle (s, "b", 1.0f);
    Triangle (s, "c", 2.0f);
    EXPECT_TRUE (s.Remove ("b"));
    EXPECT_FALSE (s.Remove ("b")) << "already gone";
    EXPECT_EQ (s.VertexCount (), 6u);
    const Flat flat = Assembled (s);
    ASSERT_EQ (flat.positions.size (), 18u);
    EXPECT_EQ (flat.opaque, (std::vector<uint32_t> { 0, 1, 2, 3, 4, 5 }));
    EXPECT_FLOAT_EQ (flat.positions[9 + 2], 2.0f) << "c follows a once b is gone";
}

TEST (OccluderStore, GlassKeepsItsEdgesButNeverOccludes)
{
    store::Store s;
    const float xyz[9] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    const uint32_t indices[3] = { 0, 1, 2 };
    ASSERT_TRUE (s.Begin ("glass", xyz, 3, kRoom));
    ASSERT_TRUE (s.AddTransparent (indices, 3));
    Triangle (s, "wall", 1.0f);
    const Flat flat = Assembled (s);
    EXPECT_EQ (flat.transparent, (std::vector<uint32_t> { 0, 1, 2 }));
    EXPECT_EQ (flat.opaque, (std::vector<uint32_t> { 3, 4, 5 }));
}

TEST (OccluderStore, AnElementThatWouldNotFitIsRefusedAndItsOldGeometryGoes)
{
    store::Store s;
    Triangle (s, "a", 0.0f);
    const float big[12] = {};
    EXPECT_FALSE (s.Begin ("a", big, 4, 3)) << "four vertices into room for three";
    EXPECT_EQ (s.ElementCount (), 0u) << "the old triangle is not what the model holds any more";
    EXPECT_EQ (s.VertexCount (), 0u);
    const uint32_t indices[3] = { 0, 1, 2 };
    EXPECT_FALSE (s.AddOpaque (indices, 3, kRoom)) << "no element is open after a refusal";
}

TEST (OccluderStore, RemovingTheElementBeingFilledClosesIt)
{
    store::Store s;
    const float xyz[9] = {};
    ASSERT_TRUE (s.Begin ("a", xyz, 3, kRoom));
    EXPECT_TRUE (s.Remove ("a"));
    const uint32_t indices[3] = { 0, 1, 2 };
    EXPECT_FALSE (s.AddOpaque (indices, 3, kRoom));
    EXPECT_EQ (s.OpaqueIndexCount (), 0u);
}
