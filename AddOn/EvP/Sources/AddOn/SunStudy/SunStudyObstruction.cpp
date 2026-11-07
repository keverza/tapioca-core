#include "SunStudy/SunStudyObstruction.hpp"

#include "SunStudy/SunStudyOccluders.hpp"
#include "SunStudy/SunStudyPointProfile.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <algorithm>
#include <cmath>

namespace evp::sunstudy {

namespace {

constexpr double kAttributionTMin = 0.001;

bool FirstHit (const geomsrv::QueryEngine& engine, const double origin[3], const double direction[3],
               geomsrv::QueryEngine::RayHit& hit, std::string& error)
{
    hit = engine.Raycast (origin, direction, 0.0);
    if (!hit.hit)
        return true;
    const auto& snapshot = engine.SourceSnapshot ();
    if (snapshot == nullptr || hit.meshIndex >= snapshot->meshes.size ()) {
        error = "the obstruction hit is outside its source snapshot";
        return false;
    }
    size_t firstTriangle = 0;
    for (size_t mesh = 0; mesh < hit.meshIndex; ++mesh)
        firstTriangle += snapshot->meshes[mesh].TriangleCount ();
    const size_t triangleCount = snapshot->meshes[hit.meshIndex].TriangleCount ();
    if (hit.tri < firstTriangle || hit.tri >= firstTriangle + triangleCount) {
        error = "the obstruction triangle is outside its source mesh";
        return false;
    }
    return true;
}

} // namespace

bool AttributeSunObstruction (const StudyRecord& record, const std::vector<size_t>& samples, const SunStep& sun,
                              SunObstructionResult& result)
{
    result = SunObstructionResult ();
    result.minute = std::clamp (sun.time.hour * 60 + sun.time.minute, 0, 1439);
    if (!record.defaultRayBounds) {
        result.error = "exact obstruction attribution needs the study's original custom ray bounds";
        return false;
    }
    const double length = std::sqrt (sun.direction[0] * sun.direction[0] + sun.direction[1] * sun.direction[1] +
                                     sun.direction[2] * sun.direction[2]);
    if (!(length > 0.0) || !std::isfinite (length)) {
        result.error = "exact obstruction attribution needs a finite, nonzero sun direction";
        return false;
    }
    double direction[3];
    for (int axis = 0; axis < 3; ++axis)
        direction[axis] = sun.direction[axis] / length;

    result.cells.reserve (samples.size ());
    for (const size_t sample : samples) {
        SunObstructionCell cell;
        cell.sample = sample;
        result.cells.push_back (std::move (cell));
        SunObstructionCell& out = result.cells.back ();
        if (sample >= record.positions.size () / 3)
            continue;
        const double* normal =
            record.normals.size () == record.positions.size () ? &record.normals[3 * sample] : nullptr;
        if (!FacesSun (normal, direction)) {
            out.state = SunObstructionState::BackFacing;
            continue;
        }
        double origin[3];
        for (int axis = 0; axis < 3; ++axis)
            origin[axis] = record.positions[3 * sample + axis] + kAttributionTMin * direction[axis];

        geomsrv::QueryEngine::RayHit nearest;
        const geomsrv::QueryEngine* source = nullptr;
        bool context = false;
        if (record.occluders != nullptr) {
            const std::pair<std::shared_ptr<const geomsrv::QueryEngine>, bool> engines[] = {
                { record.occluders->analysis, false }, { record.occluders->context, true }
            };
            for (const auto& [engine, isContext] : engines) {
                if (engine == nullptr)
                    continue;
                geomsrv::QueryEngine::RayHit hit;
                if (!FirstHit (*engine, origin, direction, hit, result.error)) {
                    result.cells.clear ();
                    return false;
                }
                if (hit.hit && (!nearest.hit || hit.t < nearest.t)) {
                    nearest = hit;
                    source = engine.get ();
                    context = isContext;
                }
            }
        }
        if (!nearest.hit) {
            out.state = SunObstructionState::Lit;
            continue;
        }
        const auto& snapshot = source->SourceSnapshot ();
        const auto& mesh = snapshot->meshes[nearest.meshIndex];
        size_t firstTriangle = 0;
        for (size_t index = 0; index < nearest.meshIndex; ++index)
            firstTriangle += snapshot->meshes[index].TriangleCount ();
        out.state = SunObstructionState::Blocked;
        out.blockerGuid = mesh.guid;
        out.sourceTriangle = nearest.tri - firstTriangle;
        out.context = context;
        out.distance = kAttributionTMin + nearest.t;
    }
    return true;
}

bool AttributeSunObstructionDay (const StudyRecord& record, const std::vector<size_t>& samples,
                                 SunObstructionDayResult& result, const std::function<bool ()>& cancelled)
{
    result = SunObstructionDayResult ();
    const auto& steps = record.series.Steps ();
    result.steps.reserve (steps.size ());
    for (const SunStep& step : steps) {
        if (cancelled && cancelled ()) {
            result.steps.clear ();
            return false;
        }
        SunObstructionResult attributed;
        if (!AttributeSunObstruction (record, samples, step, attributed)) {
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
