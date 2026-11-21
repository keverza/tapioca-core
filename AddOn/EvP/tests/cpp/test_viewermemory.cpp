// #14 memory and timing: the viewer's memory report (ArchViz/ViewerMemory). The HUD's Model card
// and archviz.log quote it, so a mip chain counted short or a traced part shown before any build
// would be read as a real measurement on a large project.

#include "ArchViz/ViewerMemory.hpp"

#include <gtest/gtest.h>

using namespace geomsrv::archviz;

TEST (ViewerMemory, TextureBytesCountTheWholeMipChain)
{
    EXPECT_EQ (TextureGpuBytes (4, 4, 1, 4), 64u);
    EXPECT_EQ (TextureGpuBytes (4, 4, 3, 4), (16u + 4u + 1u) * 4u);
    EXPECT_EQ (TextureGpuBytes (4, 4, 0, 4), (16u + 4u + 1u) * 4u);      // 0 = full chain
    EXPECT_EQ (TextureGpuBytes (8, 2, 0, 4), (16u + 4u + 2u + 1u) * 4u); // the short side stops at 1
    EXPECT_EQ (TextureGpuBytes (0, 16, 0, 4), 0u);
    // A 2048 px image with its mips is about 4/3 of the top level.
    const uint64_t top = 2048ull * 2048ull * 4ull;
    const uint64_t chain = TextureGpuBytes (2048, 2048, 0, 4);
    EXPECT_GT (chain, top);
    EXPECT_LT (chain, top * 4 / 3 + 8);
}

TEST (ViewerMemory, MegabytesKeepOneDecimalBelowAHundred)
{
    EXPECT_EQ (FormatMegabytes (0), "0.0 MB");
    EXPECT_EQ (FormatMegabytes (1536ull * 1024ull), "1.5 MB");
    EXPECT_EQ (FormatMegabytes (250ull * 1024ull * 1024ull), "250 MB");
}

TEST (ViewerMemory, TracedPartAppearsOnlyAfterABuild)
{
    ViewerMemory m;
    m.meshGpuBytes = 10ull << 20;
    m.textureGpuBytes = 5ull << 20;
    m.textureImages = 3;
    m.meshCpuBytes = 2ull << 20;
    EXPECT_EQ (FormatViewerMemoryFigure (m), "GPU 15.0 MB (meshes 10.0, textures 5.0), CPU 2.0 MB");
    EXPECT_EQ (FormatViewerMemoryLog (m).find ("traced"), std::string::npos);

    m.tracedBufferBytes = 4ull << 20;
    m.tracedScratchBytes = 1ull << 20;
    m.tracedTriangles = 1234;
    m.tracedBuildMs = 42.4;
    m.msPerSample = 0.5;
    EXPECT_EQ (m.GpuBytes (), 20ull << 20);
    EXPECT_EQ (FormatViewerMemoryFigure (m), "GPU 20.0 MB (meshes 10.0, textures 5.0, traced 5.0), CPU 2.0 MB");
    EXPECT_EQ (FormatViewerMemoryLog (m), "memory: GPU 20.0 MB -- meshes 10.0 MB, textures 5.0 MB (3 images), traced "
                                          "4.0 MB buffers + 1.0 MB build scratch (1234 triangles, built in 42 ms, "
                                          "0.50 ms per sample); CPU meshes 2.0 MB");
}
