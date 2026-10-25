#include "Edit/EditLandscapeEdge.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditPath.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tapioca::geometry {
namespace {
namespace edit = geomsrv::edit;

constexpr double kLevelTolerance = 1.0e-3; // a stair's steps add up to its levels within 1 mm
constexpr double kStraight = 1.0e-6;       // a control point this far off the line keeps it straight

bool Fail (std::string& error, const std::string& message)
{
    error = message;
    return false;
}

std::string Metres (double value)
{
    char text[32];
    std::snprintf (text, sizeof text, "%.3f m", value);
    return text;
}

bool Commit (edit::Document& doc, edit::Document candidate, std::string& error)
{
    if (!ValidateLandscape (candidate, error))
        return false;
    candidate.revision = doc.revision + 1;
    doc = std::move (candidate);
    error.clear ();
    return true;
}

// Plan unit vectors along the baseline and to its left, and its length.
bool Frame (Vec3 start, Vec3 end, Vec3& along, Vec3& left, double& length)
{
    length = std::hypot (end.x - start.x, end.y - start.y);
    if (!(length > kStraight))
        return false;
    along = { (end.x - start.x) / length, (end.y - start.y) / length, 0.0 };
    left = { -along.y, along.x, 0.0 };
    return true;
}

// `count` + 1 evenly spaced values from `from` to `to`, `count` the fewest pieces no longer
// than `step`.
std::vector<double> Spaced (double from, double to, double step)
{
    const int count = std::max (1, static_cast<int> (std::ceil ((to - from) / step - 1.0e-9)));
    std::vector<double> out;
    for (int i = 0; i <= count; ++i)
        out.push_back (from + (to - from) * i / count);
    return out;
}

bool StepOk (double step, std::string& error)
{
    return step > 0.0 && std::isfinite (step) ? true : Fail (error, "a sampling step must be positive");
}
} // namespace

bool EdgeBaseline (const edit::Document& doc, const EdgeSpec& edge, Vec3& start, Vec3& end, std::string& error)
{
    const edit::Path* path = edit::FindPath (doc, edge.pathId);
    if (path == nullptr || path->points.size () < 2)
        return Fail (error, "EdgeBaselineMissing: the edge's baseline path no longer exists");
    const Vec3 a = path->points.front ().p, b = path->points.back ().p;
    Vec3 along, left;
    double length = 0.0;
    if (!Frame (a, b, along, left, length))
        return Fail (error, "EdgeBaselineNotStraight: the baseline has no length");
    for (const auto& point : path->points) {
        const double off = (point.p.x - a.x) * left.x + (point.p.y - a.y) * left.y;
        const double at = (point.p.x - a.x) * along.x + (point.p.y - a.y) * along.y;
        if (std::abs (off) > kStraight * std::max (1.0, length) || at < -kStraight || at > length + kStraight)
            return Fail (error, "EdgeBaselineNotStraight: an edge follows one straight line");
    }
    start = { a.x, a.y, 0.0 };
    end = { b.x, b.y, 0.0 };
    return true;
}

