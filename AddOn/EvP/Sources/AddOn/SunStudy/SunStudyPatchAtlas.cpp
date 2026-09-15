#include "SunStudy/SunStudyPatchAtlas.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace evp::sunstudy {

namespace {

uint32_t NextPowerOfTwo (uint32_t value)
{
    uint32_t result = 1;
    while (result < value && result < (1u << 30))
        result <<= 1;
    return result;
}

} // namespace

void SunStudyPatchAtlas::Clear ()
{
    allocations_.clear ();
    width_ = height_ = 0;
    penX_ = penY_ = 0;
    shelfHeight_ = 0;
    // ⚠️ THE GENERATION DOES NOT RESET. A consumer still holding a rectangle
    // from before the clear must not find its number matching one issued after.
    ++nextGeneration_;
}

bool SunStudyPatchAtlas::Place (uint32_t width, uint32_t height, uint32_t gutter, uint32_t& x, uint32_t& y)
{
    const uint32_t needW = width + gutter;
    const uint32_t needH = height + gutter;
    if (penX_ + needW > width_) { // next shelf
        penX_ = gutter;
        penY_ += shelfHeight_;
        shelfHeight_ = 0;
    }
    if (penY_ + needH > height_)
        return false;
    x = penX_;
    y = penY_;
    penX_ += needW;
    shelfHeight_ = std::max (shelfHeight_, needH);
    return true;
}

void SunStudyPatchAtlas::Rebuild (const PatchSampleGrid& grid, uint32_t gutter, uint32_t side)
{
    // ⚠️ A REBUILD IS THE FALLBACK, NOT THE PATH. It happens when the atlas has
    // to grow, and then every rectangle legitimately moves -- which is why the
    // caller is told `resized` and must re-send the whole layout rather than a
    // rectangle.
    allocations_.clear ();
    width_ = height_ = side;
    penX_ = penY_ = gutter;
    shelfHeight_ = 0;

    // Tallest first, as the original packer does: a shelf fed in arbitrary order
    // leaves a row as tall as its tallest member and wastes the rest.
    std::vector<size_t> order (grid.spans.size ());
    for (size_t i = 0; i < order.size (); ++i)
        order[i] = i;
    std::stable_sort (order.begin (), order.end (), [&grid] (size_t a, size_t b) {
        if (grid.spans[a].rows != grid.spans[b].rows)
            return grid.spans[a].rows > grid.spans[b].rows;
        return grid.spans[a].columns > grid.spans[b].columns;
    });

    for (const size_t index : order) {
        const PatchSampleSpan& span = grid.spans[index];
        PatchAtlasAllocation allocation;
        allocation.key = span.key;
        allocation.width = span.columns;
        allocation.height = span.rows;
        allocation.generation = nextGeneration_++;
        if (!Place (span.columns, span.rows, gutter, allocation.x, allocation.y)) {
            allocation.width = allocation.height = 0; // refused; the caller grows
        }
        allocations_[span.key] = allocation;
    }
}

