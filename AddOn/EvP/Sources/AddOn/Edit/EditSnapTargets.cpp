#include "Edit/EditSnapInternal.hpp"
#include <memory>

namespace geomsrv::edit {

using snapgather::Gather;
using snapgather::P2;

SnapTargets::SnapTargets () = default;
SnapTargets::~SnapTargets () = default;
SnapTargets::SnapTargets (SnapTargets&&) noexcept = default;
SnapTargets& SnapTargets::operator= (SnapTargets&&) noexcept = default;

void SnapTargets::Reset ()
{
    index_.reset ();
}

size_t SnapTargets::Points () const
{
    return index_ ? index_->points.size () : 0;
}

size_t SnapTargets::Segments () const
{
    return index_ ? index_->segments.size () : 0;
}

bool SnapTargets::Update (const Document& doc, const SnapContext& context, uint64_t contextKey, const SnapQuery& query)
{
    if (index_ && index_->contextKey == contextKey && index_->For (doc, query))
        return false;
    auto index = std::make_unique<Index> ();
    index->entities = doc.entities;
    index->paths = doc.paths;
    index->contextKey = contextKey;
    index->exclude = query.exclude;
    index->pathStep = query.pathStep;
    index->plane = query.plane;
    index->frame.origin = query.plane.origin;
    PlaneAxes (query.plane, index->frame.u, index->frame.v);
    Gather (doc, context, query, index->points, index->segments);
    // The grid's extent and cell: about one target per cell, at most 512 cells a side.
    double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    const auto grow = [&] (Vec3 p) {
        const P2 q = index->frame.To (p);
        if (!std::isfinite (q.x) || !std::isfinite (q.y))
            return;
        x0 = std::min (x0, q.x), x1 = std::max (x1, q.x);
        y0 = std::min (y0, q.y), y1 = std::max (y1, q.y);
    };
    for (const auto& p : index->points)
        grow (p.p);
    for (const auto& s : index->segments)
        grow (s.a), grow (s.b);
    if (std::isfinite (x0)) {
        const double w = std::max (x1 - x0, 1.0e-6), h = std::max (y1 - y0, 1.0e-6);
        const double n = double (std::max<size_t> (1, index->points.size () + index->segments.size ()));
        index->cell = std::max ({ std::sqrt (w * h / n), w / 512.0, h / 512.0, 1.0e-3 });
        index->x0 = x0, index->y0 = y0;
        index->nx = int (std::floor (w / index->cell)) + 1;
        index->ny = int (std::floor (h / index->cell)) + 1;
        index->pointCells.assign (size_t (index->nx) * size_t (index->ny), {});
        index->segmentCells.assign (index->pointCells.size (), {});
        for (uint32_t i = 0; i < index->points.size (); ++i) {
            const P2 q = index->frame.To (index->points[i].p);
            if (std::isfinite (q.x) && std::isfinite (q.y))
                index->pointCells[size_t (index->Row (q.y)) * size_t (index->nx) + size_t (index->Column (q.x))]
                    .push_back (i);
        }
        for (uint32_t i = 0; i < index->segments.size (); ++i) {
            const P2 a = index->frame.To (index->segments[i].a), b = index->frame.To (index->segments[i].b);
            if (!std::isfinite (a.x + a.y + b.x + b.y))
                continue;
            for (int y = index->Row (std::min (a.y, b.y)); y <= index->Row (std::max (a.y, b.y)); ++y)
                for (int x = index->Column (std::min (a.x, b.x)); x <= index->Column (std::max (a.x, b.x)); ++x)
                    index->segmentCells[size_t (y) * size_t (index->nx) + size_t (x)].push_back (i);
        }
    }
    index_ = std::move (index);
    ++builds;
    return true;
}

} // namespace geomsrv::edit