bool ValidateEdge (const edit::Document& doc, const EdgeSpec& edge, const EdgeLimits& limits, std::string& error)
{
    Vec3 start, end;
    if (!EdgeBaseline (doc, edge, start, end, error))
        return false;
    const double height = edge.topZ - edge.bottomZ;
    if (!std::isfinite (height) || !(edge.width > 0.0) || !std::isfinite (edge.width))
        return Fail (error, "EdgeInvalid: an edge needs a finite width and levels");
    switch (edge.kind) {
        case EdgeKind::Curb:
            if (!(height > 0.0))
                return Fail (error, "EdgeLevelsInverted: a curb's upper side is above its lower side");
            if (height > limits.maxCurbHeight + 1.0e-9)
                return Fail (error, "CurbTooHigh: a " + Metres (height) + " step is above the " +
                                        Metres (limits.maxCurbHeight) +
                                        " curb maximum; use a retaining wall or a stair");
            return true;
        case EdgeKind::RetainingWall:
            if (edge.width < kMinWallThickness - 1.0e-9)
                return Fail (error,
                             "WallTooThin: a retaining wall is at least " + Metres (kMinWallThickness) + " thick");
            if (!(height > 0.0))
                return Fail (error, "EdgeLevelsInverted: a wall's top is above its foot");
            if (!(edge.footingDepth >= 0.0))
                return Fail (error, "EdgeInvalid: a footing cannot be negative");
            return true;
        case EdgeKind::Stair: {
            if (!(edge.stairRun > 0.0) || !(edge.stairRise > 0.0) || edge.stairSteps < 1 ||
                !(edge.landingDepth >= 0.0) || !(edge.clearance >= 0.0))
                return Fail (error, "StairInvalid: a stair needs a positive run, rise, step count and width");
            if (std::abs (edge.stairSteps * edge.stairRise - height) > kLevelTolerance)
                return Fail (error, "StairLevelsMismatch: " + std::to_string (edge.stairSteps) + " rises of " +
                                        Metres (edge.stairRise) + " do not climb " + Metres (height));
            const double length = std::hypot (end.x - start.x, end.y - start.y);
            if (edge.stairSteps * edge.stairRun + edge.landingDepth > length + kLevelTolerance)
                return Fail (error, "StairTooLong: the flight and landing are longer than the baseline");
            return true;
        }
    }
    return Fail (error, "EdgeInvalid: unknown edge kind");
}

bool AddLandscapeEdge (edit::Document& doc, EdgeSpec edge, const EdgeLimits& limits, Id& id, std::string& error)
{
    if (!doc.landscape)
        return Fail (error, "an edge belongs to a landscape session");
    if (!ValidateEdge (doc, edge, limits, error))
        return false;
    edit::Document candidate = doc;
    edge.id = edit::NewId (candidate);
    candidate.landscape->edges.push_back (edge);
    if (!Commit (doc, std::move (candidate), error))
        return false;
    id = edge.id;
    return true;
}

bool UpdateLandscapeEdge (edit::Document& doc, const EdgeSpec& edge, const EdgeLimits& limits, std::string& error)
{
    if (FindLandscapeEdge (doc, edge.id) == nullptr)
        return Fail (error, "the edge no longer exists");
    if (*FindLandscapeEdge (doc, edge.id) == edge)
        return Fail (error, "the edge is unchanged");
    if (!ValidateEdge (doc, edge, limits, error))
        return false;
    edit::Document candidate = doc;
    for (auto& e : candidate.landscape->edges)
        if (e.id == edge.id)
            e = edge;
    return Commit (doc, std::move (candidate), error);
}

bool RemoveLandscapeEdge (edit::Document& doc, Id edge, std::string& error)
{
    if (FindLandscapeEdge (doc, edge) == nullptr)
        return Fail (error, "the edge no longer exists");
    edit::Document candidate = doc;
    auto& edges = candidate.landscape->edges;
    edges.erase (std::remove_if (edges.begin (), edges.end (), [&] (const EdgeSpec& e) { return e.id == edge; }),
                 edges.end ());
    return Commit (doc, std::move (candidate), error);
}

const EdgeSpec* FindLandscapeEdge (const edit::Document& doc, Id edge)
{
    if (!doc.landscape)
        return nullptr;
    for (const auto& e : doc.landscape->edges)
        if (e.id == edge)
            return &e;
    return nullptr;
}

// ---- stairs ---------------------------------------------------------------------------

bool BuildStairEnvelope (const edit::Document& doc, const EdgeSpec& edge, StairEnvelope& out, std::string& error)
{
    if (edge.kind != EdgeKind::Stair)
        return Fail (error, "StairInvalid: the edge is not a stair");
    if (!ValidateEdge (doc, edge, {}, error))
        return false;
    Vec3 start, end;
    double length = 0.0;
    if (!EdgeBaseline (doc, edge, start, end, error) || !Frame (start, end, out.along, out.left, length))
        return false;
    out.start = start;
    out.width = edge.width;
    out.run = edge.stairRun;
    out.rise = edge.stairRise;
    out.steps = edge.stairSteps;
    out.landing = edge.landingDepth;
    out.bottomZ = edge.bottomZ;
    out.topZ = edge.topZ;
    out.clearance = edge.clearance;
    return true;
}

