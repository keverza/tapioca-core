#include "SunStudy/SunStudyAtlas.hpp"

#include <algorithm>
#include <cmath>

namespace evp::sunstudy {
namespace {

// A power-of-two square is the shape every GPU is happiest with, and it makes
// the atlas dimensions a pure function of the area needed rather than of the
// order the faces happened to arrive in.
uint32_t NextPowerOfTwo (uint32_t value)
{
    uint32_t result = 1;
    while (result < value && result < (1u << 30))
        result <<= 1;
    return result;
}

struct Pending {
    uint32_t face = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

} // namespace

SunStudyAtlas BuildSunStudyAtlas (const SampleGrid& grid, const AtlasOptions& options)
{
    SunStudyAtlas atlas;
    if (grid.layouts.empty ())
        return atlas; // the sampler was not asked for layouts

    const uint32_t gutter = std::max (1u, options.gutter);

    std::vector<Pending> pending;
    pending.reserve (grid.layouts.size ());
    uint64_t area = 0;
    for (size_t face = 0; face < grid.layouts.size (); ++face) {
        const FaceLayout& layout = grid.layouts[face];
        if (!layout.gridded || layout.columns == 0 || layout.rows == 0)
            continue;
        Pending item;
        item.face = static_cast<uint32_t> (face);
        item.width = layout.columns + gutter;
        item.height = layout.rows + gutter;
        area += static_cast<uint64_t> (item.width) * item.height;
        pending.push_back (item);
    }

    atlas.tiles.assign (grid.layouts.size (), AtlasTile ());
    if (pending.empty ()) {
        // Nothing to place is not a failure: a model of nothing but slivers is
        // a legitimate, if useless, study.
        atlas.width = atlas.height = 0;
        atlas.valid = true;
        return atlas;
    }

    // Start from the area needed plus a margin for shelf waste, then grow.
    uint32_t side = NextPowerOfTwo (static_cast<uint32_t> (std::ceil (std::sqrt (static_cast<double> (area)) * 1.15)));
    for (const Pending& item : pending)
        side = std::max (side, NextPowerOfTwo (std::max (item.width, item.height) + gutter));

    // ⚠️ TALLEST FIRST. A shelf packer fed in arbitrary order leaves a row as
    // tall as its tallest member and wastes the rest; sorted, the waste is
    // bounded by the height step between neighbours. The tie-break on face index
    // is what keeps the result identical run to run.
    std::sort (pending.begin (), pending.end (), [] (const Pending& a, const Pending& b) {
        if (a.height != b.height)
            return a.height > b.height;
        if (a.width != b.width)
            return a.width > b.width;
        return a.face < b.face;
    });

    while (side <= options.maxDimension) {
        bool fitted = true;
        uint32_t penX = gutter;
        uint32_t penY = gutter;
        uint32_t shelfHeight = 0;

        for (AtlasTile& tile : atlas.tiles)
            tile = AtlasTile ();

        for (const Pending& item : pending) {
            if (penX + item.width > side) { // next shelf
                penX = gutter;
                penY += shelfHeight;
                shelfHeight = 0;
            }
            if (penY + item.height > side) {
                fitted = false;
                break;
            }

            AtlasTile& tile = atlas.tiles[item.face];
            tile.x = penX;
            tile.y = penY;
            tile.width = item.width - gutter;
            tile.height = item.height - gutter;

            penX += item.width;
            shelfHeight = std::max (shelfHeight, item.height);
        }

        if (fitted) {
            atlas.width = side;
            atlas.height = side;
            break;
        }
        side <<= 1;
    }

    if (atlas.width == 0) {
        // See the header: refused, not truncated.
        for (AtlasTile& tile : atlas.tiles)
            tile = AtlasTile ();
        return atlas;
    }

    for (const AtlasTile& tile : atlas.tiles) {
        if (tile.Placed ())
            ++atlas.placedFaces;
    }

    // Every sample's texel, in the sample grid's own order.
    atlas.texels.assign (grid.Count (), 0u);
    const bool haveCells = grid.cellColumns.size () == grid.Count () && grid.cellRows.size () == grid.Count ();
    for (size_t i = 0; i < grid.Count (); ++i) {
        const uint32_t face = grid.faces[i];
        if (face >= atlas.tiles.size () || !haveCells)
            continue;
        const AtlasTile& tile = atlas.tiles[face];
        if (!tile.Placed ())
            continue;
        const uint32_t column = std::min (grid.cellColumns[i], tile.width - 1);
        const uint32_t row = std::min (grid.cellRows[i], tile.height - 1);
        atlas.texels[i] = (tile.y + row) * atlas.width + (tile.x + column);
    }

    atlas.valid = true;
    return atlas;
}

std::vector<float> ScatterToAtlas (const SunStudyAtlas& atlas, const std::vector<double>& perSample, float emptyValue)
{
    std::vector<float> image;
    if (!atlas.valid || atlas.TexelCount () == 0)
        return image;

    image.assign (atlas.TexelCount (), emptyValue);
    const size_t count = std::min (perSample.size (), atlas.texels.size ());
    for (size_t i = 0; i < count; ++i) {
        const uint32_t texel = atlas.texels[i];
        if (texel < image.size ())
            image[texel] = static_cast<float> (perSample[i]);
    }
    return image;
}

bool AtlasUv (const SunStudyAtlas& atlas, const SampleGrid& grid, uint32_t face, const double point[3], double spacing,
              float outUv[2])
{
    if (outUv == nullptr || point == nullptr || !atlas.valid)
        return false;
    if (face >= atlas.tiles.size () || face >= grid.layouts.size ())
        return false;
    if (!(spacing > 0.0))
        return false;

    const AtlasTile& tile = atlas.tiles[face];
    const FaceLayout& layout = grid.layouts[face];
    if (!tile.Placed () || !layout.gridded)
        return false;

    // Project the point into the face's own frame, then into its cell lattice.
    const double relative[3] = { point[0] - layout.origin[0], point[1] - layout.origin[1],
                                 point[2] - layout.origin[2] };
    const double u = relative[0] * layout.uAxis[0] + relative[1] * layout.uAxis[1] + relative[2] * layout.uAxis[2];
    const double v = relative[0] * layout.vAxis[0] + relative[1] * layout.vAxis[1] + relative[2] * layout.vAxis[2];

    // Continuous cell coordinates, so a point mid-cell lands mid-texel and
    // bilinear filtering interpolates between neighbouring samples rather than
    // stepping between them.
    const double cellU = u / spacing - static_cast<double> (layout.uStart);
    const double cellV = v / spacing - static_cast<double> (layout.vStart);

    // ⚠️ CLAMPED TO THE TILE, NOT WRAPPED. A vertex can sit a hair outside its
    // own cell range through float rounding, and wrapping would send it to the
    // opposite edge of the same face -- one texel of a facade reading its far
    // corner's hours, which looks like a lighting artefact rather than a bug.
    const double clampedU = std::min (std::max (cellU, 0.0), static_cast<double> (tile.width));
    const double clampedV = std::min (std::max (cellV, 0.0), static_cast<double> (tile.height));

    outUv[0] = static_cast<float> ((static_cast<double> (tile.x) + clampedU) / static_cast<double> (atlas.width));
    outUv[1] = static_cast<float> ((static_cast<double> (tile.y) + clampedV) / static_cast<double> (atlas.height));
    return true;
}

} // namespace evp::sunstudy
