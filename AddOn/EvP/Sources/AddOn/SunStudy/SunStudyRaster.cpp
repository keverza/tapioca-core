#include "SunStudy/SunStudyRaster.hpp"

#include <algorithm>
#include <cmath>

namespace evp::sunstudy {

namespace {

double Dot (const Vec3& a, const Vec3& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 Cross (const Vec3& a, const Vec3& b)
{
    return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
}

double Length (const Vec3& v)
{
    return std::sqrt (Dot (v, v));
}

bool Normalise (Vec3& v)
{
    const double len = Length (v);
    if (len <= 0.0)
        return false;
    v[0] /= len;
    v[1] /= len;
    v[2] /= len;
    return true;
}

Mat4 Identity ()
{
    Mat4 m {};
    m[0] = m[5] = m[10] = m[15] = 1.0;
    return m;
}

// Row-major multiply: out = a * b.
Mat4 Multiply (const Mat4& a, const Mat4& b)
{
    Mat4 out {};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k)
                sum += a[row * 4 + k] * b[k * 4 + col];
            out[row * 4 + col] = sum;
        }
    }
    return out;
}

} // namespace

OrthoFrustum FitOrthoFrustum (const Vec3& aabbMin, const Vec3& aabbMax, const Vec3& sunDir, double margin)
{
    OrthoFrustum frustum;

    Vec3 toSun = sunDir;
    if (!Normalise (toSun))
        return frustum; // zero-length sun: below the horizon or unset
    if (aabbMax[0] < aabbMin[0] || aabbMax[1] < aabbMin[1] || aabbMax[2] < aabbMin[2])
        return frustum;

    // The light looks FROM the sun toward the scene.
    const Vec3 L { -toSun[0], -toSun[1], -toSun[2] };

    // ⚠️ THE UP CANDIDATE MUST FLIP NEAR THE POLES. With the sun overhead, L is
    // parallel to +Z and cross(up, L) collapses to a zero vector -- which
    // normalises to NaN and takes the whole frustum with it. Noon on a summer
    // solstice at low latitude is exactly that case, so it is an ordinary input
    // rather than an edge one.
    Vec3 upCandidate { 0.0, 0.0, 1.0 };
    if (std::abs (Dot (L, upCandidate)) > 0.999)
        upCandidate = { 0.0, 1.0, 0.0 };

    Vec3 R = Cross (upCandidate, L);
    if (!Normalise (R))
        return frustum;
    const Vec3 U = Cross (L, R);

    const Vec3& lo = aabbMin;
    const Vec3& hi = aabbMax;
    const Vec3 corners[8] = {
        { lo[0], lo[1], lo[2] }, { hi[0], lo[1], lo[2] }, { lo[0], hi[1], lo[2] }, { lo[0], lo[1], hi[2] },
        { hi[0], hi[1], lo[2] }, { hi[0], lo[1], hi[2] }, { lo[0], hi[1], hi[2] }, { hi[0], hi[1], hi[2] },
    };

    double rMin = 0.0, rMax = 0.0, uMin = 0.0, uMax = 0.0, lMin = 0.0, lMax = 0.0;
    for (int i = 0; i < 8; ++i) {
        const double pr = Dot (corners[i], R);
        const double pu = Dot (corners[i], U);
        const double pl = Dot (corners[i], L);
        if (i == 0) {
            rMin = rMax = pr;
            uMin = uMax = pu;
            lMin = lMax = pl;
            continue;
        }
        rMin = std::min (rMin, pr);
        rMax = std::max (rMax, pr);
        uMin = std::min (uMin, pu);
        uMax = std::max (uMax, pu);
        lMin = std::min (lMin, pl);
        lMax = std::max (lMax, pl);
    }

    frustum.left = rMin - margin;
    frustum.right = rMax + margin;
    frustum.bottom = uMin - margin;
    frustum.top = uMax + margin;
    frustum.nearPlane = lMin - margin; // may be negative; see the header
    frustum.farPlane = lMax + margin;
    frustum.worldWidth = (rMax - rMin) + 2.0 * margin;
    frustum.worldHeight = (uMax - uMin) + 2.0 * margin;
    frustum.worldDepth = frustum.farPlane - frustum.nearPlane;
    frustum.rightAxis = R;
    frustum.upAxis = U;
    frustum.lightAxis = L;
    frustum.valid = true;
    return frustum;
}

