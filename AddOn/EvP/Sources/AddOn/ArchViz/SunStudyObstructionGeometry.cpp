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

} // namespace geomsrv::archviz
