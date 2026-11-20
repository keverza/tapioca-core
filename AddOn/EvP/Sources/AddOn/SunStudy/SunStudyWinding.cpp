#include "SunStudy/SunStudyWinding.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_map>

namespace evp::sunstudy {

double SignedVolume (const double* vertices, const uint32_t* triangles, const uint32_t* faceList, size_t faceCount)
{
    if (faceCount == 0)
        return 0.0;

    // See the header: centre first, or the sign is noise at survey coordinates.
    double centre[3] = { 0.0, 0.0, 0.0 };
    size_t corners = 0;
    for (size_t f = 0; f < faceCount; ++f) {
        const uint32_t face = faceList[f];
        for (int c = 0; c < 3; ++c) {
            const double* p = vertices + static_cast<size_t> (triangles[face * 3 + c]) * 3;
            centre[0] += p[0];
            centre[1] += p[1];
            centre[2] += p[2];
            ++corners;
        }
    }
    if (corners == 0)
        return 0.0;
    centre[0] /= static_cast<double> (corners);
    centre[1] /= static_cast<double> (corners);
    centre[2] /= static_cast<double> (corners);

    double total = 0.0;
    for (size_t f = 0; f < faceCount; ++f) {
        const uint32_t face = faceList[f];
        const double* p0 = vertices + static_cast<size_t> (triangles[face * 3 + 0]) * 3;
        const double* p1 = vertices + static_cast<size_t> (triangles[face * 3 + 1]) * 3;
        const double* p2 = vertices + static_cast<size_t> (triangles[face * 3 + 2]) * 3;

        const double a[3] = { p0[0] - centre[0], p0[1] - centre[1], p0[2] - centre[2] };
        const double b[3] = { p1[0] - centre[0], p1[1] - centre[1], p1[2] - centre[2] };
        const double c[3] = { p2[0] - centre[0], p2[1] - centre[1], p2[2] - centre[2] };

        const double cross[3] = { b[1] * c[2] - b[2] * c[1], b[2] * c[0] - b[0] * c[2], b[0] * c[1] - b[1] * c[0] };
        total += a[0] * cross[0] + a[1] * cross[1] + a[2] * cross[2];
    }
    return total / 6.0;
}

} // namespace evp::sunstudy
