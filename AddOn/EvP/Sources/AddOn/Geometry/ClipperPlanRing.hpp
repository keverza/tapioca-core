#ifndef TAPIOCA_GEOMETRY_CLIPPER_PLAN_RING_HPP
#define TAPIOCA_GEOMETRY_CLIPPER_PLAN_RING_HPP

#include <clipper2/clipper.h>
#include <cmath>

namespace tapioca::geometry {
// The bundled Clipper PointInPolygon template's orientation predicate multiplies
// integer operands even for PointD, truncating sub-metre differences. Classify on
// ClipperD precision-6's binary lattice instead, with a ring-local origin so
// survey translations do not consume the integer coordinate budget.
class ClipperPlanRing {
  public:
    explicit ClipperPlanRing (const Clipper2Lib::PathD& path)
    {
        if (path.empty ())
            return;
        origin = path.front ();
        for (const auto& p : path) {
            Clipper2Lib::Point64 value;
            if (!Convert (p, value)) {
                ring.clear ();
                return;
            }
            ring.push_back (value);
        }
    }
    Clipper2Lib::PointInPolygonResult Locate (Clipper2Lib::PointD point) const
    {
        Clipper2Lib::Point64 value;
        return Convert (point, value) ? Clipper2Lib::PointInPolygon (value, ring)
                                      : Clipper2Lib::PointInPolygonResult::IsOutside;
    }

  private:
    bool Convert (Clipper2Lib::PointD point, Clipper2Lib::Point64& value) const
    {
        constexpr double scale = 1048576.0;
        const double x = (point.x - origin.x) * scale, y = (point.y - origin.y) * scale;
        if (!std::isfinite (x) || !std::isfinite (y) || std::abs (x) >= Clipper2Lib::max_coord ||
            std::abs (y) >= Clipper2Lib::max_coord)
            return false;
        value = { static_cast<int64_t> (std::round (x)), static_cast<int64_t> (std::round (y)) };
        return true;
    }
    Clipper2Lib::PointD origin;
    Clipper2Lib::Path64 ring;
};
} // namespace tapioca::geometry
#endif
