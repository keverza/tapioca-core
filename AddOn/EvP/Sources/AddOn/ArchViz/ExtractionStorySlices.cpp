#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/ExtractionStorySlices.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/SceneCmdQueue.hpp"
#include "ArchViz/StorySliceGeometry.hpp" // ChainUnionSegments, BuildSliceRibbon/Fill
#include "ArchViz/StorySliceUnion.hpp"    // UnionLoops
#include "Diagnostics/ApiError.hpp"       // DescribeErr - never print a bare GSErrCode
#include "Geometry/SliceEngine.hpp"       // SliceMesh, IsTangentToPlane

#include <cmath>
#include <memory>
#include <string>
#include <utility>

namespace geomsrv {
namespace archviz {

ProjectStoreys ReadStoreys ()
{
    ProjectStoreys out;
    API_StoryInfo info = {};
    const GSErrCode err = ACAPI_ProjectSetting_GetStorySettings (&info);
    if (err != NoError) {
        ArchVizLog (std::string ("extraction: storey read failed, story slices will be empty ") +
                    "(ACAPI_ProjectSetting_GetStorySettings: " + std::string (evp::DescribeErr (err).ToCStr ().Get ()) +
                    ")");
        return out;
    }
    if (info.data != nullptr) {
        const short count = info.lastStory - info.firstStory + 1;
        for (short i = 0; i < count; ++i) {
            const API_StoryType& story = (*info.data)[i];
            out.levels.push_back (story.level);
            out.names.push_back (std::string (GS::UniString (story.uName).ToCStr (0, GS::MaxUSize, CC_UTF8).Get ()));
        }
        // The handle is ours to free -- the SDK allocated it for this call.
        BMKillHandle (reinterpret_cast<GSHandle*> (&info.data));
    }
    return out;
}

void StorySliceAccumulator::Begin (const ProjectStoreys& storeys, bool wanted)
{
    loops_.clear ();
    planes_.clear ();
    if (!wanted || storeys.Empty ())
        return;
    planes_ = storeys.levels;
    loops_.resize (planes_.size ());
}

void StorySliceAccumulator::Cut (const Mesh& mesh)
{
    if (planes_.empty () || mesh.vertices.empty () || mesh.triangles.empty ())
        return;

    for (size_t si = 0; si < planes_.size (); ++si) {
        double z = planes_[si];
        // ⚠️ THE CHEAP REJECT, AND IT IS NOT AN OPTIMISATION HERE THE WAY IT IS IN
        // SliceZ. That function cuts ONE plane; this cuts every storey, so without
        // it each element is walked triangle by triangle once PER STOREY -- twenty
        // times over on a tall building, on every full extraction, to discover
        // nineteen times that a door does not reach the roof.
        if (mesh.bounds.Valid () && (mesh.bounds.mn[2] > z || mesh.bounds.mx[2] < z))
            continue;
        // The tangency lift SliceEngine's header describes: a storey plane sits
        // exactly at the base of every wall rising from it, and a solid the plane
        // is tangent to has no cross-section at all. Cutting a micron above is the
        // intuitive reading -- the storey excludes its own slab and includes the
        // walls standing on it.
        if (IsTangentToPlane (mesh.vertices.data (), mesh.VertexCount (), z))
            z += 1e-6;
        std::vector<Polyline> loops =
            SliceMesh (mesh.vertices.data (), mesh.VertexCount (), mesh.triangles.data (), mesh.TriangleCount (), z);
        for (Polyline& loop : loops)
            loops_[si].push_back (std::move (loop));
    }
}

namespace {

// Total area of the triangles in [begin, end) of a fill buffer, square metres.
double TriangleFanArea (const std::vector<StorySliceFillVertex>& tris, size_t begin)
{
    double area = 0.0;
    for (size_t i = begin; i + 2 < tris.size (); i += 3) {
        const double ax = tris[i].x, ay = tris[i].y;
        const double bx = tris[i + 1].x, by = tris[i + 1].y;
        const double cx = tris[i + 2].x, cy = tris[i + 2].y;
        area += std::fabs ((bx - ax) * (cy - ay) - (cx - ax) * (by - ay)) * 0.5;
    }
    return area;
}

} // namespace

void StorySliceAccumulator::FinishAndPush ()
{
    if (planes_.empty ())
        return;

    auto upload = std::make_unique<StorySliceUpload> ();
    size_t segments = 0;
    for (size_t si = 0; si < loops_.size (); ++si) {
        if (loops_[si].empty ())
            continue; // this plane misses the model entirely
        const std::vector<UnionSegment> unioned = UnionLoops (loops_[si]);
        if (unioned.empty ())
            continue;
        segments += unioned.size ();

        const std::vector<SliceChain> chains = ChainUnionSegments (unioned);
        const float z = static_cast<float> (planes_[si]);
        BuildSliceRibbon (chains, z, upload->outline);
        // ⚠️ THE FILL IS BUILT EVEN THOUGH IT MAY NOT BE DRAWN. Toggling it in the
        // HUD must not need a re-extraction -- that would put a multi-second stall
        // behind a checkbox whose entire purpose is quick comparison.
        const size_t fillBegin = upload->fill.size ();
        BuildSliceFill (chains, z, upload->fill);
        // ⚠️ MEASURED FROM THE TRIANGLES JUST BUILT, NOT VIA UnionArea. That helper
        // is the same decomposition again, and calling it here would run the whole
        // trapezoid sweep TWICE per storey to learn something the triangles already
        // on hand say exactly. It stays the tested definition of the area; this is
        // the same sum over the same triangles.
        upload->areaM2 += TriangleFanArea (upload->fill, fillBegin);
        ++upload->storeys;
    }

    ArchVizLog ("extraction: story slices - " + std::to_string (upload->storeys) + "/" +
                std::to_string (planes_.size ()) + " storeys cut, " + std::to_string (segments) + " union segments, " +
                std::to_string (upload->outline.size ()) + " outline vertices, " +
                std::to_string (upload->fill.size ()) + " fill vertices, " +
                std::to_string (static_cast<int64_t> (upload->areaM2)) + " m2");
    SceneCmdQueue::Get ().PushStorySlices (std::move (upload));

    // The buckets are the whole model's cross-sections; a pass that has pushed
    // them has no further use for them and the next pass refills from empty.
    loops_.clear ();
    loops_.resize (planes_.size ());
}

} // namespace archviz
} // namespace geomsrv
