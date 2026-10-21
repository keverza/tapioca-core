#ifndef TAPIOCA_EDIT_LANDSCAPE_INTERNAL_HPP
#define TAPIOCA_EDIT_LANDSCAPE_INTERNAL_HPP

#include "Edit/EditDocument.hpp"
#include <clipper2/clipper.h>
#include <map>

namespace tapioca::geometry::landscape_detail {
namespace cp = Clipper2Lib;
using Shape = std::vector<DerivedRegion>;
constexpr int kPrecision = 6;
// Clipper's decimal precision selects this binary lattice internally.
constexpr double kBoundaryEpsilon = 2.0 / 1048576.0;
struct SourceCurve {
    PlanarCurve curve;
    // Each H1 leaving ordinal -> its authored segment; a chord of a live corner's arc
    // carries both incident segments, incoming first.
    std::vector<std::vector<Id>> segments;
};
bool ReadSourceCurve (const geomsrv::edit::Document& doc, const LandscapeSource& source, SourceCurve& out,
                      std::string& error);
DerivedRegion FaceRegion (const PlanarMap& map, const PlanarFace& face, const std::map<Id, SourceCurve>& curves);
void Describe (DerivedRegion& region);
cp::PathsD Paths (const DerivedRegion& region);
cp::PathsD Paths (const Shape& shape);
bool BooleanShape (OperationKind kind, const Shape& a, const Shape& b, Id operation, const std::vector<Id>& order,
                   Shape& out, std::string& error);
bool GeneratedShape (const cp::PathsD& paths, const Shape& parents, Id operation, const std::vector<Id>& order,
                     Shape& out, std::string& error);
bool PointInside (Vec3 point, const std::vector<std::vector<Vec3>>& rings);
bool SourceGuide (const SourceCurve& curve, Shape& guide, std::string& error);
bool SplitShape (const Shape& surface, const Shape& guides, Id operation, const std::vector<Id>& order, Shape& out,
                 std::string& error);
bool BoundaryLess (const std::vector<BoundaryRef>& a, const std::vector<BoundaryRef>& b);
void Diagnostic (LandscapeResult& out, LandscapeDiagnosticCode code, Id op, const std::string& message);
} // namespace tapioca::geometry::landscape_detail
#endif
