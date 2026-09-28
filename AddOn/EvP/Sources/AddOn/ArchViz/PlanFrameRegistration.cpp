// ArchViz/PlanFrameRegistration -- see the header.

#include "ArchViz/PlanFrameRegistration.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace planframes {

namespace {

constexpr int kCoarseFactor = 4;
constexpr int kPatch = 64; // full-resolution patch side, samples
constexpr int kHalf = kPatch / 2;
constexpr int kCoarseHalf = kHalf / kCoarseFactor;
constexpr int kCoarseRange = 16; // +-16 coarse samples: +-64 full-resolution samples
constexpr int kFineRange = 4;    // refinement around four times the coarse answer
// The integer basin the refinement starts from, against the previous frame warped
// by the fit so far: wide on the first pass, where the fit came from translation
// matches that a zoom step blurs, and narrow on the second, where it did not.
constexpr int kFirstBasin = 3;
constexpr int kSecondBasin = 1;
constexpr int kRing = 3; // how far out a match's surroundings are read
constexpr int kGrid = 3;
constexpr uint32_t kMinWidth = 256;
constexpr uint32_t kMinHeight = 192;
constexpr double kMinTextureStd = 4.0; // grey levels; a line across the patch is far above it
// ⚠️ PER DIRECTION, NOT ON AVERAGE. A patch holding one straight wall line matches
// perfectly anywhere ALONG the line (the aperture problem), and an average over
// all directions still looks sharp because the other direction is. The weakest of
// the four directions is what decides whether the patch located anything.
constexpr double kMinContrast = 0.15;
// The same question asked of the refinement: the weaker eigenvalue of the patch's
// gradient matrix against the stronger. A single straight stroke is near zero.
constexpr double kMinEigenRatio = 0.05;
constexpr double kMaxResidual = 1.0; // samples
// How far a patch may sit from the consensus and still count. The translation
// search is blurred by a zoom step across its own patch, so its answers are only
// good to a couple of samples; the refinement's are good to a small fraction.
constexpr double kLocateTolerance = 2.5;
constexpr double kRefinedTolerance = 0.75;
constexpr uint32_t kMinPatches = 3;

uint8_t Pixel (const GreyImage& image, int x, int y)
{
    return image.pixels[size_t (y) * size_t (image.stride) + size_t (x)];
}

// An owned grey image: the quarter-resolution level.
struct Plane {
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;

    GreyImage View () const
    {
        GreyImage view;
        view.pixels = pixels.data ();
        view.width = uint32_t (width);
        view.height = uint32_t (height);
        view.stride = uint32_t (width);
        return view;
    }
};

Plane Downsample (const GreyImage& image, int factor)
{
    Plane plane;
    plane.width = int (image.width) / factor;
    plane.height = int (image.height) / factor;
    plane.pixels.resize (size_t (plane.width) * size_t (plane.height));
    const int area = factor * factor;
    for (int y = 0; y < plane.height; ++y) {
        for (int x = 0; x < plane.width; ++x) {
            int sum = 0;
            for (int j = 0; j < factor; ++j)
                for (int i = 0; i < factor; ++i)
                    sum += Pixel (image, x * factor + i, y * factor + j);
            plane.pixels[size_t (y) * size_t (plane.width) + size_t (x)] = uint8_t ((sum + area / 2) / area);
        }
    }
    return plane;
}

bool Inside (const GreyImage& image, int left, int top, int size)
{
    return left >= 0 && top >= 0 && left + size <= int (image.width) && top + size <= int (image.height);
}

// Mean absolute difference between the current patch whose top-left is (cx, cy)
// and the previous one displaced by (dx, dy). Negative when the displaced patch
// leaves the previous image.
double PatchDifference (const GreyImage& previous, const GreyImage& current, int left, int top, int size, int dx,
                        int dy)
{
    if (!Inside (previous, left + dx, top + dy, size) || !Inside (current, left, top, size))
        return -1.0;
    uint64_t sum = 0;
    for (int y = 0; y < size; ++y) {
        const uint8_t* a = current.pixels + size_t (top + y) * current.stride + size_t (left);
        const uint8_t* b = previous.pixels + size_t (top + y + dy) * previous.stride + size_t (left + dx);
        for (int x = 0; x < size; ++x)
            sum += uint64_t (std::abs (int (a[x]) - int (b[x])));
    }
    return double (sum) / double (size * size);
}

double PatchStd (const GreyImage& image, int left, int top, int size)
{
    double sum = 0.0;
    double sumSq = 0.0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double v = double (Pixel (image, left + x, top + y));
            sum += v;
            sumSq += v * v;
        }
    }
    const double n = double (size * size);
    const double mean = sum / n;
    const double variance = sumSq / n - mean * mean;
    return variance > 0.0 ? std::sqrt (variance) : 0.0;
}

