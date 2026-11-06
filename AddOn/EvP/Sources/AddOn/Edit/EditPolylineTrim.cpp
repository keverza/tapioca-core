#include "Edit/EditPolyline.hpp"
#include "Edit/EditPolylineDetail.hpp"
#include "Edit/EditShape.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace geomsrv::edit {
namespace {

constexpr double kParameterTolerance = 1.0e-9;
constexpr double kStationTolerance = 1.0e-8; // metres along the source's straight-chord traversal

struct Crossing {
    double station = 0.0;
    Id segment = kNoId; // source segment, stable while a later crossing is inserted
    Id vertex = kNoId;  // set only at this station's original vertex, never a coincident later one
    Vec3 at;
};

struct TrimPlan {
    Id entity = kNoId, hovered = kNoId;
    bool closed = false;
    double total = 0.0;
    Crossing first, second;
    std::vector<Vec3> removed;
};

bool Straight (const Entity& e)
{
    if (e.ellipse)
        return false;
    for (const auto& v : e.vertices)
        if (v.radius > 0.0)
            return false;
    for (size_t i = 0; i < SegmentCount (e); ++i)
        if (SegmentCurved (e, i))
            return false;
    return true;
}

struct Bounds {
    double minX = std::numeric_limits<double>::infinity (), minY = minX;
    double maxX = -minX, maxY = maxX;
    bool valid = false;
};

void Include (Bounds& bounds, Vec3 p)
{
    bounds.minX = std::min (bounds.minX, p.x);
    bounds.minY = std::min (bounds.minY, p.y);
    bounds.maxX = std::max (bounds.maxX, p.x);
    bounds.maxY = std::max (bounds.maxY, p.y);
    bounds.valid = true;
}

Bounds BoundsOf (const Entity& e)
{
    Bounds bounds;
    for (const auto& v : e.vertices) {
        Include (bounds, v.p);
        // Conservative curve bounds: a cubic lies inside its control-point hull.
        Include (bounds, v.p + v.in);
        Include (bounds, v.p + v.out);
    }
    return bounds;
}

bool Overlaps (const Bounds& a, const Bounds& b)
{
    return a.valid && b.valid && a.minX <= b.maxX && a.maxX >= b.minX && a.minY <= b.maxY && a.maxY >= b.minY;
}

double Clamped (double t)
{
    return std::clamp (t, 0.0, 1.0);
}

void Ordered (std::vector<Crossing>& crossings)
{
    std::sort (crossings.begin (), crossings.end (),
               [] (const Crossing& a, const Crossing& b) { return a.station < b.station; });
    std::vector<Crossing> unique;
    for (const auto& crossing : crossings) {
        if (unique.empty () || crossing.station - unique.back ().station > kStationTolerance)
            unique.push_back (crossing);
        else if (unique.back ().vertex == kNoId && crossing.vertex != kNoId)
            unique.back () = crossing; // preserve the original vertex at this exact station
    }
    crossings = std::move (unique);
}

bool Inside (double at, double start, double end, bool last)
{
    return at >= start - kStationTolerance && (at < end - kStationTolerance || (last && at <= end + kStationTolerance));
}

// The removed arc includes its two cut points and every original vertex strictly between them.
// Rings rotate their vertex stations into the chosen interval, including the wrap crossing.
std::vector<Vec3> RemovedArc (const Entity& e, const std::vector<double>& stations, const Crossing& first,
                              const Crossing& second)
{
    std::vector<Vec3> removed { first.at };
    std::vector<std::pair<double, Vec3>> interior;
    for (size_t i = 0; i < e.vertices.size (); ++i) {
        double station = stations[i];
        if (e.closed && first.station > kStationTolerance && station <= first.station + kStationTolerance)
            station += stations.back () + DistanceXY (e.vertices.back ().p, e.vertices.front ().p);
        if (station > first.station + kStationTolerance && station < second.station - kStationTolerance)
            interior.push_back ({ station, e.vertices[i].p });
    }
    std::sort (interior.begin (), interior.end (), [] (const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& entry : interior)
        removed.push_back (entry.second);
    removed.push_back (second.at);
    return removed;
}

bool LockedInterior (const Entity& e, const std::vector<double>& stations, double total, const Crossing& first,
                     const Crossing& second, std::string& error)
{
    // An open end is a boundary of the removed interval, but unlike a crossing boundary it
    // has no copy in the surviving piece. It is deleted and therefore protected by either lock.
    const auto locked = [] (const Vertex& v) { return v.lockXY || v.lockZ; };
    if (!e.closed && ((first.station <= kStationTolerance && locked (e.vertices.front ())) ||
                      (total - second.station <= kStationTolerance && locked (e.vertices.back ())))) {
        error = "trimming would remove a locked endpoint";
        return true;
    }
    for (size_t i = 0; i < e.vertices.size (); ++i) {
        double station = stations[i];
        if (e.closed && first.station > kStationTolerance && station <= first.station + kStationTolerance)
            station += total;
        if (station > first.station + kStationTolerance && station < second.station - kStationTolerance &&
            (e.vertices[i].lockXY || e.vertices[i].lockZ)) {
            error = "trimming would remove a locked vertex";
            return true;
        }
    }
    return false;
}

bool Plan (const Document& doc, Id entity, Id segment, Vec3 pointer, TrimPlan& plan, std::string& error)
{
    error.clear ();
    const Entity* e = Find (doc, entity);
    if (e == nullptr || e->kind != Kind::Polyline) {
        error = "pick a polyline to trim";
        return false;
    }
    if (IsCompound (*e)) {
        error = "trimming an outline with holes is not supported; its holes would be left unbounded";
        return false;
    }
    if (!Straight (*e)) {
        error = "trim between crossings currently requires a straight-segment polyline";
        return false;
    }
    const int hoveredIndex = SegmentIndex (*e, segment);
    if (hoveredIndex < 0 || !Finite (pointer)) {
        error = "the hovered segment no longer exists";
        return false;
    }
    const Bounds sourceBounds = BoundsOf (*e);
    for (const auto& cutter : doc.entities)
        if (cutter.id != entity && !Straight (cutter) && Overlaps (sourceBounds, BoundsOf (cutter))) {
            error = "trim between crossings currently requires straight-segment cutters near the source";
            return false;
        }

    const size_t count = SegmentCount (*e);
    std::vector<double> lengths (count), stations (e->vertices.size ());
    double total = 0.0;
    for (size_t i = 0; i < count; ++i) {
        size_t from = 0, to = 0;
        SegmentEnds (*e, i, from, to);
        lengths[i] = DistanceXY (e->vertices[from].p, e->vertices[to].p);
        if (lengths[i] <= kStationTolerance) {
            error = "trim between crossings does not support vertical source legs";
            return false;
        }
        if (to < stations.size () && to == from + 1)
            stations[to] = total + lengths[i];
        total += lengths[i];
    }
    if (!(total > 0.0)) {
        error = "the polyline has no plan length to trim";
        return false;
    }

    std::vector<Crossing> crossings;
    for (size_t i = 0; i < count; ++i) {
        size_t from = 0, to = 0;
        SegmentEnds (*e, i, from, to);
        const Vec3 a = e->vertices[from].p, b = e->vertices[to].p;
        for (const auto& cutter : doc.entities) {
            if (cutter.id == entity)
                continue;
            for (const auto& [c, d] : polyline_detail::EdgesOf (cutter)) {
                double t = 0.0, s = 0.0;
                if (!polyline_detail::CrossPlan (a, b, c, d, t, s) || t < -kParameterTolerance ||
                    t > 1.0 + kParameterTolerance || s < -kParameterTolerance || s > 1.0 + kParameterTolerance)
                    continue;
                t = Clamped (t);
                double station = stations[from] + t * lengths[i];
                Vec3 at = Lerp (a, b, t);
                Id sourceSegment = e->segments[i].id;
                Id sourceVertex = kNoId;
                if (Distance (at, a) <= kStationTolerance) {
                    sourceVertex = e->vertices[from].id;
                    at = a;
                    station = stations[from];
                }
                else if (Distance (at, b) <= kStationTolerance) {
                    sourceVertex = e->vertices[to].id;
                    at = b;
                    station = stations[to];
                }
                if (e->closed && (station <= kStationTolerance || total - station <= kStationTolerance)) {
                    station = 0.0;
                    at = e->vertices.front ().p;
                    sourceSegment = e->segments.front ().id;
                    sourceVertex = e->vertices.front ().id;
                }
                else if (!e->closed && (station <= kStationTolerance || total - station <= kStationTolerance)) {
                    continue; // open ends are already boundaries, not additional crossings
                }
                crossings.push_back ({ station, sourceSegment, sourceVertex, at });
            }
        }
    }
    Ordered (crossings);
    if ((!e->closed && crossings.empty ()) || (e->closed && crossings.size () < 2)) {
        error = e->closed ? "a closed trim needs at least two crossings" : "the polyline has no crossing to trim at";
        return false;
    }

    size_t hoverFrom = 0, hoverTo = 0;
    SegmentEnds (*e, static_cast<size_t> (hoveredIndex), hoverFrom, hoverTo);
    const Vec3 a = e->vertices[hoverFrom].p, b = e->vertices[hoverTo].p;
    const double dx = b.x - a.x, dy = b.y - a.y, length2 = dx * dx + dy * dy;
    if (!(length2 > 0.0)) {
        error = "a vertical segment has no plan trim";
        return false;
    }
    const double t = Clamped (((pointer.x - a.x) * dx + (pointer.y - a.y) * dy) / length2);
    double hover = stations[hoverFrom] + t * lengths[static_cast<size_t> (hoveredIndex)];
    if (e->closed && total - hover <= kStationTolerance)
        hover = 0.0;

    std::vector<Crossing> bounds = crossings;
    if (!e->closed) {
        bounds.push_back ({ 0.0, e->segments.front ().id, e->vertices.front ().id, e->vertices.front ().p });
        bounds.push_back ({ total, e->segments.back ().id, e->vertices.back ().id, e->vertices.back ().p });
        Ordered (bounds);
    }
    bool found = false;
    Crossing first, second;
    const size_t intervals = e->closed ? bounds.size () : bounds.size () - 1;
    for (size_t i = 0; i < intervals && !found; ++i) {
        const bool last = i + 1 == intervals;
        first = bounds[i];
        second = bounds[(i + 1) % bounds.size ()];
        if (e->closed && last)
            second.station += total;
        double at = hover;
        if (e->closed && last && at < first.station - kStationTolerance)
            at += total;
        found = Inside (at, first.station, second.station, last);
    }
    if (!found) {
        error = "the pointer is not beside a removable span";
        return false;
    }
    for (const Crossing* cut : { &first, &second })
        for (const double vertexStation : stations) {
            const double cutStation = e->closed ? std::fmod (cut->station, total) : cut->station;
            double distance = std::abs (cutStation - vertexStation);
            if (e->closed)
                distance = std::min (distance, total - distance);
            if (distance > kStationTolerance && distance < kMinSegment) {
                error = "a trim crossing is too close to a vertex";
                return false;
            }
        }
    const double suffix = total - second.station;
    const bool shortOpenSurvivor = !e->closed && ((first.station > kStationTolerance && first.station < kMinSegment) ||
                                                  (suffix > kStationTolerance && suffix < kMinSegment));
    const bool shortRingSurvivor = e->closed && total - (second.station - first.station) < kMinSegment;
    if (shortOpenSurvivor || shortRingSurvivor) {
        error = "the trim would leave a segment shorter than the document minimum";
        return false;
    }
    if (LockedInterior (*e, stations, total, first, second, error))
        return false;

    plan.entity = entity;
    plan.hovered = segment;
    plan.closed = e->closed;
    plan.total = total;
    plan.first = first;
    plan.second = second;
    plan.removed = RemovedArc (*e, stations, first, second);
    return true;
}

bool EnsureCut (Document& doc, Id entity, const Crossing& crossing, Id& created, std::string& error)
{
    const Entity* e = Find (doc, entity);
    if (e == nullptr) {
        error = "the trim source no longer exists";
        return false;
    }
    // A coincident point elsewhere in a self-touching polyline is a different vertex. Only
    // the original vertex recorded at this traversal station can satisfy the cut directly.
    if (crossing.vertex != kNoId && VertexIndex (*e, crossing.vertex) >= 0) {
        created = crossing.vertex;
        return true;
    }
    const int index = SegmentIndex (*e, crossing.segment);
    if (index < 0) {
        error = "a trim boundary no longer exists";
        return false;
    }
    size_t from = 0, to = 0;
    SegmentEnds (*e, static_cast<size_t> (index), from, to);
    if (Distance (e->vertices[from].p, crossing.at) <= kStationTolerance) {
        created = e->vertices[from].id;
        return true;
    }
    if (Distance (e->vertices[to].p, crossing.at) <= kStationTolerance) {
        created = e->vertices[to].id;
        return true;
    }
    return InsertVertex (doc, entity, crossing.segment, crossing.at, error, &created);
}

void Erase (Document& doc, Id entity)
{
    doc.entities.erase (std::remove_if (doc.entities.begin (), doc.entities.end (),
                                        [entity] (const Entity& e) { return e.id == entity; }),
                        doc.entities.end ());
}

// SplitAtVertex gives the tail a fresh entity id and fresh copies of its cut-end vertices.
// When the tail is the surviving piece, keep those item ids but restore the source entity id.
void KeepTail (Document& doc, Id entity, Id tail)
{
    Entity keep = *Find (doc, tail);
    keep.id = entity;
    Erase (doc, tail);
    *Find (doc, entity) = std::move (keep);
}

bool Apply (Document& doc, const TrimPlan& plan, std::string& error)
{
    Id first = kNoId, second = kNoId;
    // Insert later in the ORIGINAL traversal first, not later in an unwrapped trim interval:
    // a ring's wrap end can precede its start on the same segment. Source segment ids and
    // absolute cut points then keep the earlier insertion on the right remaining chord.
    const double firstOriginal = std::fmod (plan.first.station, plan.total);
    const double secondOriginal = std::fmod (plan.second.station, plan.total);
    if (secondOriginal > firstOriginal) {
        if (!EnsureCut (doc, plan.entity, plan.second, second, error) ||
            !EnsureCut (doc, plan.entity, plan.first, first, error))
            return false;
    }
    else {
        if (!EnsureCut (doc, plan.entity, plan.first, first, error) ||
            !EnsureCut (doc, plan.entity, plan.second, second, error))
            return false;
    }

    if (plan.closed) {
        if (!SplitAtVertex (doc, plan.entity, first, error))
            return false;
        Id tail = kNoId;
        if (!SplitAtVertex (doc, plan.entity, second, error, &tail))
            return false;
        KeepTail (doc, plan.entity, tail);
        return true;
    }

    const bool fromStart = plan.first.station <= kStationTolerance;
    const bool toEnd = plan.total - plan.second.station <= kStationTolerance;
    if (fromStart && toEnd) {
        error = "a trim cannot remove the whole polyline";
        return false;
    }
    if (fromStart) {
        Id tail = kNoId;
        if (!SplitAtVertex (doc, plan.entity, second, error, &tail))
            return false;
        KeepTail (doc, plan.entity, tail);
        return true;
    }

    Id tail = kNoId;
    if (toEnd) {
        if (!SplitAtVertex (doc, plan.entity, first, error, &tail))
            return false;
        Erase (doc, tail);
        return true;
    }

    Id suffix = kNoId, middle = kNoId;
    if (!SplitAtVertex (doc, plan.entity, second, error, &suffix) ||
        !SplitAtVertex (doc, plan.entity, first, error, &middle))
        return false;
    Erase (doc, middle);
    return true;
}

} // namespace

bool FindTrimSpan (const Document& doc, Id entity, Id segment, Vec3 pointer, TrimSpan& span, std::string& error)
{
    TrimPlan plan;
    if (!Plan (doc, entity, segment, pointer, plan, error))
        return false;
    span.entity = entity;
    span.segment = segment;
    span.removed = plan.removed;
    return true;
}

bool TrimBetweenCrossings (Document& doc, Id entity, Id segment, Vec3 pointer, std::string& error)
{
    TrimPlan plan;
    if (!Plan (doc, entity, segment, pointer, plan, error))
        return false;
    Document candidate = doc;
    if (!Apply (candidate, plan, error))
        return false;
    std::string why;
    if (!Validate (candidate, why)) {
        error = "the trim would leave invalid geometry: " + why;
        return false;
    }
    doc = std::move (candidate);
    return true;
}

} // namespace geomsrv::edit
