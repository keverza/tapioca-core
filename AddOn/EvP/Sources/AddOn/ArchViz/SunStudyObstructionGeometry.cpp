#include "ArchViz/SunStudyObstructionGeometry.hpp"

#include "ArchViz/SunStudyPreview.hpp"
#include "SunStudy/SunStudyRoles.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace geomsrv::archviz {

namespace {

constexpr float kFootprintAlpha = 0.85f;

bool AppendQuad (const double corners[4][3], uint32_t rgba, GhPreviewBucket& bucket)
{
    const double u[3] = { corners[1][0] - corners[0][0], corners[1][1] - corners[0][1], corners[1][2] - corners[0][2] };
    const double v[3] = { corners[3][0] - corners[0][0], corners[3][1] - corners[0][1], corners[3][2] - corners[0][2] };
    double normal[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
    const double length = std::sqrt (normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
    if (!(length > 0.0) || !std::isfinite (length))
        return false;
    const size_t first = bucket.meshVertices.size ();
    for (int c = 0; c < 4; ++c) {
        GhPreviewMeshVertex vertex;
        vertex.x = float (corners[c][0]);
        vertex.y = float (corners[c][1]);
        vertex.z = float (corners[c][2]);
        vertex.nx = float (normal[0] / length);
        vertex.ny = float (normal[1] / length);
        vertex.nz = float (normal[2] / length);
        vertex.rgba = rgba;
        bucket.meshVertices.push_back (vertex);
    }
    const uint32_t base = uint32_t (first);
    bucket.meshIndices.insert (bucket.meshIndices.end (), { base, base + 1, base + 2, base, base + 2, base + 3 });
    return true;
}

uint32_t WithAlpha (uint32_t rgba, float alpha)
{
    return (rgba & 0xFFFFFF00u) | uint32_t (std::clamp (alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
}

struct BlockerSteps {
    std::string guid;
    int32_t elemType = 0;
    bool context = false;
    std::map<int, size_t> cellsAt; // minute -> cells it blocks then
};

} // namespace

uint32_t SunObstructionTimeColour (int minute)
{
    return SunStudyTimeOfDayRgba (double (minute));
}

bool BuildSunObstructionGeometry (const geomsrv::Snapshot& snapshot,
                                  const std::vector<evp::sunstudy::SunObstructionResult>& steps, int timestepMinutes,
                                  SunObstructionGeometry& result)
{
    result = SunObstructionGeometry ();
    std::map<std::string, size_t> meshes;
    for (size_t index = 0; index < snapshot.meshes.size (); ++index) {
        const std::string guid = evp::sunstudy::CanonicalGuid (snapshot.meshes[index].guid);
        if (!meshes.emplace (guid, index).second) {
            result.error = "the study snapshot has an ambiguous blocking-object GUID";
            return false;
        }
    }
    const auto meshOf = [&] (const std::string& guid, size_t& index) {
        const auto found = meshes.find (evp::sunstudy::CanonicalGuid (guid));
        if (found == meshes.end ()) {
            result.error = "a blocking object is missing from the study snapshot";
            return false;
        }
        index = found->second;
        return true;
    };
    const bool everyRay = steps.size () <= 1;
    std::map<size_t, BlockerSteps> blockers; // by mesh index
    size_t cellsPerStep = 0;
    for (const auto& step : steps) {
        const uint32_t colour = SunObstructionTimeColour (step.minute);
        cellsPerStep = (std::max) (cellsPerStep, step.cells.size ());
        for (size_t c = 0; c < step.cells.size (); ++c) {
            const auto& cell = step.cells[c];
            switch (cell.state) {
                case evp::sunstudy::SunObstructionState::Lit:
                    ++result.lit;
                    break;
                case evp::sunstudy::SunObstructionState::BackFacing:
                    ++result.backFacing;
                    break;
                case evp::sunstudy::SunObstructionState::Unavailable:
                    ++result.unavailable;
                    break;
                case evp::sunstudy::SunObstructionState::Blocked:
                    ++result.blocked;
                    break;
            }
            for (const auto& fragment : cell.fragments) {
                size_t index = 0;
                if (!meshOf (fragment.blockerGuid, index))
                    return false;
                if (AppendQuad (fragment.corners, WithAlpha (colour, kFootprintAlpha), result.drawables.depthTested))
                    ++result.footprints;
            }
            if (cell.state != evp::sunstudy::SunObstructionState::Blocked)
                continue;
            size_t index = 0;
            if (!meshOf (cell.blockerGuid, index))
                return false;
            if (cell.sourceTriangle >= snapshot.meshes[index].TriangleCount ()) {
                result.error = "a blocking triangle is invalid in the study snapshot";
                return false;
            }
            BlockerSteps& blocker = blockers[index];
            blocker.guid = snapshot.meshes[index].guid;
            blocker.elemType = snapshot.meshes[index].elemType;
            blocker.context = cell.context;
            ++blocker.cellsAt[step.minute];
            if (everyRay || c == 0) {
                const float from[3] = { float (cell.origin[0]), float (cell.origin[1]), float (cell.origin[2]) };
                const float to[3] = { float (cell.hit[0]), float (cell.hit[1]), float (cell.hit[2]) };
                AppendPreviewSegment (from, to, colour, result.drawables.depthTested);
            }
        }
    }
    const int quantum = (std::max) (1, timestepMinutes);
    for (auto& [index, steps] : blockers) {
        SunObstructionBlocker out;
        out.guid = steps.guid;
        out.elemType = steps.elemType;
        out.context = steps.context;
        double shares = 0.0;
        for (const auto& [minute, cells] : steps.cellsAt) {
            if (!out.intervals.empty () && minute <= out.intervals.back ().second)
                out.intervals.back ().second = minute + quantum;
            else
                out.intervals.emplace_back (minute, minute + quantum);
            shares += cellsPerStep > 0 ? double (cells) / double (cellsPerStep) : 0.0;
        }
        out.hours = double (steps.cellsAt.size ()) * double (quantum) / 60.0;
        out.meanCellShare = steps.cellsAt.empty () ? 0.0 : shares / double (steps.cellsAt.size ());
        result.blockers.push_back (std::move (out));
    }
    std::stable_sort (
        result.blockers.begin (), result.blockers.end (),
        [] (const SunObstructionBlocker& a, const SunObstructionBlocker& b) { return a.hours > b.hours; });
    result.blockerObjects = result.blockers.size ();
    return true;
}

void BuildSolarCutGeometry (const evp::sunstudy::SolarCutResult& cut, const double point[3],
                            GhPreviewDrawables& drawables)
{
    const auto triangles = [&drawables] (const std::vector<evp::sunstudy::SolarCutTriangle>& list, uint32_t rgba) {
        for (const auto& triangle : list) {
            double corners[4][3];
            for (int c = 0; c < 3; ++c)
                std::copy (triangle.v[c], triangle.v[c] + 3, corners[c]);
            std::copy (triangle.v[2], triangle.v[2] + 3, corners[3]); // a degenerate quad: one triangle
            AppendQuad (corners, rgba, drawables.depthTested);
        }
    };
    triangles (cut.envelope, kSolarCutEnvelopeRgba);
    triangles (cut.removed, kSolarCutRemovedRgba);
    triangles (cut.caps, kSolarCutCapRgba);
    // The envelope's rays and rim: the surface's edges, so it reads as one sheet.
    const float from[3] = { float (point[0]), float (point[1]), float (point[2]) };
    for (size_t i = 0; i < cut.envelope.size (); ++i) {
        const auto& fan = cut.envelope[i];
        const float a[3] = { float (fan.v[1][0]), float (fan.v[1][1]), float (fan.v[1][2]) };
        const float b[3] = { float (fan.v[2][0]), float (fan.v[2][1]), float (fan.v[2][2]) };
        if (i == 0)
            AppendPreviewSegment (from, a, kSunObstructionPink, drawables.depthTested);
        if (i + 1 == cut.envelope.size ())
            AppendPreviewSegment (from, b, kSunObstructionPink, drawables.depthTested);
        AppendPreviewSegment (a, b, kSunObstructionPink, drawables.depthTested);
    }
}

void AppendSolarCutWonCells (const std::vector<SolarCutWonCell>& cells, double size, GhPreviewDrawables& drawables)
{
    const double half = 0.5 * (size > 0.0 ? size : 0.5);
    for (const auto& cell : cells) {
        double n[3] = { cell.normal[0], cell.normal[1], cell.normal[2] };
        const double length = std::sqrt (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (!(length > 0.0))
            continue;
        for (double& c : n)
            c /= length;
        const double axis[3] = { std::abs (n[2]) < 0.9 ? 0.0 : 1.0, 0.0, std::abs (n[2]) < 0.9 ? 1.0 : 0.0 };
        double u[3] = { axis[1] * n[2] - axis[2] * n[1], axis[2] * n[0] - axis[0] * n[2],
                        axis[0] * n[1] - axis[1] * n[0] };
        const double ul = std::sqrt (u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        for (double& c : u)
            c /= ul;
        const double v[3] = { n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0] };
        double corners[4][3];
        const double signs[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 3; ++k)
                corners[c][k] = cell.position[k] + 0.03 * n[k] + half * (signs[c][0] * u[k] + signs[c][1] * v[k]);
        AppendQuad (corners, kSolarCutWonRgba, drawables.depthTested);
    }
}

} // namespace geomsrv::archviz