// The vertex of the parabola through (-1, l), (0, c), (1, r), clamped to half a
// sample: a quantity that should be a fraction and is not says the neighbours
// were not on one valley.
double Parabola (double l, double c, double r)
{
    const double denominator = l - 2.0 * c + r;
    if (!(denominator > 1e-9))
        return 0.0;
    const double offset = 0.5 * (l - r) / denominator;
    return std::max (-0.5, std::min (0.5, offset));
}

// Bilinear sample; false outside the image.
bool Sample (const GreyImage& image, double x, double y, double& value)
{
    if (!(x >= 0.0) || !(y >= 0.0) || x > double (image.width - 1) || y > double (image.height - 1))
        return false;
    const int x0 = std::min (int (x), int (image.width) - 2);
    const int y0 = std::min (int (y), int (image.height) - 2);
    const double fx = x - double (x0);
    const double fy = y - double (y0);
    const double top = double (Pixel (image, x0, y0)) * (1.0 - fx) + double (Pixel (image, x0 + 1, y0)) * fx;
    const double bottom = double (Pixel (image, x0, y0 + 1)) * (1.0 - fx) + double (Pixel (image, x0 + 1, y0 + 1)) * fx;
    value = top * (1.0 - fy) + bottom * fy;
    return true;
}

struct Correspondence {
    double px = 0.0, py = 0.0; // centre in the current frame
    double qx = 0.0, qy = 0.0; // where it was in the previous one
    bool used = true;
};

struct Fit {
    bool ok = false;
    double a = 1.0, b = 0.0, tx = 0.0, ty = 0.0;
};

Fit FitSimilarity (const std::vector<Correspondence>& points)
{
    Fit fit;
    double n = 0.0;
    double mpx = 0.0, mpy = 0.0, mqx = 0.0, mqy = 0.0;
    for (const Correspondence& c : points) {
        if (!c.used)
            continue;
        n += 1.0;
        mpx += c.px;
        mpy += c.py;
        mqx += c.qx;
        mqy += c.qy;
    }
    if (n < double (kMinPatches))
        return fit;
    mpx /= n;
    mpy /= n;
    mqx /= n;
    mqy /= n;
    double denominator = 0.0, sa = 0.0, sb = 0.0;
    for (const Correspondence& c : points) {
        if (!c.used)
            continue;
        const double px = c.px - mpx, py = c.py - mpy, qx = c.qx - mqx, qy = c.qy - mqy;
        denominator += px * px + py * py;
        sa += px * qx + py * qy;
        sb += px * qy - py * qx;
    }
    if (!(denominator > 1e-9))
        return fit;
    fit.a = sa / denominator;
    fit.b = sb / denominator;
    fit.tx = mqx - (fit.a * mpx - fit.b * mpy);
    fit.ty = mqy - (fit.b * mpx + fit.a * mpy);
    fit.ok = std::isfinite (fit.a) && std::isfinite (fit.b) && std::isfinite (fit.tx) && std::isfinite (fit.ty);
    return fit;
}

double Residual (const Fit& fit, const Correspondence& c)
{
    const double x = fit.a * c.px - fit.b * c.py + fit.tx;
    const double y = fit.b * c.px + fit.a * c.py + fit.ty;
    return std::hypot (x - c.qx, y - c.qy);
}

// Fit, then drop the worst correspondence while it is an outlier and enough remain.
// The similarity through two correspondences, which determine it exactly.
bool PairFit (const Correspondence& i, const Correspondence& j, Fit& fit)
{
    const double dpx = j.px - i.px, dpy = j.py - i.py;
    const double dqx = j.qx - i.qx, dqy = j.qy - i.qy;
    const double denominator = dpx * dpx + dpy * dpy;
    if (!(denominator > 1.0))
        return false;
    fit.a = (dpx * dqx + dpy * dqy) / denominator;
    fit.b = (dpx * dqy - dpy * dqx) / denominator;
    fit.tx = i.qx - (fit.a * i.px - fit.b * i.py);
    fit.ty = i.qy - (fit.b * i.px + fit.a * i.py);
    fit.ok = true;
    return true;
}

