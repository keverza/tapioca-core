#ifndef TAPIOCA_EDIT_PLANAR_INTERNAL_HPP
#define TAPIOCA_EDIT_PLANAR_INTERNAL_HPP

// Shared ingress types for EditPlanarBuild.cpp and EditPlanarFlatten.cpp.
// Normalization owns the source-point/handle metadata; topology consumes the
// resulting segment records without reinterpreting authored provenance.

#include "Edit/EditPlanar.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace tapioca::geometry::detail {

struct NormalizedPoint {
    Vec3 p;
    uint32_t firstOriginal = 0;      // Incoming cubic handle owner.
    uint32_t lastOriginal = 0;       // Leaving segment, arc and piece owner.
    std::vector<uint32_t> originals; // Every duplicate folded into this run.
};

struct NormalizedCurve {
    Id source = kNoId;
    bool closed = false;
    std::vector<NormalizedPoint> points;
    std::vector<double> angles;
    std::vector<Vec3> inHandles;
    std::vector<Vec3> outHandles;
};

struct PieceSegment {
    Vec3 a, b;
    Id source = kNoId;
    uint32_t piece = 0;
    double u0 = 0.0, u1 = 1.0;
    bool openStart = false, openEnd = false;
    uint32_t ordinal = 0; // Index into the flattened input segment table.
};

bool NormalizeCurves (const std::vector<PlanarCurve>& curves, std::vector<NormalizedCurve>& normalized, PlanarMap& out,
                      std::string& error);
bool FlattenCurves (const std::vector<NormalizedCurve>& normalized, const PlanarOptions& options,
                    std::vector<PieceSegment>& segments, PlanarMap& out, std::string& error);

} // namespace tapioca::geometry::detail

#endif