bool StairUnderside (const StairEnvelope& stair, Vec3 p, double& z)
{
    const double dx = p.x - stair.start.x, dy = p.y - stair.start.y;
    const double t = dx * stair.along.x + dy * stair.along.y, side = dx * stair.left.x + dy * stair.left.y;
    constexpr double e = 1.0e-9;
    if (t < -e || t > stair.Length () + e || std::abs (side) > stair.width * 0.5 + e)
        return false;
    const double flight = stair.steps * stair.run;
    const double tread =
        t < flight ? stair.bottomZ + (std::floor (std::max (t, 0.0) / stair.run) + 1.0) * stair.rise : stair.topZ;
    z = std::min (tread, stair.topZ) - stair.rise;
    return true;
}

double StairCutHeight (const StairEnvelope& stair, Vec3 p, double terrainZ)
{
    double underside = 0.0;
    return StairUnderside (stair, p, underside) ? std::min (terrainZ, underside - stair.clearance) : terrainZ;
}

bool CheckStairClearance (const StairEnvelope& stair, const HeightAt& height, double step, StairClearance& out,
                          std::string& error)
{
    out = {};
    if (!StepOk (step, error))
        return false;
    // Inset a hair so the samples on the footprint's rim and at step fronts are inside.
    const double inset = 1.0e-7;
    for (double t : Spaced (inset, stair.Length () - inset, step))
        for (double side : Spaced (-stair.width * 0.5 + inset, stair.width * 0.5 - inset, step)) {
            const Vec3 p = stair.start + stair.along * t + stair.left * side;
            double underside = 0.0, ground = 0.0;
            if (!StairUnderside (stair, p, underside) || !height (p.x, p.y, ground))
                continue;
            ++out.samples;
            const double limit = underside - stair.clearance;
            if (ground > limit + 1.0e-9) {
                out.cuts.push_back ({ p.x, p.y, ground });
                out.maxCut = std::max (out.maxCut, ground - limit);
            }
            else
                out.maxGap = std::max (out.maxGap, limit - ground);
        }
    if (out.samples == 0)
        return Fail (error, "StairClearanceUnknown: no terrain under the stair");
    return true;
}

// ---- curbs and walls ------------------------------------------------------------------

bool SampleEdgeFaces (const edit::Document& doc, const EdgeSpec& edge, const HeightAt& height, double step,
                      EdgeFaces& out, std::string& error)
{
    out = {};
    Vec3 start, end, along, left;
    double length = 0.0;
    if (!StepOk (step, error) || !EdgeBaseline (doc, edge, start, end, error) ||
        !Frame (start, end, along, left, length))
        return false;
    const Vec3 high = left * (edge.highSide == EdgeSide::Left ? edge.width * 0.5 : -edge.width * 0.5);
    out.highMin = out.lowMin = INFINITY;
    out.highMax = out.lowMax = -INFINITY;
    for (double t : Spaced (0.0, length, step)) {
        const Vec3 c = start + along * t, h = c + high, l = c - high;
        double zh = 0.0, zl = 0.0;
        if (!height (h.x, h.y, zh) || !height (l.x, l.y, zl))
            continue;
        ++out.samples;
        out.highMin = std::min (out.highMin, zh), out.highMax = std::max (out.highMax, zh);
        out.lowMin = std::min (out.lowMin, zl), out.lowMax = std::max (out.lowMax, zl);
    }
    return out.samples > 0 ? true : Fail (error, "EdgeTerrainUnknown: no terrain along the edge");
}

bool CheckCurbLevels (const edit::Document& doc, const EdgeSpec& edge, const HeightAt& height, double step,
                      double tolerance, std::string& error)
{
    EdgeFaces faces;
    if (!SampleEdgeFaces (doc, edge, height, step, faces, error))
        return false;
    const double high = std::max (std::abs (faces.highMin - edge.topZ), std::abs (faces.highMax - edge.topZ));
    const double low = std::max (std::abs (faces.lowMin - edge.bottomZ), std::abs (faces.lowMax - edge.bottomZ));
    if (high > tolerance)
        return Fail (error, "CurbLevelMismatch: the upper side is up to " + Metres (high) + " off the curb top");
    if (low > tolerance)
        return Fail (error, "CurbLevelMismatch: the lower side is up to " + Metres (low) + " off the curb foot");
    return true;
}