uint32_t MarkInliers (const Fit& fit, std::vector<Correspondence>& points, double tolerance)
{
    uint32_t inliers = 0;
    for (Correspondence& c : points) {
        c.used = Residual (fit, c) <= tolerance;
        if (c.used)
            ++inliers;
    }
    return inliers;
}

// ⚠️ CONSENSUS FIRST, LEAST SQUARES SECOND. One patch matched to the wrong stroke
// drags a least-squares fit so far that every residual is large and none stands
// out -- measured: a single 60-sample outlier among six patches left a fit 22
// samples out that "drop the worst while it is an outlier" never corrected. With
// at most nine patches every pair can be tried: the similarity two of them fix
// that most others agree with is the motion, and least squares only polishes it.
Fit RobustFit (std::vector<Correspondence>& points, double tolerance, double& rms, uint32_t& used)
{
    Fit best;
    uint32_t bestInliers = 0;
    double bestSpread = 0.0;
    for (size_t i = 0; i < points.size (); ++i) {
        for (size_t j = i + 1; j < points.size (); ++j) {
            Fit candidate;
            if (!PairFit (points[i], points[j], candidate))
                continue;
            const double scale = std::hypot (candidate.a, candidate.b);
            if (!(scale > 0.5 && scale < 2.0))
                continue;
            uint32_t inliers = 0;
            double spread = 0.0;
            for (const Correspondence& c : points) {
                const double r = Residual (candidate, c);
                if (r <= tolerance) {
                    ++inliers;
                    spread += r;
                }
            }
            if (inliers > bestInliers || (inliers == bestInliers && spread < bestSpread)) {
                best = candidate;
                bestInliers = inliers;
                bestSpread = spread;
            }
        }
    }
    rms = 0.0;
    used = 0;
    if (bestInliers < kMinPatches)
        return Fit {};

    Fit fit = best;
    for (int round = 0; round < 2; ++round) {
        if (MarkInliers (fit, points, tolerance) < kMinPatches)
            return Fit {};
        const Fit polished = FitSimilarity (points);
        if (!polished.ok)
            return Fit {};
        fit = polished;
    }
    double sum = 0.0;
    for (const Correspondence& c : points) {
        if (!c.used)
            continue;
        const double r = Residual (fit, c);
        sum += r * r;
        ++used;
    }
    rms = used > 0 ? std::sqrt (sum / double (used)) : 0.0;
    return fit;
}

// The weakest directional contrast of a match: how much worse the match gets
// `kRing` samples away, along each of four directions.
double DirectionalContrast (const GreyImage& previous, const GreyImage& current, int left, int top, int dx, int dy,
                            double best)
{
    static const int kDirections[4][2] = { { kRing, 0 }, { 0, kRing }, { kRing, kRing }, { kRing, -kRing } };
    double weakest = 1.0;
    for (const auto& direction : kDirections) {
        const double forward =
            PatchDifference (previous, current, left, top, kPatch, dx + direction[0], dy + direction[1]);
        const double backward =
            PatchDifference (previous, current, left, top, kPatch, dx - direction[0], dy - direction[1]);
        double ring = 0.0;
        int count = 0;
        if (forward >= 0.0) {
            ring += forward;
            ++count;
        }
        if (backward >= 0.0) {
            ring += backward;
            ++count;
        }
        if (count == 0)
            return 0.0;
        ring /= double (count);
        if (!(ring > 1e-9))
            return 0.0;
        weakest = std::min (weakest, (ring - best) / ring);
    }
    return weakest;
}

