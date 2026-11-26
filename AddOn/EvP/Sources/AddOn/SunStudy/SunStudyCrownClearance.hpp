#ifndef EVP_SUNSTUDY_SUNSTUDYCROWNCLEARANCE_HPP
#define EVP_SUNSTUDY_SUNSTUDYCROWNCLEARANCE_HPP

// SunStudy/SunStudyCrownClearance -- the room a mature crown has above a
// planting sample (#278 Stage 2, above-ground space fit).
//
// For every sample, measured against the study's own occluders (the same
// analysis and context geometry its light was traced against):
// - `overhead`: from the host surface straight up to the nearest surface
//   (SunStudyOverheadDistance), or < 0 for open sky;
// - `free`: at each level above the host, the distance to the nearest surface
//   along a fan of horizontal rays: the radius of the largest crown disc that
//   level could hold. Capped at `reach`.
//
// ⚠️ A FAN, NOT A SWEEP. `directions` rays per level see walls, balconies and
// anything wider than the gap between neighbouring rays; a slender post can
// slip between two rays far from the sample. 32 directions leave a gap of
// about 0.2 x the distance (0.6 m at 3 m).
//
// ⚠️ ONLY THE LEVELS. A crown is checked at the levels at or below its height;
// what lies between two levels is not seen, so the levels are dense low down
// where plants are many.
//
// ⚠️ THE SAMPLE'S OWN HOST IS NOT AN OBSTACLE. Horizontal rays skip the host
// mesh the sample stands on, so a slope does not read as a wall; a rise in
// that same mesh (a terrain bank) is therefore not seen sideways either.
//
// Pure CPU geometry on immutable occluders; no store lock, no DevKit.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace evp::sunstudy {

struct SunStudyOccluders;

struct CrownClearance {
    std::vector<double> levels; // m above the host, ascending
    double reach = 0.0;         // m: a free radius equal to reach means "at least reach"
    int directions = 0;
    std::vector<float> overhead; // per sample, m; < 0 open above
    std::vector<float> free;     // per sample x level (sample-major), m, <= reach

    size_t Count () const
    {
        return overhead.size ();
    }
    bool Empty () const
    {
        return overhead.empty ();
    }
    float FreeAt (size_t sample, size_t level) const
    {
        return free[sample * levels.size () + level];
    }
    // The rows `keep` names, in that order. Inline: the pure planting code
    // (and the UI playground) uses it without the geometry.
    CrownClearance Subset (const std::vector<size_t>& keep) const
    {
        CrownClearance out;
        out.levels = levels;
        out.reach = reach;
        out.directions = directions;
        if (Empty ())
            return out;
        const size_t k = levels.size ();
        out.overhead.reserve (keep.size ());
        out.free.reserve (keep.size () * k);
        for (size_t s : keep) {
            out.overhead.push_back (overhead[s]);
            out.free.insert (out.free.end (), free.begin () + std::ptrdiff_t (s * k),
                             free.begin () + std::ptrdiff_t ((s + 1) * k));
        }
        return out;
    }
};

struct CrownClearanceOptions {
    // Dense where perennials and shrubs are, sparser up a tree's crown.
    std::vector<double> levels { 0.15, 0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 5.0, 6.5, 8.0, 10.0, 13.0, 16.0, 20.0, 25.0 };
    double reach = 12.0;
    int directions = 32;
    size_t maxParallel = 0; // 0: hardware threads less one
};

// Measures every sample (`x`, `y`, `groundZ` parallel; `host[i]` indexes
// `hostGuids`, the mesh the sample stands on). `done`, when given, counts the
// finished samples. False with `error` on mismatched input or cancellation.
bool MeasureCrownClearance (const SunStudyOccluders& occluders, const std::vector<double>& x,
                            const std::vector<double>& y, const std::vector<double>& groundZ,
                            const std::vector<uint32_t>& host, const std::vector<std::string>& hostGuids,
                            const CrownClearanceOptions& options, CrownClearance& out, std::string& error,
                            const std::function<bool ()>& isCancelled = {}, std::atomic<size_t>* done = nullptr);

} // namespace evp::sunstudy

#endif
