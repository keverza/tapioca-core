#include "ArchViz/MeshGroups.hpp"

#include <algorithm>
#include <numeric>

namespace geomsrv {
namespace archviz {

void BuildMaterialGroups (const std::vector<uint32_t>& triangles,
                          const std::vector<int32_t>&  triMaterial,
                          std::vector<uint32_t>&       outIndices,
                          std::vector<MaterialRange>&  outRanges)
{
    outIndices.clear ();
    outRanges.clear ();

    const size_t triCount = triangles.size () / 3;
    if (triCount == 0)
        return;

    // A truncated material array is a bad extraction, not a reason to crash —
    // see the header. Missing entries read as material 0.
    auto MaterialOf = [&triMaterial] (size_t tri) -> int32_t {
        return tri < triMaterial.size () ? triMaterial[tri] : 0;
    };

    // Sort TRIANGLE ORDER, not the index buffer: three indices move together or
    // the triangles are shredded. This is the whole reason an explicit
    // permutation exists rather than a sort over the flat array.
    std::vector<size_t> order (triCount);
    std::iota (order.begin (), order.end (), size_t (0));
    // stable_sort, not sort: equal materials must keep their input order so the
    // output is reproducible and a checked-in fixture does not rot when the
    // standard library changes its introsort pivot.
    std::stable_sort (order.begin (), order.end (),
                      [&MaterialOf] (size_t a, size_t b) {
                          return MaterialOf (a) < MaterialOf (b);
                      });

    outIndices.resize (triCount * 3);
    outRanges.reserve (8);

    int32_t  runMaterial = MaterialOf (order[0]);
    uint32_t runStartTri = 0;

    for (size_t i = 0; i < triCount; ++i) {
        const size_t src = order[i];
        outIndices[i * 3 + 0] = triangles[src * 3 + 0];
        outIndices[i * 3 + 1] = triangles[src * 3 + 1];
        outIndices[i * 3 + 2] = triangles[src * 3 + 2];

        const int32_t mat = MaterialOf (src);
        if (mat != runMaterial) {
            outRanges.push_back (MaterialRange {
                runMaterial, runStartTri * 3, (uint32_t (i) - runStartTri) * 3 });
            runMaterial = mat;
            runStartTri = uint32_t (i);
        }
    }
    // The final run is never closed by the loop — it has no successor to
    // differ from. Forgetting this drops the LAST material entirely, which
    // looks like one missing surface rather than a loop bug.
    outRanges.push_back (MaterialRange {
        runMaterial, runStartTri * 3, (uint32_t (triCount) - runStartTri) * 3 });
}

}   // namespace archviz
}   // namespace geomsrv