// One patch, translation only: coarse exhaustive search, then full-resolution
// refinement with a sub-sample vertex. False when the patch located nothing.
bool LocatePatch (const GreyImage& previous, const GreyImage& current, const GreyImage& coarsePrevious,
                  const GreyImage& coarseCurrent, int centreX, int centreY, double& dx, double& dy)
{
    const int coarseLeft = centreX / kCoarseFactor - kCoarseHalf;
    const int coarseTop = centreY / kCoarseFactor - kCoarseHalf;
    double best = -1.0;
    int bestX = 0, bestY = 0;
    for (int sy = -kCoarseRange; sy <= kCoarseRange; ++sy) {
        for (int sx = -kCoarseRange; sx <= kCoarseRange; ++sx) {
            const double d =
                PatchDifference (coarsePrevious, coarseCurrent, coarseLeft, coarseTop, 2 * kCoarseHalf, sx, sy);
            if (d < 0.0)
                continue;
            // Ties go to the smaller displacement, so a still frame reads as still.
            if (best < 0.0 || d < best ||
                (d == best && std::abs (sx) + std::abs (sy) < std::abs (bestX) + std::abs (bestY))) {
                best = d;
                bestX = sx;
                bestY = sy;
            }
        }
    }
    if (best < 0.0)
        return false;

    const int left = centreX - kHalf;
    const int top = centreY - kHalf;
    double fineBest = -1.0;
    int fx = 0, fy = 0;
    for (int sy = -kFineRange; sy <= kFineRange; ++sy) {
        for (int sx = -kFineRange; sx <= kFineRange; ++sx) {
            const int tx = bestX * kCoarseFactor + sx;
            const int ty = bestY * kCoarseFactor + sy;
            const double d = PatchDifference (previous, current, left, top, kPatch, tx, ty);
            if (d < 0.0)
                continue;
            if (fineBest < 0.0 || d < fineBest ||
                (d == fineBest && std::abs (tx) + std::abs (ty) < std::abs (fx) + std::abs (fy))) {
                fineBest = d;
                fx = tx;
                fy = ty;
            }
        }
    }
    if (fineBest < 0.0)
        return false;
    if (DirectionalContrast (previous, current, left, top, fx, fy, fineBest) < kMinContrast)
        return false;

    const double l = PatchDifference (previous, current, left, top, kPatch, fx - 1, fy);
    const double r = PatchDifference (previous, current, left, top, kPatch, fx + 1, fy);
    const double u = PatchDifference (previous, current, left, top, kPatch, fx, fy - 1);
    const double d = PatchDifference (previous, current, left, top, kPatch, fx, fy + 1);
    dx = double (fx) + ((l >= 0.0 && r >= 0.0) ? Parabola (l, fineBest, r) : 0.0);
    dy = double (fy) + ((u >= 0.0 && d >= 0.0) ? Parabola (u, fineBest, d) : 0.0);
    return true;
}

// Sum of squared differences between the current patch and the previous frame
// warped by `fit` and displaced by (ox, oy), over every `stride`-th sample of the
// patch. Negative when any sample leaves the previous frame.
double WarpedSsd (const GreyImage& previous, const GreyImage& current, int left, int top, const Fit& fit, double ox,
                  double oy, int stride)
{
    double sum = 0.0;
    for (int y = 0; y < kPatch; y += stride) {
        for (int x = 0; x < kPatch; x += stride) {
            const double cx = double (left + x);
            const double cy = double (top + y);
            const double px = fit.a * cx - fit.b * cy + fit.tx + ox;
            const double py = fit.b * cx + fit.a * cy + fit.ty + oy;
            double value = 0.0;
            if (!Sample (previous, px, py, value))
                return -1.0;
            const double e = double (Pixel (current, left + x, top + y)) - value;
            sum += e * e;
        }
    }
    return sum;
}

