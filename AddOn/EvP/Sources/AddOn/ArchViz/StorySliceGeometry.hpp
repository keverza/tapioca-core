#ifndef EVP_ARCHVIZ_STORYSLICEGEOMETRY_HPP
#define EVP_ARCHVIZ_STORYSLICEGEOMETRY_HPP

// ArchViz/StorySliceGeometry — a storey's union outline turned into drawables.
//
// `StorySliceUnion` answers WHERE the boundary is, as an unordered segment soup.
// This file answers WHAT IS DRAWN: the constant-width ribbon that outlines it and
// the triangles that fill it. Pure — no Diligent, no DevKit, no ACAPI — so the
// offline suite compiles the real source, which is the usual ArchViz reason:
// every way of getting this wrong produces a PICTURE rather than an error.
//
// ⚠️ THE SEGMENTS ARE CHAINED FIRST, AND THAT IS NOT TIDINESS. Two consumers
// need the chains and neither can work without them:
//
//   * THE DASH. A dash pattern is a function of distance ALONG the line. The
//     union's segments are split at every intersection, so they are short and
//     arbitrary; dashing each one from zero restarts the pattern at every
//     junction and produces a stutter that reads as z-fighting rather than as a
//     line style. Chaining gives one continuous arc length per contour.
//   * THE FILL. A trapezoid decomposition needs closed rings to run even-odd
//     over. A soup has no inside.
//
// ⚠️ THE RIBBON WIDTH IS IN PIXELS, NOT METRES — the same design as
// PlanAnchorRibbon and for the same reason. A storey contour is compared against
// the building around it at every zoom, and a fixed WORLD width is a fat band
// when zoomed in and invisible when zoomed out. Each vertex carries the two
// directions it may be pushed in; the vertex shader does the pushing in clip
// space, where a pixel has a size.

#include "ArchViz/StorySliceUnion.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {

// ⚠️ THIS STRUCT AND kStorySliceVS's input layout ARE ONE CONTRACT (see
// StorySliceLayer.cpp). A mismatch does not fail; it reads the wrong bytes and
// draws confidently wrong lines.
struct StorySliceVertex {
    float x, y, z; // world metres, Z-up, exactly as the slice emits
    float nx, ny;  // push direction ACROSS the line, unit, model space
    float tx, ty;  // push direction ALONG it (the square cap), unit
    // Distance along this CONTOUR in metres, continuous across the whole chain.
    // The pixel shader converts it to screen pixels to place the dashes; see the
    // header's note on why a per-segment value cannot work.
    float arc;
    // Which side of the centreline this corner is pushed to: -1 or +1.
    //
    // ⚠️ THIS IS THE ANTIALIASING, and it is the reason the ribbon is built one
    // pixel WIDER than the line it draws. Interpolated across the quad it is the
    // signed position across the line, so the pixel shader can measure its own
    // distance from the centre in pixels and fade the outermost pixel instead of
    // ending on a hard triangle edge. Without it a near-horizontal contour
    // staircases, which on a section line reads as bad geometry rather than as
    // the missing MSAA it actually is -- this target has none.
    float side;
};

// One triangle corner of the low-opacity fill. Position only — the colour is one
// constant for the whole layer, so there is nothing per-vertex to get wrong.
struct StorySliceFillVertex {
    float x, y, z;
};

// One contour of the union boundary, walked end to end.
struct SliceChain {
    std::vector<double> xy; // x, y interleaved, no closing repeat
    bool closed = false;

    size_t Count () const
    {
        return xy.size () / 2;
    }
};

// Segment soup -> contours. Open chains are walked from their loose ends first,
// then whatever remains forms closed loops — the same order and the same reason
// as SliceEngine's own chaining: a chain that legitimately fails to close is
// RETURNED open rather than dropped, because a silently dropped contour is a
// hole in the outline that nobody notices.
std::vector<SliceChain> ChainUnionSegments (const std::vector<UnionSegment>& segments);

// Contours -> ribbon triangles. Appends; does not clear `out`, so every storey
// accumulates into one buffer and one draw call.
//
// Each segment becomes a quad with SQUARE CAPS — extended half a width along its
// own tangent at both ends. Without them a corner leaves a triangular notch half
// a line-width across, which on a storey contour reads as a gap in the wall.
//
// Returns how many vertices were appended. A chain with fewer than 2 points
// appends nothing: a single point has no direction to be perpendicular to, and
// guessing one draws a line that is not in the model.
size_t BuildSliceRibbon (const std::vector<SliceChain>& chains, float z, std::vector<StorySliceVertex>& out);

// Contours -> fill triangles, by TRAPEZOID DECOMPOSITION under the even-odd rule.
//
// ⚠️ NOT AN EAR-CLIP, AND NOT THE STENCIL TRICK. A storey's union is not one
// simple polygon: it is several disjoint regions, some with holes (a wall with an
// opening cuts an inner ring). Ear clipping needs hole bridging, which is the
// fiddliest code in computer graphics and fails on exactly the degenerate input
// a real building produces. The stencil even-odd trick needs a stencil buffer,
// and this renderer's depth target is D32_FLOAT with no stencil plane.
//
// The decomposition works because the union output is already a PLANAR
// ARRANGEMENT — every crossing is a vertex, so between two consecutive vertex
// Y values no edge crosses another, edge order along X is constant across the
// band, and even-odd pairing of the crossings is exact. Holes and disjoint
// regions come out for free, with no special case for either.
//
// Appends; returns how many vertices were appended. Open chains are ignored —
// they cannot bound a region.
size_t BuildSliceFill (const std::vector<SliceChain>& chains, float z, std::vector<StorySliceFillVertex>& out);

// Indexed alternative; the scanline path above remains available to measured
// callers. Clipper2 resolves even-odd regions first, including nested islands,
// duplicate contours and intersections. Indices address shared normalized ring
// vertices, NOT original chain identities (normalization can generate corners).
// Double world XYZ avoids the legacy float conversion at survey coordinates.
struct StorySliceFillMesh {
    std::vector<double> points;
    std::vector<uint32_t> indices;
};

// Replaces out atomically on success, including a valid empty even-odd domain.
// Open/short chains bound nothing. Closed chains must contain complete finite
// XY pairs within +/-1e9 metres. decimalPrecision (0..8) controls local ClipperD
// normalization: 6 defaults to a 2^-20 metre grid; 7 retains the facade caller's
// 2^-24 grid. This is not exact-coordinate preservation.
// Approximate covers and generated triangulator crossings refuse with a reason.
bool BuildIndexedSliceFill (const std::vector<SliceChain>& chains, double z, StorySliceFillMesh& out,
                            std::string& error, int decimalPrecision = 6);

// Checked positive triangle area from double differences, before any GPU float
// conversion. Refusal leaves area untouched; valid empty domains measure zero.
bool MeasureSliceFill (const StorySliceFillMesh& mesh, double& area, std::string& error);

// The nonnegative area enclosed by the closed chains under even-odd, in square
// metres. Exposed because it is the honest way to TEST the fill — a triangle
// count says nothing about whether the right region was covered — and because a
// massing feasibility study wants exactly this number for a storey. Uses the
// default indexed normalization precision; returns NaN, never a fake zero, on
// invalid input/refusal. Call BuildIndexedSliceFill/MeasureSliceFill for errors.
double UnionArea (const std::vector<SliceChain>& chains);

} // namespace archviz
} // namespace geomsrv

#endif // EVP_ARCHVIZ_STORYSLICEGEOMETRY_HPP