Mat4 OrthoProjection (const OrthoFrustum& frustum)
{
    Mat4 proj = Identity ();
    const double l = frustum.left, r = frustum.right;
    const double b = frustum.bottom, t = frustum.top;
    const double n = frustum.nearPlane, f = frustum.farPlane;
    if (r == l || t == b || f == n)
        return proj;

    proj[0] = 2.0 / (r - l);
    proj[5] = 2.0 / (t - b);
    proj[10] = -2.0 / (f - n);
    proj[3] = -(r + l) / (r - l);
    proj[7] = -(t + b) / (t - b);
    proj[11] = -(f + n) / (f - n);
    return proj;
}

Vec3 RtcOrigin (const Vec3& aabbMin, const Vec3& aabbMax)
{
    return { std::round ((aabbMin[0] + aabbMax[0]) * 0.5), std::round ((aabbMin[1] + aabbMax[1]) * 0.5),
             std::round ((aabbMin[2] + aabbMax[2]) * 0.5) };
}

Mat4 LightViewProjection (const OrthoFrustum& frustum, const Vec3* origin)
{
    // A pure rotation: a directional light has no eye point, which is exactly
    // why frustum.nearPlane is normally negative.
    Mat4 view = Identity ();
    view[0] = frustum.rightAxis[0];
    view[1] = frustum.rightAxis[1];
    view[2] = frustum.rightAxis[2];
    view[4] = frustum.upAxis[0];
    view[5] = frustum.upAxis[1];
    view[6] = frustum.upAxis[2];
    view[8] = -frustum.lightAxis[0];
    view[9] = -frustum.lightAxis[1];
    view[10] = -frustum.lightAxis[2];

    if (origin != nullptr) {
        // Points arrive as p - o, so the matrix adds o back before rotating.
        Mat4 shift = Identity ();
        shift[3] = (*origin)[0];
        shift[7] = (*origin)[1];
        shift[11] = (*origin)[2];
        view = Multiply (view, shift);
    }

    return Multiply (OrthoProjection (frustum), view);
}

Vec3 TransformPoint (const Mat4& m, const Vec3& p)
{
    const double x = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
    const double y = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
    const double z = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
    const double w = m[12] * p[0] + m[13] * p[1] + m[14] * p[2] + m[15];
    if (w != 0.0 && w != 1.0)
        return { x / w, y / w, z / w };
    return { x, y, z };
}

double TexelWorldSize (const OrthoFrustum& frustum, uint32_t shadowMapResolution)
{
    if (shadowMapResolution == 0)
        return 0.0;
    const double size = std::max (frustum.worldWidth, frustum.worldHeight);
    return size / static_cast<double> (shadowMapResolution);
}

double DeriveBias (double texelWorldSize, double minBias)
{
    return std::max (2.0 * texelWorldSize, minBias);
}

double DeriveBiasFromFrustum (const OrthoFrustum& frustum, uint32_t shadowMapResolution, double minBias)
{
    return DeriveBias (TexelWorldSize (frustum, shadowMapResolution), minBias);
}

double DepthBiasNdc (double biasMetres, double nearPlane, double farPlane)
{
    const double span = farPlane - nearPlane;
    if (span <= 0.0)
        return 0.0;
    return std::min (0.5, biasMetres / span);
}

double PickBias (const std::vector<OrthoFrustum>& frustums, uint32_t shadowMapResolution, double minBias)
{
    // ⚠️ AN EMPTY SEQUENCE RETURNS THE FLOOR, NOT ZERO. Zero bias is acne on
    // every surface, and "no frustums" happens legitimately whenever every
    // timestep is below the horizon.
    double bias = minBias;
    for (const OrthoFrustum& frustum : frustums) {
        if (!frustum.valid)
            continue;
        bias = std::max (bias, DeriveBiasFromFrustum (frustum, shadowMapResolution, minBias));
    }
    return bias;
}