// One patch against the previous frame warped by the fit so far: the residual
// displacement, which a zoom leaves where a pure translation search cannot see.
//
// ⚠️ GAUSS-NEWTON, NOT A VERTEX FIT. A parabola through three difference values is
// biased by however lopsided the patch's content is -- measured at 0.1-0.3 sample
// on rendered strokes, which at two physical pixels a sample is most of the
// "within a pixel" this instrument has to resolve. Solving for the shift from the
// image gradients converges to a small fraction of a sample, and the gradients'
// own 2x2 matrix says whether the patch can locate BOTH directions at all.
bool RefinePatch (const GreyImage& previous, const GreyImage& current, int centreX, int centreY, const Fit& fit,
                  int basin, double& qx, double& qy)
{
    const int left = centreX - kHalf;
    const int top = centreY - kHalf;
    // The basin first: an integer search, so the solve starts within one sample.
    // Every other sample is enough to find the basin; the solve uses them all.
    double best = -1.0;
    int bx = 0, by = 0;
    for (int sy = -basin; sy <= basin; ++sy) {
        for (int sx = -basin; sx <= basin; ++sx) {
            const double d = WarpedSsd (previous, current, left, top, fit, double (sx), double (sy), 2);
            if (d < 0.0)
                continue;
            if (best < 0.0 || d < best ||
                (d == best && std::abs (sx) + std::abs (sy) < std::abs (bx) + std::abs (by))) {
                best = d;
                bx = sx;
                by = sy;
            }
        }
    }
    if (best < 0.0)
        return false;
    // At the edge of a WIDE window the answer is not a local refinement at all; at
    // the edge of the narrow one it is the fit being a sample out, which the solve
    // below is free to correct.
    if (basin > 1 && (std::abs (bx) == basin || std::abs (by) == basin))
        return false;

    double ox = double (bx);
    double oy = double (by);
    double hxx = 0.0, hxy = 0.0, hyy = 0.0;
    for (int iteration = 0; iteration < 12; ++iteration) {
        hxx = hxy = hyy = 0.0;
        double gx = 0.0, gy = 0.0;
        for (int y = 0; y < kPatch; ++y) {
            for (int x = 0; x < kPatch; ++x) {
                const double cx = double (left + x);
                const double cy = double (top + y);
                const double px = fit.a * cx - fit.b * cy + fit.tx + ox;
                const double py = fit.b * cx + fit.a * cy + fit.ty + oy;
                double v = 0.0, vl = 0.0, vr = 0.0, vu = 0.0, vd = 0.0;
                if (!Sample (previous, px, py, v) || !Sample (previous, px - 1.0, py, vl) ||
                    !Sample (previous, px + 1.0, py, vr) || !Sample (previous, px, py - 1.0, vu) ||
                    !Sample (previous, px, py + 1.0, vd))
                    return false;
                const double dx = 0.5 * (vr - vl);
                const double dy = 0.5 * (vd - vu);
                const double e = double (Pixel (current, left + x, top + y)) - v;
                hxx += dx * dx;
                hxy += dx * dy;
                hyy += dy * dy;
                gx += dx * e;
                gy += dy * e;
            }
        }
        const double determinant = hxx * hyy - hxy * hxy;
        if (!(determinant > 1e-9))
            return false;
        const double stepX = (hyy * gx - hxy * gy) / determinant;
        const double stepY = (hxx * gy - hxy * gx) / determinant;
        ox += stepX;
        oy += stepY;
        if (std::fabs (ox - double (bx)) > 1.5 || std::fabs (oy - double (by)) > 1.5)
            return false; // it walked out of the basin it started in
        if (std::hypot (stepX, stepY) < 1e-3)
            break;
    }
    // The aperture test: the weaker direction must carry a real share of the
    // gradient energy, or the patch located a line, not a place.
    const double trace = hxx + hyy;
    const double gap = std::sqrt (std::max (0.0, (hxx - hyy) * (hxx - hyy) + 4.0 * hxy * hxy));
    const double weak = 0.5 * (trace - gap);
    const double strong = 0.5 * (trace + gap);
    if (!(strong > 0.0) || weak / strong < kMinEigenRatio)
        return false;

    const double cx = double (centreX);
    const double cy = double (centreY);
    qx = fit.a * cx - fit.b * cy + fit.tx + ox;
    qy = fit.b * cx + fit.a * cy + fit.ty + oy;
    return true;
}

} // namespace