PatchAtlasUpdate SunStudyPatchAtlas::Fit (const PatchSampleGrid& grid, uint32_t gutter, uint32_t maxDimension)
{
    PatchAtlasUpdate update;
    gutter = std::max (1u, gutter);

    std::set<PatchKey> wanted;
    for (const PatchSampleSpan& span : grid.spans)
        wanted.insert (span.key);

    // ---- 1. retire what is gone -------------------------------------------
    for (auto it = allocations_.begin (); it != allocations_.end ();) {
        if (wanted.count (it->first) == 0) {
            // ⚠️ THE SPACE IS NOT RECLAIMED. Reclaiming means compacting, and
            // compacting moves tiles that other patches are still drawn from.
            it = allocations_.erase (it);
            ++update.removed;
        }
        else {
            ++it;
        }
    }

    // ---- 2. an empty atlas needs a size before anything can be placed ------
    if (width_ == 0) {
        uint64_t area = 0;
        uint32_t widest = 1;
        for (const PatchSampleSpan& span : grid.spans) {
            area += static_cast<uint64_t> (span.columns + gutter) * (span.rows + gutter);
            widest = std::max (widest, std::max (span.columns, span.rows) + gutter * 2);
        }
        uint32_t side = NextPowerOfTwo (static_cast<uint32_t> (std::ceil (std::sqrt (double (area)) * 1.15)));
        side = std::max (side, NextPowerOfTwo (widest));
        while (side <= maxDimension) {
            Rebuild (grid, gutter, side);
            bool fitted = true;
            for (const auto& entry : allocations_)
                fitted = fitted && entry.second.Placed ();
            if (fitted) {
                update.added = allocations_.size ();
                update.resized = true;
                return update;
            }
            side <<= 1;
        }
        Clear ();
        return update; // refused
    }

    // ---- 3. place what is new, keep what is not ---------------------------
    for (const PatchSampleSpan& span : grid.spans) {
        auto found = allocations_.find (span.key);
        if (found != allocations_.end () && found->second.width == span.columns && found->second.height == span.rows) {
            // ⚠️ THE WHOLE POINT OF THE CLASS. Same surface, same lattice: the
            // rectangle does not move, so every texture coordinate already
            // handed out stays valid and the GPU needs nothing at all.
            ++update.retained;
            continue;
        }

        PatchAtlasAllocation allocation;
        allocation.key = span.key;
        allocation.width = span.columns;
        allocation.height = span.rows;
        allocation.generation = nextGeneration_++;
        if (!Place (span.columns, span.rows, gutter, allocation.x, allocation.y)) {
            // Out of room without compacting: grow and repack everything. The
            // caller learns this through `resized` and re-sends the layout.
            uint32_t side = width_;
            while (side <= maxDimension) {
                side <<= 1;
                if (side > maxDimension)
                    break;
                Rebuild (grid, gutter, side);
                bool fitted = true;
                for (const auto& entry : allocations_)
                    fitted = fitted && entry.second.Placed ();
                if (fitted) {
                    PatchAtlasUpdate grown;
                    grown.added = allocations_.size ();
                    grown.resized = true;
                    return grown;
                }
            }
            Clear ();
            return PatchAtlasUpdate {};
        }

        if (found != allocations_.end ())
            ++update.moved;
        else
            ++update.added;
        allocations_[span.key] = allocation;
    }
    return update;
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

bool SunStudyPatchAtlas::ScatterPatch (const PatchSampleGrid& grid, size_t spanIndex,
                                       const std::vector<double>& hoursForSpan, std::vector<float>& image) const
{
    if (spanIndex >= grid.spans.size () || image.size () != TexelCount ())
        return false;
    const PatchSampleSpan& span = grid.spans[spanIndex];
    if (hoursForSpan.size () != span.count)
        return false;
    const PatchAtlasAllocation* allocation = Find (span.key);
    if (allocation == nullptr || !allocation->Placed ())
        return false;

    for (size_t i = 0; i < span.count; ++i) {
        const int64_t texel = TexelOf (grid, span.first + i);
        if (texel < 0 || static_cast<size_t> (texel) >= image.size ())
            continue;
        image[static_cast<size_t> (texel)] = static_cast<float> (hoursForSpan[i]);
    }
    return true;
}

bool SunStudyPatchAtlas::ReadRectangle (const PatchAtlasAllocation& allocation, const std::vector<float>& image,
                                        std::vector<float>& out) const
{
    if (!allocation.Placed () || image.size () != TexelCount ())
        return false;
    out.assign (static_cast<size_t> (allocation.width) * allocation.height, -1.0f);
    for (uint32_t row = 0; row < allocation.height; ++row) {
        for (uint32_t column = 0; column < allocation.width; ++column) {
            const size_t source = static_cast<size_t> (allocation.y + row) * width_ + (allocation.x + column);
            if (source < image.size ())
                out[static_cast<size_t> (row) * allocation.width + column] = image[source];
        }
    }
    return true;
}

} // namespace evp::sunstudy
