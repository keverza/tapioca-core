// ArchViz/Dxgi/OverlayComposer: whether a depth can be bound beside the target the overlay
// composes into at Present. D3D11 refuses a render target and a depth-stencil that differ
// in size or in sample count, and then nothing composed lands while every counter rises.

#include "ArchViz/Dxgi/OverlayComposer.hpp"

#include <gtest/gtest.h>

namespace compose = geomsrv::archviz::dxgi::overlaycompose;

// ⚠️ THE USER, 2026-10-01: the 3D overlay composed by every counter and nothing reached
// the screen. The presented frames were byte-identical before and after our drawing; the
// back buffer is 1-sample, the host depth was copied from Archicad's scene depth, sample
// count included. Same size, different counts: refused.
TEST (OverlayComposer, ADepthWithAnotherSampleCountDoesNotFitTheTarget)
{
    EXPECT_FALSE (compose::DepthFitsTarget (3177, 1809, 1, 3177, 1809, 4)) << "multisampled depth, plain back buffer";
    EXPECT_FALSE (compose::DepthFitsTarget (3177, 1809, 4, 3177, 1809, 1));
    EXPECT_TRUE (compose::DepthFitsTarget (3177, 1809, 1, 3177, 1809, 1));
}

// The size rule it extends (OVERLAY-INVARIANTS.md §2): one pixel is enough to refuse.
TEST (OverlayComposer, ADepthOnePixelNarrowerDoesNotFitTheTarget)
{
    EXPECT_FALSE (compose::DepthFitsTarget (2450, 1300, 1, 2449, 1300, 1));
    EXPECT_FALSE (compose::DepthFitsTarget (2450, 1300, 1, 2450, 1301, 1));
}