bool CheckWallCoverage (const edit::Document& doc, const EdgeSpec& edge, const HeightAt& height, double step,
                        std::string& error)
{
    EdgeFaces faces;
    if (!SampleEdgeFaces (doc, edge, height, step, faces, error))
        return false;
    const double foot = edge.bottomZ - edge.footingDepth;
    if (faces.highMax > edge.topZ + 1.0e-9)
        return Fail (error, "WallCoverageIncomplete: the high ground is up to " + Metres (faces.highMax - edge.topZ) +
                                " above the wall top");
    if (faces.lowMin < foot - 1.0e-9)
        return Fail (error, "WallCoverageIncomplete: the low ground is up to " + Metres (foot - faces.lowMin) +
                                " below the wall foot");
    return true;
}

// ---- steep jumps ----------------------------------------------------------------------

std::vector<SteepJump> FindSteepJumps (const edit::Document& doc, const std::vector<Vec3>& line, const HeightAt& height,
                                       double step, const EdgeLimits& limits, bool walkway)
{
    std::vector<SteepJump> out;
    std::string error;
    if (!StepOk (step, error) || line.size () < 2)
        return out;
    // The line sampled, each sample with its ground (NaN where there is none).
    std::vector<Vec3> samples;
    for (size_t i = 0; i + 1 < line.size (); ++i) {
        const double length = std::hypot (line[i + 1].x - line[i].x, line[i + 1].y - line[i].y);
        for (double t : Spaced (0.0, 1.0, length > 0.0 ? step / length : 1.0)) {
            if (i > 0 && t == 0.0)
                continue;
            Vec3 p { line[i].x + (line[i + 1].x - line[i].x) * t, line[i].y + (line[i + 1].y - line[i].y) * t, NAN };
            double z = 0.0;
            if (height (p.x, p.y, z))
                p.z = z;
            samples.push_back (p);
        }
    }
    const auto steep = [&] (size_t i) {
        const Vec3 a = samples[i], b = samples[i + 1];
        const double run = std::hypot (b.x - a.x, b.y - a.y);
        return std::isfinite (a.z) && std::isfinite (b.z) && run > 0.0 &&
               std::abs (b.z - a.z) / run * 100.0 > limits.maxGrade;
    };
    for (size_t i = 0; i + 1 < samples.size ();) {
        if (!steep (i)) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j + 1 < samples.size () && steep (j))
            ++j;
        SteepJump jump;
        jump.from = samples[i];
        jump.to = samples[j];
        jump.drop = std::abs (jump.to.z - jump.from.z);
        jump.suggestion = jump.drop <= limits.maxCurbHeight ? EdgeKind::Curb
                          : walkway                         ? EdgeKind::Stair
                                                            : EdgeKind::RetainingWall;
        // An edge whose baseline the jump crosses already carries it.
        const Vec3 mid { (jump.from.x + jump.to.x) * 0.5, (jump.from.y + jump.to.y) * 0.5, 0.0 };
        if (doc.landscape)
            for (const auto& edge : doc.landscape->edges) {
                Vec3 a, b, along, left;
                double length = 0.0;
                if (!EdgeBaseline (doc, edge, a, b, error) || !Frame (a, b, along, left, length))
                    continue;
                const double t = (mid.x - a.x) * along.x + (mid.y - a.y) * along.y;
                const double off = (mid.x - a.x) * left.x + (mid.y - a.y) * left.y;
                const double reach =
                    std::hypot (jump.to.x - jump.from.x, jump.to.y - jump.from.y) * 0.5 + edge.width * 0.5 + step;
                if (t >= -step && t <= length + step && std::abs (off) <= reach) {
                    jump.existing = edge.id;
                    break;
                }
            }
        out.push_back (jump);
        i = j;
    }
    return out;
}

} // namespace tapioca::geometry
