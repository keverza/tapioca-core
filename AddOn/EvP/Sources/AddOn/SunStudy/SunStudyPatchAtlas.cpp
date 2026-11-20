#include "SunStudy/SunStudyPatchAtlas.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace evp::sunstudy {

SunStudyPatchAtlas SunStudyPatchAtlas::FromState (PatchAtlasState state)
{
    SunStudyPatchAtlas atlas;
    atlas.allocations_ = std::move (state.allocations);
    atlas.width_ = state.width;
    atlas.height_ = state.height;
    atlas.penX_ = state.penX;
    atlas.penY_ = state.penY;
    atlas.shelfHeight_ = state.shelfHeight;
    atlas.nextGeneration_ = state.nextGeneration;
    return atlas;
}

PatchAtlasState SunStudyPatchAtlas::State () const
{
    PatchAtlasState state;
    state.width = width_;
    state.height = height_;
    state.penX = penX_;
    state.penY = penY_;
    state.shelfHeight = shelfHeight_;
    state.nextGeneration = nextGeneration_;
    state.allocations = allocations_;
    return state;
}

const PatchAtlasAllocation* SunStudyPatchAtlas::Find (const PatchKey& key) const
{
    const auto found = allocations_.find (key);
    return found == allocations_.end () ? nullptr : &found->second;
}

int64_t SunStudyPatchAtlas::TexelOf (const PatchSampleGrid& grid, size_t sampleIndex) const
{
    if (sampleIndex >= grid.Count () || sampleIndex >= grid.spanOf.size ())
        return -1;
    const uint32_t spanIndex = grid.spanOf[sampleIndex];
    if (spanIndex >= grid.spans.size ())
        return -1;
    const PatchAtlasAllocation* allocation = Find (grid.spans[spanIndex].key);
    if (allocation == nullptr || !allocation->Placed ())
        return -1;
    const uint32_t column = std::min (grid.cellColumns[sampleIndex], allocation->width - 1);
    const uint32_t row = std::min (grid.cellRows[sampleIndex], allocation->height - 1);
    return static_cast<int64_t> (allocation->y + row) * width_ + (allocation->x + column);
}

} // namespace evp::sunstudy
