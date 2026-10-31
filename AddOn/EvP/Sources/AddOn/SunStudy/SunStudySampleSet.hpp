#ifndef EVP_SUNSTUDY_SUNSTUDYSAMPLESET_HPP
#define EVP_SUNSTUDY_SUNSTUDYSAMPLESET_HPP

// SunStudy/SunStudySampleSet -- the analysis points a study traces, and the
// marker a reuse map gives a sample that inherits nothing.

#include <cstddef>

namespace evp::sunstudy {

// The analysis points. Non-owning: the caller keeps the arrays alive.
struct SampleSet {
    // xyz interleaved, `count` positions, already offset off their surface.
    const double* positions = nullptr;

    // xyz interleaved unit normals, or null for "no orientation".
    //
    // ⚠️ WITHOUT NORMALS EVERY SAMPLE IS TREATED AS FACING THE SUN, which is
    // right for a bare ground grid and wrong for a facade: the back of a wall
    // would count the sun that strikes its front. Supply them for anything with
    // an orientation.
    const double* normals = nullptr;

    size_t count = 0;
};

// In a reuse map (source sample per target sample): this target sample has no
// proven-unaffected source and is traced afresh.
inline constexpr size_t kNoReuse = static_cast<size_t> (-1);

} // namespace evp::sunstudy

#endif
