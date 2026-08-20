#include "MeshFixtures.hpp"
#include "RenderEngine.hpp"

#include <gtest/gtest.h>

#include <memory>

using namespace evptest;
using geomsrv::Camera;
using geomsrv::QueryEngine;
using geomsrv::Render;
using geomsrv::RenderResult;

namespace {

std::shared_ptr<const geomsrv::Snapshot> BoxSnapshot ()
{
    return std::make_shared<const geomsrv::Snapshot> (
        MakeSnapshot ({ MakeBox ("box", -0.5, -0.5, -0.5) }));
}

// Looking straight down the -Z axis at a box centred on the origin.
Camera TopDown ()
{
    Camera c;
    c.eye[0] = 0; c.eye[1] = 0; c.eye[2] = 10;
    c.target[0] = 0; c.target[1] = 0; c.target[2] = 0;
    c.up[0] = 0; c.up[1] = 1; c.up[2] = 0;   // Z is the view axis, so up must not be Z
    return c;
}

size_t CountForeground (const RenderResult& r)
{
    size_t n = 0;
    for (const int32_t id : r.id)
        if (id >= 0) ++n;
    return n;
}

} // namespace

TEST (RenderEngine, RendersTheRequestedSize)
{
    const QueryEngine eng (BoxSnapshot ());
    const RenderResult r = Render (eng, TopDown (), 64, 48, /*threads=*/1);

    EXPECT_TRUE (r.Valid ());
    EXPECT_EQ (r.w, 64);
    EXPECT_EQ (r.h, 48);
    EXPECT_EQ (r.id.size (), 64u * 48u);
    EXPECT_EQ (r.depth.size (), 64u * 48u);
    EXPECT_EQ (r.normal.size (), 64u * 48u * 3u);
}

TEST (RenderEngine, CentrePixelHitsTheBoxAndCornersDoNot)
{
    const QueryEngine eng (BoxSnapshot ());
    const RenderResult r = Render (eng, TopDown (), 64, 64, 1);

    const int centre = 32 * 64 + 32;
    EXPECT_GE (r.id[centre], 0) << "the box is dead centre of the view";
    // Box top face is at z = 0.5, eye at z = 10 -> depth 9.5.
    EXPECT_NEAR (r.depth[centre], 9.5, 1e-3);

    EXPECT_EQ (r.id[0], -1) << "top-left corner should be background";
    EXPECT_FLOAT_EQ (r.depth[0], 0.0f);
}

TEST (RenderEngine, BackgroundAndForegroundPartitionTheImage)
{
    const QueryEngine eng (BoxSnapshot ());
    const RenderResult r = Render (eng, TopDown (), 32, 32, 1);

    const size_t fg = CountForeground (r);
    EXPECT_GT (fg, 0u) << "nothing was hit";
    EXPECT_LT (fg, r.id.size ()) << "everything was hit; the box should not fill the view";

    for (size_t i = 0; i < r.id.size (); ++i) {
        if (r.id[i] < 0)
            EXPECT_FLOAT_EQ (r.depth[i], 0.0f) << "background pixel " << i << " has depth";
        else
            EXPECT_GT (r.depth[i], 0.0f) << "foreground pixel " << i << " has no depth";
    }
}

// Threading must not change the image — this is the one place in Geometry/ that
// is internally multithreaded (docs/guides/testing.md §5).
TEST (RenderEngine, ThreadCountDoesNotChangeTheImage)
{
    const QueryEngine eng (BoxSnapshot ());
    const RenderResult one = Render (eng, TopDown (), 48, 48, 1);
    const RenderResult many = Render (eng, TopDown (), 48, 48, 8);
    const RenderResult autoN = Render (eng, TopDown (), 48, 48, 0);

    ASSERT_EQ (one.id.size (), many.id.size ());
    EXPECT_EQ (one.id, many.id);
    EXPECT_EQ (one.depth, many.depth);
    EXPECT_EQ (one.id, autoN.id);
    EXPECT_EQ (one.depth, autoN.depth);
}

TEST (RenderEngine, OrthographicRendersToo)
{
    const QueryEngine eng (BoxSnapshot ());
    Camera cam = TopDown ();
    cam.ortho = true;
    cam.orthoHeight = 4.0;

    const RenderResult r = Render (eng, cam, 32, 32, 1);
    EXPECT_TRUE (r.Valid ());
    EXPECT_GT (CountForeground (r), 0u);
}

TEST (RenderEngine, EmptySceneRendersAllBackground)
{
    const auto snap = std::make_shared<const geomsrv::Snapshot> (geomsrv::Snapshot {});
    const QueryEngine eng (snap);
    const RenderResult r = Render (eng, TopDown (), 16, 16, 1);

    EXPECT_TRUE (r.Valid ());
    EXPECT_EQ (CountForeground (r), 0u);
}

TEST (RenderEngine, DegenerateSizesAreRejectedNotCrashed)
{
    const QueryEngine eng (BoxSnapshot ());
    for (const auto wh : { std::pair<int, int> { 0, 10 }, { 10, 0 }, { -1, 10 }, { 10, -1 } }) {
        RenderResult r;
        ASSERT_NO_FATAL_FAILURE (r = Render (eng, TopDown (), wh.first, wh.second, 1));
        EXPECT_FALSE (r.Valid ()) << wh.first << "x" << wh.second;
    }
}

TEST (RenderEngine, DegenerateSceneDoesNotCrash)
{
    const auto snap = std::make_shared<const geomsrv::Snapshot> (MakeDegenerateSnapshot ());
    const QueryEngine eng (snap);
    EXPECT_NO_FATAL_FAILURE (Render (eng, TopDown (), 16, 16, 2));
}