AccumLayout ComputeAccumLayout (size_t sampleCount, uint32_t maxWidth)
{
    AccumLayout layout;
    if (maxWidth == 0)
        return layout;
    if (sampleCount == 0) {
        layout.valid = true;
        return layout;
    }

    const double root = std::ceil (std::sqrt (static_cast<double> (sampleCount)));
    uint32_t width = static_cast<uint32_t> (std::min (static_cast<double> (maxWidth), std::max (1.0, root)));

    const size_t height = (sampleCount + width - 1) / width;
    if (height > maxWidth)
        return layout; // refused; see the header

    layout.width = width;
    layout.height = static_cast<uint32_t> (height);
    layout.valid = true;
    return layout;
}

void SampleIndexToTexel (size_t sampleIndex, uint32_t accumWidth, uint32_t& column, uint32_t& row)
{
    if (accumWidth == 0) {
        column = 0;
        row = 0;
        return;
    }
    column = static_cast<uint32_t> (sampleIndex % accumWidth);
    row = static_cast<uint32_t> (sampleIndex / accumWidth);
}

double AutoGroundPad (const Vec3& aabbMin, const Vec3& aabbMax, double minPad)
{
    const double height = aabbMax[2] - aabbMin[2];
    const double footprint = std::max (aabbMax[0] - aabbMin[0], aabbMax[1] - aabbMin[1]);
    return std::max (std::max (height, 0.25 * footprint), minPad);
}

GroundGrid MakeGroundSampleGrid (const Vec3& aabbMin, const Vec3& aabbMax, double spacing, double pad, double zOffset,
                                 size_t maxSamples)
{
    GroundGrid grid;
    if (!(spacing > 0.0))
        return grid;
    if (aabbMax[0] < aabbMin[0] || aabbMax[1] < aabbMin[1] || aabbMax[2] < aabbMin[2])
        return grid;

    grid.pad = (pad < 0.0) ? AutoGroundPad (aabbMin, aabbMax) : pad;
    grid.groundZ = aabbMin[2] + zOffset;

    const double xLo = aabbMin[0] - grid.pad;
    const double xHi = aabbMax[0] + grid.pad;
    const double yLo = aabbMin[1] - grid.pad;
    const double yHi = aabbMax[1] + grid.pad;

    // At least one sample per axis, so a degenerate footprint still produces a
    // point rather than an empty study that reads as "no sun anywhere".
    const size_t columns = static_cast<size_t> (std::floor ((xHi - xLo) / spacing)) + 1;
    const size_t rows = static_cast<size_t> (std::floor ((yHi - yLo) / spacing)) + 1;

    // Refuse rather than truncate; see the header.
    if (columns == 0 || rows == 0 || columns > maxSamples || rows > maxSamples / columns)
        return grid;

    grid.columns = columns;
    grid.rows = rows;
    grid.positions.reserve (columns * rows * 3);
    grid.normals.reserve (columns * rows * 3);

    // Centred: the leftover after an exact number of steps is split between the
    // two edges, so the grid is not biased toward the minimum corner.
    const double xStart = xLo + ((xHi - xLo) - static_cast<double> (columns - 1) * spacing) * 0.5;
    const double yStart = yLo + ((yHi - yLo) - static_cast<double> (rows - 1) * spacing) * 0.5;

    for (size_t row = 0; row < rows; ++row) {
        const double y = yStart + static_cast<double> (row) * spacing;
        for (size_t column = 0; column < columns; ++column) {
            grid.positions.push_back (xStart + static_cast<double> (column) * spacing);
            grid.positions.push_back (y);
            grid.positions.push_back (grid.groundZ);
            grid.normals.push_back (0.0);
            grid.normals.push_back (0.0);
            grid.normals.push_back (1.0);
        }
    }

    grid.valid = true;
    return grid;
}

std::vector<double> GridLadder (double areaM2, double targetGrid, double coarseThreshold, double midThreshold)
{
    std::vector<double> ladder;
    if (targetGrid <= 0.0) {
        ladder.push_back (targetGrid);
        return ladder;
    }

    const double estimatedSamples = areaM2 / (targetGrid * targetGrid);
    if (estimatedSamples > coarseThreshold)
        ladder.push_back (targetGrid * 4.0);
    if (estimatedSamples > midThreshold)
        ladder.push_back (targetGrid * 2.0);
    ladder.push_back (targetGrid);
    return ladder;
}

} // namespace evp::sunstudy
