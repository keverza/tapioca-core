#include "Edit/EditPolyline.hpp"
#include "Edit/EditPolylineDetail.hpp"
#include "Edit/EditShape.hpp"
#include <algorithm>
#include <cmath>

namespace geomsrv::edit {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kRoundPiecesPerQuarter = 8;

bool StraightInput (const Entity& entity, std::string& error)
{
    if (entity.ellipse) {
        error = "a parametric ellipse is edited by its parameters; convert it to a curve for node edits";
        return false;
    }
    if (IsCompound (entity)) {
        error = "offset of an outline with holes is not supported yet";
        return false;
    }
    for (const auto& vertex : entity.vertices)
        if (vertex.radius > 0.0) {
            error = "offset joins currently require sharp corners, not live-corner radii";
            return false;
        }
    for (size_t i = 0; i < SegmentCount (entity); ++i)
        if (SegmentCurved (entity, i)) {
            error = "offset joins currently require straight segments";
            return false;
        }
    return true;
}

bool BuildOffset (Document& doc, Id entityId, double distance, const OffsetOptions& options, bool strictMiter,
                  std::string& error, Id* created)
{
    error.clear ();
    const Entity* source = Find (doc, entityId);
    if (source == nullptr) {
        error = "the entity no longer exists";
        return false;
    }
    if (!std::isfinite (distance)) {
        error = "the offset distance must be finite";
        return false;
    }
    if (options.join != OffsetJoinStyle::Miter && options.join != OffsetJoinStyle::Bevel &&
        options.join != OffsetJoinStyle::Round) {
        error = "unknown offset join style";
        return false;
    }
    if ((options.join == OffsetJoinStyle::Miter || strictMiter) &&
        (!(options.miterLimit >= 1.0) || !std::isfinite (options.miterLimit))) {
        error = "the offset miter limit must be finite and at least 1";
        return false;
    }
    if (!StraightInput (*source, error))
        return false;

    const size_t n = source->vertices.size (), segmentCount = SegmentCount (*source);
    if (n < 2 || segmentCount == 0) {
        error = "the outline has no segment to offset";
        return false;
    }
    std::vector<Vec3> directions (segmentCount);
    for (size_t i = 0; i < segmentCount; ++i) {
        size_t from = 0, to = 0;
        SegmentEnds (*source, i, from, to);
        double length = 0.0;
        if (!polyline_detail::PlanDirection (source->vertices[from].p, source->vertices[to].p, directions[i], length)) {
            error = "a vertical segment has no plan offset";
            return false;
        }
    }

    // Direct polyline distance is positive left. An extrusion profile is CCW, so positive grows out.
    const double d = source->kind == Kind::Extrusion ? -distance : distance;
    std::vector<std::vector<Vec3>> joins (n);
    for (size_t i = 0; i < n; ++i) {
        const Vec3 p = source->vertices[i].p;
        const bool hasIn = source->closed || i > 0, hasOut = source->closed || i + 1 < n;
        if (d == 0.0) {
            joins[i] = { p };
            continue;
        }
        if (!hasIn || !hasOut) {
            joins[i] = { p + polyline_detail::LeftNormal (directions[hasOut ? i : i - 1]) * d };
            continue;
        }

        const Vec3 in = directions[(i + segmentCount - 1) % segmentCount], out = directions[i % segmentCount];
        const Vec3 normalIn = polyline_detail::LeftNormal (in), normalOut = polyline_detail::LeftNormal (out);
        const Vec3 a = p + normalIn * d, b = p + normalOut * d;
        double t = 0.0, s = 0.0;
        if (!polyline_detail::CrossPlan (a - in, a, b, b + out, t, s)) {
            if (Dot (in, out) <= 0.0) {
                error = "a reversing corner has no deterministic straight offset";
                return false;
            }
            joins[i] = { b }; // collinear: both shifted lines agree within roundoff
            continue;
        }
        Vec3 corner = Lerp (a - in, a, t);
        corner.z = p.z;
        if (!Finite (corner)) {
            error = "an offset corner is not finite";
            return false;
        }
        const double turn = in.x * out.y - in.y * out.x;
        const bool outer = turn * d < 0.0;
        const double miterRatio = DistanceXY (corner, p) / std::abs (d);
        if (strictMiter && miterRatio > options.miterLimit + 1.0e-9) {
            error = "a corner is too sharp to offset within the miter limit";
            return false;
        }
        if (!outer) {
            joins[i] = { corner }; // concave offset corners intersect for every join style
            continue;
        }
        if (DistanceXY (a, b) < kMinSegment) {
            joins[i] = { corner }; // below document resolution: no tiny join segment
            continue;
        }
        if (options.join == OffsetJoinStyle::Bevel ||
            (options.join == OffsetJoinStyle::Miter && miterRatio > options.miterLimit + 1.0e-9)) {
            joins[i] = { a, b };
            continue;
        }
        if (options.join == OffsetJoinStyle::Miter) {
            joins[i] = { corner };
            continue;
        }

        const double sweep = std::atan2 (turn, Dot (normalIn, normalOut));
        // Exact subdivision boundaries must not gain a chord through atan2 roundoff.
        int pieces =
            std::max (1, static_cast<int> (std::ceil (std::abs (sweep) * 2.0 * kRoundPiecesPerQuarter / kPi - 1.0e-9)));
        const double start = std::atan2 (a.y - p.y, a.x - p.x), radius = std::abs (d);
        while (pieces > 1 && 2.0 * radius * std::sin (std::abs (sweep) / (2.0 * pieces)) < kMinSegment)
            --pieces; // coarsen below document resolution rather than emit invalid tiny chords
        joins[i].reserve (size_t (pieces + 1));
        for (int k = 0; k <= pieces; ++k) {
            if (k == 0)
                joins[i].push_back (a);
            else if (k == pieces)
                joins[i].push_back (b);
            else {
                const double angle = start + sweep * k / pieces;
                joins[i].push_back ({ p.x + radius * std::cos (angle), p.y + radius * std::sin (angle), p.z });
            }
        }
    }

    // Every source segment must survive as a forward parallel run between its adjacent joins.
    for (size_t i = 0; i < segmentCount; ++i) {
        size_t from = 0, to = 0;
        SegmentEnds (*source, i, from, to);
        Vec3 direction;
        double length = 0.0;
        if (!polyline_detail::PlanDirection (joins[from].back (), joins[to].front (), direction, length) ||
            Dot (direction, directions[i]) <= 0.0) {
            error = "the offset is larger than the outline allows";
            return false;
        }
    }

    Entity copy = *source;
    copy.id = kNoId;
    copy.vertices.clear ();
    copy.segments.clear ();
    const auto appendVertex = [&] (size_t sourceIndex, Vec3 point, bool joinPiece) {
        Vertex vertex = source->vertices[sourceIndex];
        vertex.id = kNoId;
        vertex.p = point;
        vertex.lockXY = vertex.lockZ = false;
        vertex.radius = 0.0;
        vertex.in = vertex.out = {};
        if (joinPiece)
            vertex.smooth = false;
        copy.vertices.push_back (vertex);
    };
    const auto appendSegment = [&] (size_t sourceIndex) {
        Segment segment = source->segments[sourceIndex];
        segment.id = kNoId;
        copy.segments.push_back (segment);
    };
    appendVertex (0, joins[0].front (), joins[0].size () > 1);
    for (size_t i = 0; i < n; ++i) {
        const size_t arriving = (i + segmentCount - 1) % segmentCount;
        for (size_t j = i == 0 ? 1 : 0; j < joins[i].size (); ++j) {
            appendSegment (arriving); // join pieces keep the arriving segment's width/tag
            appendVertex (i, joins[i][j], joins[i].size () > 1);
        }
    }
    if (source->closed)
        appendSegment (segmentCount - 1);
    if (!polyline_detail::CleanupOffset (copy, error))
        return false;

    Document candidate = doc;
    Normalize (candidate, copy);
    candidate.entities.push_back (copy);
    std::string why;
    if (!Validate (candidate, why)) {
        error = "the offset would leave invalid geometry: " + why;
        return false;
    }
    const Id made = copy.id;
    doc = std::move (candidate);
    if (created != nullptr)
        *created = made;
    return true;
}

} // namespace

bool OffsetEntity (Document& doc, Id entity, double distance, std::string& error, Id* created)
{
    return BuildOffset (doc, entity, distance, OffsetOptions {}, true, error, created);
}

bool OffsetEntity (Document& doc, Id entity, double distance, const OffsetOptions& options, std::string& error,
                   Id* created)
{
    return BuildOffset (doc, entity, distance, options, false, error, created);
}

} // namespace geomsrv::edit
