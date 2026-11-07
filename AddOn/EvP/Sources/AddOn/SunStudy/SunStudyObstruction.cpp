#include "SunStudy/SunStudyObstruction.hpp"

#include "SunStudy/SunStudyOccluders.hpp"
#include "SunStudy/SunStudyPointProfile.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <algorithm>
#include <cmath>

namespace evp::sunstudy {

namespace {

constexpr double kAttributionTMin = 0.001;
constexpr double kFootprintLift = 0.002; // sub-cell rays start just off the surface
constexpr double kDefaultCellSize = 0.5; // metres, when the record has no grid spacing
constexpr double kGrazingDenominator = 1.0e-4;

double Dot (const double a[3], const double b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void Cross (const double a[3], const double b[3], double out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

bool Normalise (double v[3])
{
    const double length = std::sqrt (Dot (v, v));
    if (!(length > 0.0) || !std::isfinite (length))
        return false;
    for (int axis = 0; axis < 3; ++axis)
        v[axis] /= length;
    return true;
}

// The nearer of the analysis and context hits, with the mesh-local triangle.
struct NearestHit {
    bool hit = false;
    double t = 0.0;
    std::string guid;
    size_t localTriangle = 0;
    bool context = false;
    double plane[3] = {}; // the hit face's geometric normal
    double onPlane[3] = {};
};

bool FirstHit (const geomsrv::QueryEngine& engine, const double origin[3], const double direction[3],
               geomsrv::QueryEngine::RayHit& hit, size_t& firstTriangle, std::string& error)
{
    hit = engine.Raycast (origin, direction, 0.0);
    if (!hit.hit)
        return true;
    const auto& snapshot = engine.SourceSnapshot ();
    if (snapshot == nullptr || hit.meshIndex >= snapshot->meshes.size ()) {
        error = "the obstruction hit is outside its source snapshot";
        return false;
    }
    firstTriangle = 0;
    for (size_t mesh = 0; mesh < hit.meshIndex; ++mesh)
        firstTriangle += snapshot->meshes[mesh].TriangleCount ();
    const size_t triangleCount = snapshot->meshes[hit.meshIndex].TriangleCount ();
    if (hit.tri < firstTriangle || hit.tri >= firstTriangle + triangleCount) {
        error = "the obstruction triangle is outside its source mesh";
        return false;
    }
    return true;
}

bool Nearest (const StudyRecord& record, const double origin[3], const double direction[3], NearestHit& out,
              std::string& error)
{
    out = NearestHit ();
    if (record.occluders == nullptr)
        return true;
    const std::pair<std::shared_ptr<const geomsrv::QueryEngine>, bool> engines[] = {
        { record.occluders->analysis, false }, { record.occluders->context, true }
    };
    for (const auto& [engine, isContext] : engines) {
        if (engine == nullptr)
            continue;
        geomsrv::QueryEngine::RayHit hit;
        size_t firstTriangle = 0;
        if (!FirstHit (*engine, origin, direction, hit, firstTriangle, error))
            return false;
        if (!hit.hit || (out.hit && hit.t >= out.t))
            continue;
        const auto& mesh = engine->SourceSnapshot ()->meshes[hit.meshIndex];
        out.hit = true;
        out.t = hit.t;
        out.guid = mesh.guid;
        out.localTriangle = hit.tri - firstTriangle;
        out.context = isContext;
        // The face's own plane, not the smooth normal: the footprint lies on it.
        const uint32_t* index = &mesh.triangles[3 * out.localTriangle];
        double corner[3][3];
        bool valid = true;
        for (int k = 0; k < 3; ++k)
            for (int axis = 0; axis < 3; ++axis) {
                const size_t at = 3ull * index[k] + size_t (axis);
                valid = valid && at < mesh.vertices.size ();
                corner[k][axis] = valid ? mesh.vertices[at] : 0.0;
            }
        const double u[3] = { corner[1][0] - corner[0][0], corner[1][1] - corner[0][1], corner[1][2] - corner[0][2] };
        const double v[3] = { corner[2][0] - corner[0][0], corner[2][1] - corner[0][1], corner[2][2] - corner[0][2] };
        Cross (u, v, out.plane);
        if (!valid || !Normalise (out.plane))
            std::copy (hit.normal, hit.normal + 3, out.plane);
        for (int axis = 0; axis < 3; ++axis)
            out.onPlane[axis] = origin[axis] + hit.t * direction[axis];
    }
    return true;
}

// The cell's sub-cells, each cast along the sun onto the face its own ray hit.
bool Footprint (const StudyRecord& record, const double position[3], const double* normal, const double direction[3],
                int divisions, std::vector<SunObstructionFragment>& fragments, std::string& error)
{
    double n[3] = { 0.0, 0.0, 1.0 };
    if (normal != nullptr) {
        std::copy (normal, normal + 3, n);
        if (!Normalise (n))
            n[0] = n[1] = 0.0, n[2] = 1.0;
    }
    const double axis[3] = { std::abs (n[2]) < 0.9 ? 0.0 : 1.0, 0.0, std::abs (n[2]) < 0.9 ? 1.0 : 0.0 };
    double u[3], v[3];
    Cross (axis, n, u);
    if (!Normalise (u))
        return true;
    Cross (n, u, v);
    const double side = record.gridSpacing > 0.0 ? record.gridSpacing : kDefaultCellSize;
    const double cell = side / double (divisions);
    const auto at = [&] (double a, double b, double out[3]) {
        for (int k = 0; k < 3; ++k)
            out[k] = position[k] + (a - 0.5 * side) * u[k] + (b - 0.5 * side) * v[k] + kFootprintLift * n[k];
    };
    for (int i = 0; i < divisions; ++i)
        for (int j = 0; j < divisions; ++j) {
            double centre[3];
            at ((i + 0.5) * cell, (j + 0.5) * cell, centre);
            double origin[3];
            for (int k = 0; k < 3; ++k)
                origin[k] = centre[k] + kAttributionTMin * direction[k];
            NearestHit hit;
            if (!Nearest (record, origin, direction, hit, error))
                return false;
            if (!hit.hit)
                continue;
            const double denominator = Dot (hit.plane, direction);
            if (std::abs (denominator) < kGrazingDenominator)
                continue;
            SunObstructionFragment fragment;
            fragment.blockerGuid = hit.guid;
            fragment.context = hit.context;
            const double offsets[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
            bool valid = true;
            for (int c = 0; c < 4; ++c) {
                double corner[3];
                at ((i + offsets[c][0]) * cell, (j + offsets[c][1]) * cell, corner);
                const double toPlane[3] = { hit.onPlane[0] - corner[0], hit.onPlane[1] - corner[1],
                                            hit.onPlane[2] - corner[2] };
                const double t = Dot (hit.plane, toPlane) / denominator;
                valid = valid && t > 0.0 && std::isfinite (t);
                for (int k = 0; k < 3; ++k)
                    fragment.corners[c][k] = corner[k] + t * direction[k];
            }
            if (valid)
                fragments.push_back (std::move (fragment));
        }
    return true;
}

} // namespace

int SunObstructionFootprintDivisions (size_t samples)
{
    return samples <= 16 ? 4 : samples <= 64 ? 3 : 2;
}

bool AttributeSunObstruction (const StudyRecord& record, const std::vector<size_t>& samples, const SunStep& sun,
                              SunObstructionResult& result, int footprintDivisions)
{
    result = SunObstructionResult ();
    result.minute = std::clamp (sun.time.hour * 60 + sun.time.minute, 0, 1439);
    if (!record.defaultRayBounds) {
        result.error = "exact obstruction attribution needs the study's original custom ray bounds";
        return false;
    }
    double direction[3] = { sun.direction[0], sun.direction[1], sun.direction[2] };
    if (!Normalise (direction)) {
        result.error = "exact obstruction attribution needs a finite, nonzero sun direction";
        return false;
    }

    result.cells.reserve (samples.size ());
    for (const size_t sample : samples) {
        SunObstructionCell cell;
        cell.sample = sample;
        result.cells.push_back (std::move (cell));
        SunObstructionCell& out = result.cells.back ();
        if (sample >= record.positions.size () / 3)
            continue;
        const double* position = &record.positions[3 * sample];
        std::copy (position, position + 3, out.origin);
        const double* normal =
            record.normals.size () == record.positions.size () ? &record.normals[3 * sample] : nullptr;
        if (!FacesSun (normal, direction)) {
            out.state = SunObstructionState::BackFacing;
            continue;
        }
        double origin[3];
        for (int axis = 0; axis < 3; ++axis)
            origin[axis] = position[axis] + kAttributionTMin * direction[axis];
        NearestHit nearest;
        if (!Nearest (record, origin, direction, nearest, result.error)) {
            result.cells.clear ();
            return false;
        }
        if (footprintDivisions > 0 &&
            !Footprint (record, position, normal, direction, footprintDivisions, out.fragments, result.error)) {
            result.cells.clear ();
            return false;
        }
        if (!nearest.hit) {
            out.state = SunObstructionState::Lit;
            continue;
        }
        out.state = SunObstructionState::Blocked;
        out.blockerGuid = nearest.guid;
        out.sourceTriangle = nearest.localTriangle;
        out.context = nearest.context;
        out.distance = kAttributionTMin + nearest.t;
        std::copy (nearest.onPlane, nearest.onPlane + 3, out.hit);
    }
    return true;
}

bool AttributeSunObstructionDay (const StudyRecord& record, const std::vector<size_t>& samples,
                                 SunObstructionDayResult& result, const std::function<bool ()>& cancelled,
                                 int footprintDivisions, int fromMinute, int toMinute)
{
    result = SunObstructionDayResult ();
    const auto& steps = record.series.Steps ();
    result.steps.reserve (steps.size ());
    for (const SunStep& step : steps) {
        if (cancelled && cancelled ()) {
            result.steps.clear ();
            return false;
        }
        const int minute = step.time.hour * 60 + step.time.minute;
        if (minute < fromMinute || minute >= toMinute)
            continue;
        SunObstructionResult attributed;
        if (!AttributeSunObstruction (record, samples, step, attributed, footprintDivisions)) {
            result.steps.clear ();
            result.error = attributed.error;
            return false;
        }
        result.steps.push_back (std::move (attributed));
    }
    if (cancelled && cancelled ()) {
        result.steps.clear ();
        return false;
    }
    return true;
}

} // namespace evp::sunstudy
