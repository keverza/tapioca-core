#include "Edit/EditPlanar.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>

namespace tapioca::geometry {

namespace {

bool ValidOptions (const PlanarOptions& options, std::string& error)
{
    if (!std::isfinite (options.snapTolerance) || !(options.snapTolerance > 0.0)) {
        error = "snapTolerance must be finite and positive";
        return false;
    }
    if (!std::isfinite (options.gapTolerance) || options.gapTolerance < 0.0) {
        error = "gapTolerance must be finite and non-negative";
        return false;
    }
    if (!std::isfinite (options.chordTolerance) || !(options.chordTolerance > 0.0)) {
        error = "chordTolerance must be finite and positive";
        return false;
    }
    if (!std::isfinite (options.minArea) || options.minArea < 0.0) {
        error = "minArea must be finite and non-negative";
        return false;
    }
    if (options.maxSegments == 0) {
        error = "maxSegments must be at least one";
        return false;
    }
    if (options.maxCandidatePairs == 0) {
        error = "maxCandidatePairs must be at least one";
        return false;
    }
    if (options.maxSplitEvents == 0) {
        error = "maxSplitEvents must be at least one";
        return false;
    }
    if (options.maxSubdivisionDepth == 0) {
        error = "maxSubdivisionDepth must be at least one";
        return false;
    }
    if (!std::isfinite (options.quantizeOriginX) || !std::isfinite (options.quantizeOriginY)) {
        error = "quantize origins must be finite";
        return false;
    }
    if (options.maxSegments > std::numeric_limits<uint32_t>::max ()) {
        error = "maxSegments exceeds the planar graph's 32-bit handles";
        return false;
    }
    return true;
}

void SortDiagnostics (std::vector<PlanarDiagnostic>& diagnostics)
{
    const auto key = [] (const PlanarDiagnostic& diagnostic) {
        return std::tuple { static_cast<int> (diagnostic.code),
                            diagnostic.sourceA,
                            diagnostic.sourceB,
                            diagnostic.at.x,
                            diagnostic.at.y,
                            diagnostic.at.z,
                            diagnostic.amount,
                            diagnostic.message };
    };
    std::sort (diagnostics.begin (), diagnostics.end (),
               [&] (const PlanarDiagnostic& a, const PlanarDiagnostic& b) { return key (a) < key (b); });
    diagnostics.erase (
        std::unique (diagnostics.begin (), diagnostics.end (),
                     [&] (const PlanarDiagnostic& a, const PlanarDiagnostic& b) { return key (a) == key (b); }),
        diagnostics.end ());
}

} // namespace

const char* PlanarDiagnosticCodeName (PlanarDiagnosticCode code)
{
    switch (code) {
        case PlanarDiagnosticCode::OpenGap:
            return "OpenGap";
        case PlanarDiagnosticCode::NearCoincident:
            return "NearCoincident";
        case PlanarDiagnosticCode::Tangent:
            return "Tangent";
        case PlanarDiagnosticCode::SelfIntersection:
            return "SelfIntersection";
        case PlanarDiagnosticCode::Sliver:
            return "Sliver";
        case PlanarDiagnosticCode::Degenerate:
            return "Degenerate";
        case PlanarDiagnosticCode::TooManySegments:
            return "TooManySegments";
        case PlanarDiagnosticCode::WorkLimit:
            return "WorkLimit";
        case PlanarDiagnosticCode::Unbounded:
            return "Unbounded";
        case PlanarDiagnosticCode::FaceKeyAmbiguous:
            return "FaceKeyAmbiguous";
    }
    return "Unknown";
}

bool BuildPlanarMap (const std::vector<PlanarCurve>& curves, const PlanarOptions& options, PlanarMap& out,
                     std::string& error)
{
    out = PlanarMap {};
    error.clear ();
    if (!ValidOptions (options, error))
        return false;

    if (!detail::BuildTopology (curves, options, out, error)) {
        SortDiagnostics (out.diagnostics);
        return false;
    }
    if (!detail::BuildFaces (options, out, error)) {
        SortDiagnostics (out.diagnostics);
        return false;
    }
    std::set<Id> closedSources;
    for (const auto& curve : curves)
        if (curve.closed)
            closedSources.insert (curve.source);
    if (!closedSources.empty ()) {
        std::set<Id> boundedSources;
        for (const auto& cycle : out.cycles) {
            if (!(cycle.area > 0.0))
                continue;
            for (uint32_t halfEdge : cycle.halfEdges) {
                const PlanarEdge& edge = out.edges[out.halfEdges[halfEdge].edge];
                boundedSources.insert (edge.sources.begin (), edge.sources.end ());
            }
        }
        for (Id source : closedSources) {
            if (boundedSources.count (source) != 0)
                continue;
            error = "closed source " + std::to_string (source) + " has no bounded cycle after numerical welding";
            out.diagnostics.push_back ({ PlanarDiagnosticCode::Degenerate, source, kNoId, {}, 0.0, error });
            SortDiagnostics (out.diagnostics);
            return false;
        }
    }
    SortDiagnostics (out.diagnostics);
    return true;
}

} // namespace tapioca::geometry
