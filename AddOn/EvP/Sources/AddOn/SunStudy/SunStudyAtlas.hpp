#ifndef EVP_SUNSTUDY_SUNSTUDYATLAS_HPP
#define EVP_SUNSTUDY_SUNSTUDYATLAS_HPP

// SunStudy/SunStudyAtlas — the study's result as a texture, and the map from a
// point on the model to a texel in it.
//
// ⚠️ THIS IS WHAT MAKES A DISPLAY CHANGE FREE. Today a new threshold, palette,
// display mode or filter range costs a whole round trip and a page rebuild,
// because the result is carried as per-vertex numbers baked into geometry.
// Resident in a texture, all four become a read of something already on the GPU:
// the study is computed once and looked at many ways, which is how a person
// actually uses one.
//
// ⚠️ IT ALSO BREAKS THE TIE BETWEEN SAMPLE DENSITY AND MESH TOPOLOGY. The engine
// this replaces measured VERTICES, so asking for a finer study meant subdividing
// the model -- 176,106 vertices for a 112-triangle site, a spent budget, and a
// mean biased by wherever the subdivision happened to put its points. Here the
// geometry is untouched and the atlas resolution is the only knob.
//
// The packing is a shelf: faces sorted tall-first, laid in rows. It is not the
// tightest packing known and does not try to be -- it is deterministic, it is a
// few dozen lines, and the wasted texels cost memory rather than correctness.
// ⚠️ DETERMINISM IS THE REQUIREMENT: the same model and spacing must produce the
// same atlas every run, or a cached study cannot be reused and two consumers
// cannot be diffed.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "SunStudy/SunStudySampler.hpp"

namespace evp::sunstudy {

// Where one face lives in the atlas. A face with `width == 0` was not placed --
// it was degenerate, or below the grid, and has no cell lattice to map into.
struct AtlasTile {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;

    bool Placed () const
    {
        return width > 0 && height > 0;
    }
};

struct SunStudyAtlas {
    uint32_t width = 0;
    uint32_t height = 0;

    // One per SOURCE face, in source order.
    std::vector<AtlasTile> tiles;

    // Texel index (row * width + column) for every sample, parallel to the
    // sample grid's own arrays. This is what turns per-sample hours into a
    // texture: `texels[i]` is where sample `i`'s value belongs.
    std::vector<uint32_t> texels;

    size_t placedFaces = 0;

    // ⚠️ FALSE MEANS REFUSED, NOT TRUNCATED. An atlas missing some faces would
    // draw a model with holes in its study while every total still looked right.
    bool valid = false;

    size_t TexelCount () const
    {
        return static_cast<size_t> (width) * static_cast<size_t> (height);
    }
};

struct AtlasOptions {
    // Texels of empty space around every tile.
    //
    // ⚠️ ONE IS THE MINIMUM AND ZERO IS A BUG. A bilinear sample near a tile
    // edge reaches into the neighbouring texel, so with no gutter a facade in
    // full sun bleeds into the roof packed beside it -- and the artefact looks
    // exactly like real shadow, appearing along element boundaries where a
    // person expects shading to change anyway.
    uint32_t gutter = 1;

    // Hard ceiling per axis. 2048 is the Texture2DArray slice limit the D3D11
    // path is already sized against; a square atlas of it holds 4.1M texels.
    uint32_t maxDimension = 8192;
};

// Pack the sampler's per-face layouts into one atlas.
SunStudyAtlas BuildSunStudyAtlas (const SampleGrid& grid, const AtlasOptions& options = AtlasOptions ());

// Scatter per-sample values into a texel image, `atlas.TexelCount()` long.
//
// `emptyValue` fills every texel no sample landed on -- the gutters, the cells
// that fell outside their triangle, and any face that was never placed.
// ⚠️ IT MUST NOT BE ZERO IN A CONSUMER THAT DRAWS HOURS: zero is a legitimate
// result meaning "never sunlit", so an empty texel drawn as zero is a shadow
// that is not there. Use a negative sentinel and test for it in the shader.
std::vector<float> ScatterToAtlas (const SunStudyAtlas& atlas, const std::vector<double>& perSample,
                                   float emptyValue = -1.0f);

// Where a point on a face lands in the atlas, as texture coordinates in [0, 1].
//
// Returns false when the face was not placed, in which case the caller has to
// fall back to that face's single sample -- see `FaceLayout::gridded`.
bool AtlasUv (const SunStudyAtlas& atlas, const SampleGrid& grid, uint32_t face, const double point[3], double spacing,
              float outUv[2]);

} // namespace evp::sunstudy

#endif // EVP_SUNSTUDY_SUNSTUDYATLAS_HPP