FrameMotion MeasureFrameMotion (const GreyImage& previous, const GreyImage& current)
{
    FrameMotion motion;
    if (previous.pixels == nullptr || current.pixels == nullptr || previous.width != current.width ||
        previous.height != current.height || current.width < kMinWidth || current.height < kMinHeight ||
        previous.stride < previous.width || current.stride < current.width) {
        motion.why = "the two frames are missing, differ in size or are too small";
        return motion;
    }

    // Patch centres on a 3x3 grid at a quarter, a half and three quarters of the
    // frame, on multiples of the coarse factor so both levels agree exactly.
    std::vector<int> centres; // x, y pairs of every textured patch
    for (int gy = 1; gy <= kGrid; ++gy) {
        for (int gx = 1; gx <= kGrid; ++gx) {
            const int cx = (int (current.width) * gx / (kGrid + 1)) / kCoarseFactor * kCoarseFactor;
            const int cy = (int (current.height) * gy / (kGrid + 1)) / kCoarseFactor * kCoarseFactor;
            if (!Inside (current, cx - kHalf, cy - kHalf, kPatch))
                continue;
            if (PatchStd (current, cx - kHalf, cy - kHalf, kPatch) < kMinTextureStd)
                continue;
            centres.push_back (cx);
            centres.push_back (cy);
        }
    }
    motion.patchesTextured = uint32_t (centres.size () / 2);
    if (motion.patchesTextured < kMinPatches) {
        motion.why = "fewer than three patches carry any drawing";
        return motion;
    }

    // ⚠️ AN UNCHANGED FRAME IS ANSWERED EXACTLY. Archicad presents without
    // redrawing -- a cursor, a tracker -- and a search would report such a frame as
    // "still, to within the vertex fit" when it is still to the bit.
    bool identical = true;
    for (uint32_t y = 0; y < current.height && identical; ++y)
        identical = std::equal (current.pixels + size_t (y) * current.stride,
                                current.pixels + size_t (y) * current.stride + current.width,
                                previous.pixels + size_t (y) * previous.stride);
    if (identical) {
        motion.patchesUsed = motion.patchesTextured;
        motion.valid = true;
        return motion;
    }

    const Plane coarsePreviousPlane = Downsample (previous, kCoarseFactor);
    const Plane coarseCurrentPlane = Downsample (current, kCoarseFactor);
    const GreyImage coarsePrevious = coarsePreviousPlane.View ();
    const GreyImage coarseCurrent = coarseCurrentPlane.View ();

    std::vector<Correspondence> points;
    for (size_t i = 0; i + 1 < centres.size (); i += 2) {
        double dx = 0.0, dy = 0.0;
        if (!LocatePatch (previous, current, coarsePrevious, coarseCurrent, centres[i], centres[i + 1], dx, dy))
            continue;
        Correspondence c;
        c.px = double (centres[i]);
        c.py = double (centres[i + 1]);
        c.qx = c.px + dx;
        c.qy = c.py + dy;
        points.push_back (c);
    }

    double rms = 0.0;
    uint32_t used = 0;
    Fit fit = RobustFit (points, kLocateTolerance, rms, used);
    if (!fit.ok) {
        motion.why = "fewer than three patches were located";
        return motion;
    }

    // ⚠️ RE-MEASURED AGAINST THE WARPED PREVIOUS FRAME, TWICE AT MOST. The
    // translation search above sees a zoom step as nine different shifts, each
    // blurred by the scale change across its own patch; warping first removes the
    // scale and leaves only what the fit got wrong.
    bool refinedOnce = false;
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<Correspondence> refined;
        for (size_t i = 0; i + 1 < centres.size (); i += 2) {
            double qx = 0.0, qy = 0.0;
            if (!RefinePatch (previous, current, centres[i], centres[i + 1], fit,
                              pass == 0 ? kFirstBasin : kSecondBasin, qx, qy))
                continue;
            Correspondence c;
            c.px = double (centres[i]);
            c.py = double (centres[i + 1]);
            c.qx = qx;
            c.qy = qy;
            refined.push_back (c);
        }
        double refinedRms = 0.0;
        uint32_t refinedUsed = 0;
        const Fit next = RobustFit (refined, kRefinedTolerance, refinedRms, refinedUsed);
        if (!next.ok)
            break;
        const double moved = std::hypot (next.tx - fit.tx, next.ty - fit.ty) +
                             std::hypot (next.a - fit.a, next.b - fit.b) * double (current.width);
        fit = next;
        rms = refinedRms;
        used = refinedUsed;
        refinedOnce = true;
        if (moved < 0.02)
            break;
    }
    // ⚠️ THE TRANSLATION SEARCH IS AN INITIAL GUESS, NEVER THE ANSWER. Its vertex
    // fit is the biased one the refinement exists to replace.
    if (!refinedOnce) {
        motion.patchesUsed = 0;
        motion.why = "the refinement located fewer than three patches";
        return motion;
    }

    motion.a = fit.a;
    motion.b = fit.b;
    motion.offsetX = fit.tx;
    motion.offsetY = fit.ty;
    motion.residual = rms;
    motion.patchesUsed = used;
    const double scale = std::hypot (fit.a, fit.b);
    if (used < kMinPatches) {
        motion.why = "fewer than three patches agree on one motion";
        return motion;
    }
    if (rms > kMaxResidual) {
        motion.why = "the patches disagree: no single pan or zoom carries this frame onto the last";
        return motion;
    }
    if (!(scale > 0.5 && scale < 2.0)) {
        motion.why = "the fitted scale change is outside 0.5..2 between two frames";
        return motion;
    }
    motion.valid = true;
    return motion;
}

void MotionAt (const FrameMotion& motion, double x, double y, double& dx, double& dy)
{
    dx = (motion.a * x - motion.b * y + motion.offsetX) - x;
    dy = (motion.b * x + motion.a * y + motion.offsetY) - y;
}

} // namespace planframes
} // namespace archviz
} // namespace geomsrv
