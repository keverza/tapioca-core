#ifndef EVP_SUNSTUDY_SUNSTUDYPATCHATLAS_HPP
#define EVP_SUNSTUDY_SUNSTUDYPATCHATLAS_HPP

// SunStudy/SunStudyPatchAtlas — one atlas tile per SURFACE, with an address that
// survives an edit.
//
// ---------------------------------------------------------------------------
// WHY THE OLD ATLAS CANNOT DO INCREMENTAL UPDATES
//
// `SunStudyAtlas` packs by TRIANGLE and repacks from scratch every time it is
// built. Both are fatal to a partial update:
//
//   * a triangle is created and destroyed by any re-tessellation, so a tile
//     addressed by one has no identity across an edit;
//   * a repack moves every OTHER tile, so updating one surface would invalidate
//     the texture coordinates of all the rest -- and the failure is silent,
//     because the model still draws, in somebody else's colours.
//
// This atlas allocates by `PatchKey` and KEEPS the allocation. An untouched
// surface's tile does not move when a wall across the site changes, so the GPU
// can be handed one rectangle instead of a new texture.
//
// ⚠️ IT DELIBERATELY NEVER COMPACTS. Fragmentation wastes texels; a compaction
// during live analysis moves tiles that other patches are still being drawn
// from. A stable address is worth far more than a tight packing, and the
// occupancy figure is not a defect to chase.
//
// ⚠️ NO ACAPI, NO Diligent, NO GPU TYPE -- it is arithmetic over rectangles.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "SunStudy/SunStudyCache.hpp"        // PatchKey
#include "SunStudy/SunStudyPatchSampler.hpp" // PatchSampleGrid

namespace evp::sunstudy {

struct PatchAtlasAllocation {
    PatchKey key;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    // Bumped whenever this allocation's RECTANGLE changes. ⚠️ A consumer holding
    // a stale rectangle is the exact fault this atlas exists to prevent, so the
    // rectangle carries a number that says when it last moved.
    uint32_t generation = 0;

    bool Placed () const
    {
        return width > 0 && height > 0;
    }
};

// What one update did, so "is this actually incremental?" is a reading rather
// than a belief.
//
// ⚠️ WITHOUT THESE THE FEATURE CANNOT BE DISTINGUISHED FROM A FULL REBUILD THAT
// HAPPENS TO LOOK THE SAME. A correct incremental update and a whole repack
// produce identical pictures; only the counts tell them apart.
struct PatchAtlasUpdate {
    size_t retained = 0; // same key, same size: the allocation did not move
    size_t reused = 0;   // dirty, same size: same rectangle, new values
    size_t moved = 0;    // dirty, different size: a new rectangle
    size_t added = 0;
    size_t removed = 0;
    bool resized = false; // the atlas itself had to grow
};

class SunStudyPatchAtlas final {
  public:
    // Fit `grid`'s patches, keeping every allocation whose key and lattice size
    // are unchanged. Returns what it did.
    //
    // ⚠️ IDEMPOTENT ON AN UNCHANGED GRID: calling it twice with the same patches
    // must report `retained == patches` and move nothing. A repack hiding behind
    // an "update" is the failure mode, so the test asserts exactly that.
    PatchAtlasUpdate Fit (const PatchSampleGrid& grid, uint32_t gutter = 1, uint32_t maxDimension = 8192);

    const PatchAtlasAllocation* Find (const PatchKey& key) const;

    uint32_t Width () const
    {
        return width_;
    }
    uint32_t Height () const
    {
        return height_;
    }
    size_t TexelCount () const
    {
        return static_cast<size_t> (width_) * height_;
    }
    size_t AllocationCount () const
    {
        return allocations_.size ();
    }
    const std::map<PatchKey, PatchAtlasAllocation>& Allocations () const
    {
        return allocations_;
    }

    // The texel index of one sample, or -1 when its patch is not placed.
    int64_t TexelOf (const PatchSampleGrid& grid, size_t sampleIndex) const;

    // Scatter one PATCH's values into `image`, which must be TexelCount() long.
    //
    // ⚠️ ONE PATCH, NOT THE WHOLE STUDY, AND THAT IS THE POINT. The incremental
    // path writes only the rectangles it recomputed; a whole-image scatter would
    // touch every texel and there would be nothing partial about the upload.
    bool ScatterPatch (const PatchSampleGrid& grid, size_t spanIndex, const std::vector<double>& hoursForSpan,
                       std::vector<float>& image) const;

    // Everything an allocation's rectangle covers, for a partial GPU upload.
    // `out` is `width * height` floats read from `image`.
    bool ReadRectangle (const PatchAtlasAllocation& allocation, const std::vector<float>& image,
                        std::vector<float>& out) const;

    void Clear ();

  private:
    bool Place (uint32_t width, uint32_t height, uint32_t gutter, uint32_t& x, uint32_t& y);
    void Rebuild (const PatchSampleGrid& grid, uint32_t gutter, uint32_t side);

    std::map<PatchKey, PatchAtlasAllocation> allocations_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    // The shelf cursor. ⚠️ IT ONLY EVER MOVES FORWARD -- freeing a tile does not
    // reclaim its space, because reclaiming means compacting and compacting means
    // moving somebody else's tile. See the file header.
    uint32_t penX_ = 0;
    uint32_t penY_ = 0;
    uint32_t shelfHeight_ = 0;
    uint32_t nextGeneration_ = 1;
};

} // namespace evp::sunstudy

#endif // EVP_SUNSTUDY_SUNSTUDYPATCHATLAS_HPP
