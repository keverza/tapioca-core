#include "ArchViz/MaterialTexture.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv {
namespace archviz {

void FitTexturePixels (TexturePixels& pixels, uint32_t maxSide)
{
    if (!pixels.Valid () || maxSide == 0)
        return;
    while (pixels.width > maxSide || pixels.height > maxSide) {
        const uint32_t w = pixels.width > 1 ? pixels.width / 2 : 1;
        const uint32_t h = pixels.height > 1 ? pixels.height / 2 : 1;
        std::vector<uint8_t> out (size_t (w) * h * 4);
        for (uint32_t y = 0; y < h; ++y) {
            const uint32_t y0 = pixels.height > 1 ? y * 2 : 0;
            const uint32_t y1 = pixels.height > 1 ? y0 + 1 : 0;
            for (uint32_t x = 0; x < w; ++x) {
                const uint32_t x0 = pixels.width > 1 ? x * 2 : 0;
                const uint32_t x1 = pixels.width > 1 ? x0 + 1 : 0;
                for (int c = 0; c < 4; ++c) {
                    const auto at = [&] (uint32_t px, uint32_t py) {
                        return uint32_t (pixels.rgba[(size_t (py) * pixels.width + px) * 4 + c]);
                    };
                    out[(size_t (y) * w + x) * 4 + c] =
                        uint8_t ((at (x0, y0) + at (x1, y0) + at (x0, y1) + at (x1, y1) + 2) / 4);
                }
            }
        }
        pixels.width = w;
        pixels.height = h;
        pixels.rgba = std::move (out);
    }
}

uint64_t TextureKeyOf (const std::string& identity)
{
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : identity) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash == 0 ? 1 : hash;
}

TextureRotation TextureRotationOf (double angleRadians)
{
    TextureRotation r;
    if (!std::isfinite (angleRadians))
        return r;
    const double turn = 6.283185307179586;
    const double wrapped = angleRadians - turn * std::floor (angleRadians / turn); // [0, 2 pi)
    if (wrapped < 1e-6 || turn - wrapped < 1e-6)
        return r;
    r.cosA = float (std::cos (wrapped));
    r.sinA = float (std::sin (wrapped));
    return r;
}

void RotateTextureCoordinate (const TextureRotation& rotation, double& u, double& v)
{
    const double ru = u * rotation.cosA - v * rotation.sinA;
    const double rv = u * rotation.sinA + v * rotation.cosA;
    u = ru;
    v = rv;
}

UvStretch TriangleUvStretch (const float* p0, const float* p1, const float* p2, const float* t0, const float* t1,
                             const float* t2)
{
    // J maps the triangle's plane to (u, v): J E = D with E = [e1 e2] (3x2) and D = [d1 d2]
    // (2x2), so J J^T = D G^-1 D^T with G = E^T E. Its eigenvalues are the squared answers.
    double e1[3], e2[3];
    for (int k = 0; k < 3; ++k) {
        e1[k] = double (p1[k]) - p0[k];
        e2[k] = double (p2[k]) - p0[k];
    }
    const double g11 = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2];
    const double g12 = e1[0] * e2[0] + e1[1] * e2[1] + e1[2] * e2[2];
    const double g22 = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2];
    const double det = g11 * g22 - g12 * g12;
    if (!(det > 1e-18 * (g11 * g22 + 1e-30)))
        return {};
    const double i11 = g22 / det, i12 = -g12 / det, i22 = g11 / det;
    const double d[2][2] = { { double (t1[0]) - t0[0], double (t2[0]) - t0[0] },
                             { double (t1[1]) - t0[1], double (t2[1]) - t0[1] } };
    double m[2][2];
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 2; ++c)
            m[r][c] = d[r][0] * (i11 * d[c][0] + i12 * d[c][1]) + d[r][1] * (i12 * d[c][0] + i22 * d[c][1]);
    const double mean = 0.5 * (m[0][0] + m[1][1]);
    const double spread = std::sqrt (0.25 * (m[0][0] - m[1][1]) * (m[0][0] - m[1][1]) + m[0][1] * m[1][0]);
    UvStretch out;
    out.maxPerUnit = std::sqrt ((std::max) (mean + spread, 0.0));
    out.minPerUnit = std::sqrt ((std::max) (mean - spread, 0.0));
    return out;
}

} // namespace archviz
} // namespace geomsrv
