#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/ExtractionStorySlices.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/HudConsole.hpp" // the Debug tab's console: what the user checks when something fails
#include "ArchViz/SceneCmdQueue.hpp"
#include "ArchViz/SlabBodies.hpp"         // the slabs sliced from their body
#include "ArchViz/StorySliceGeometry.hpp" // ChainUnionSegments, BuildSliceRibbon/Fill
#include "ArchViz/StorySliceSnapshot.hpp" // the same cuts, per storey, for the overlays
#include "ArchViz/StorySliceUnion.hpp"    // UnionLoops
#include "Diagnostics/ApiError.hpp"       // DescribeErr - never print a bare GSErrCode
#include "Geometry/SliceEngine.hpp"       // SliceMesh, IsTangentToPlane

#include <cmath>
#include <cstdio>
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
        hudconsole::Warning ("Model", std::string ("the storeys were not read: no storey slices (") +
                                          evp::DescribeErr (err).ToCStr ().Get () + ")");
        return out;
    }
    if (info.data != nullptr) {
        const short count = info.lastStory - info.firstStory + 1;
        for (short i = 0; i < count; ++i) {
            const API_StoryType& story = (*info.data)[i];
            out.levels.push_back (story.level);
            out.names.push_back (std::string (GS::UniString (story.uName).ToCStr (0, GS::MaxUSize, CC_UTF8).Get ()));
            out.indices.push_back (int (info.firstStory) + int (i));
        }
        // The handle is ours to free -- the SDK allocated it for this call.
        BMKillHandle (reinterpret_cast<GSHandle*> (&info.data));
    }
    return out;
}

void StorySliceAccumulator::Begin (const ProjectStoreys& storeys, bool wanted, bool planCut, double planZ,
                                   bool fullBodyCapture)
{
    loops_.clear ();
    planes_.clear ();
    names_.clear ();
    indices_.clear ();
    // Whatever the storeys: the slabs sliced from their body want them from every pass.
    capture_ = fullBodyCapture ? slabbodies::Capture () : std::map<std::string, uint64_t> {};
    captured_.clear ();
    planCut_ = planCut;
    planZ_ = planZ;
    planLoops_.clear ();
    if (!wanted || storeys.Empty ())
        return;
    planes_ = storeys.levels;
    names_ = storeys.names;
    indices_ = storeys.indices;
    loops_.resize (planes_.size ());
}

void StorySliceAccumulator::Cut (const Mesh& mesh)
{
    if (!capture_.empty () && capture_.count (mesh.guid) != 0)
        captured_.push_back (mesh);
    if (mesh.vertices.empty () || mesh.triangles.empty ())
        return;
    // The plan view's cut: one plane at the storey's cut height, where a wall standing on the
    // storey is cut through -- its outline as the plan draws it, with its openings' gaps.
    if (planCut_ && !(mesh.bounds.Valid () && (mesh.bounds.mn[2] > planZ_ || mesh.bounds.mx[2] < planZ_))) {
        const double z = IsTangentToPlane (mesh.vertices.data (), mesh.VertexCount (), planZ_) ? planZ_ + 1e-6 : planZ_;
        std::vector<Polyline> loops =
            SliceMesh (mesh.vertices.data (), mesh.VertexCount (), mesh.triangles.data (), mesh.TriangleCount (), z);
        for (Polyline& loop : loops)
            planLoops_.push_back (std::move (loop));
    }
    if (planes_.empty ())
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

bool Fill (const std::vector<SliceChain>& chains, double z, StorySliceUpload& upload, double& area)
{
    std::string error;
    if (AppendIndexedSliceFill (chains, z, upload.fill, upload.fillIndices, area, error))
        return true;
    const std::string message = "slice fill at " + std::to_string (z) + " m refused: " + error;
    ArchVizLog ("extraction: " + message);
    hudconsole::Warning ("Model", message + "; outlines retained, area unavailable for this cut");
    return false;
}

} // namespace

void StorySliceAccumulator::FinishAndPush ()
{
    if (!capture_.empty ()) {
        slabbodies::Publish (std::move (captured_), capture_);
        captured_.clear ();
    }
    if (planCut_) {
        // Unioned as a storey is, built as one -- an outline ribbon and a fill -- and pushed
        // alone: an empty set is an answer too (the cut misses the model), and replaces the last.
        auto plan = std::make_unique<StorySliceUpload> ();
        bool measured = true;
        const std::vector<UnionSegment> unioned =
            planLoops_.empty () ? std::vector<UnionSegment> {} : UnionLoops (planLoops_);
        if (!unioned.empty ()) {
            const std::vector<SliceChain> chains = ChainUnionSegments (unioned);
            BuildSliceRibbon (chains, float (planZ_), plan->outline);
            measured = Fill (chains, planZ_, *plan, plan->areaM2);
            plan->storeys = measured ? 1 : 0;
        }
        ArchVizLog (
            "extraction: plan cut at " + std::to_string (planZ_) + " m - " + std::to_string (planLoops_.size ()) +
            " loops, " + std::to_string (unioned.size ()) + " union segments, " +
            (measured ? std::to_string (int64_t (plan->areaM2)) + " m2 cut" : "area unavailable: fill refused"));
        // The plan view's one sign that it works: the walls it found, or that it found none.
        char height[32] = {};
        std::snprintf (height, sizeof (height), "%.2f m", planZ_);
        if (unioned.empty ())
            hudconsole::Warning ("Viewer", std::string ("the plan's cut at ") + height + " meets nothing");
        else if (measured)
            hudconsole::Note ("Viewer", std::string ("the plan's cut at ") + height + ": " +
                                            std::to_string (int64_t (plan->areaM2)) + " m2 of walls");
        SceneCmdQueue::Get ().PushPlanCut (std::move (plan));
        planLoops_.clear ();
    }
    if (planes_.empty ())
        return;

    auto upload = std::make_unique<StorySliceUpload> ();
    std::vector<storeyslices::Storey> perStorey;
    size_t segments = 0;
    bool measured = true;
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
        // Measure the indexed DOUBLE domain once, before producer-side packing
        // into the renderer's existing world-float representation.
        double area = 0;
        if (!Fill (chains, planes_[si], *upload, area)) {
            measured = false;
            continue; // do not publish an authoritative per-storey zero after refusal
        }
        upload->areaM2 += area;
        ++upload->storeys;
        storeyslices::Storey storey;
        storey.index = si < indices_.size () ? indices_[si] : int (si);
        storey.level = planes_[si];
        storey.name = si < names_.size () ? names_[si] : std::string ();
        storey.chains = chains;
        storey.areaM2 = area;
        perStorey.push_back (std::move (storey));
    }

    if (!measured) {
        // A partial sum is not the model's area. Publish only the full outlines
        // and an empty measured snapshot, not successful-storey fragments.
        upload->fill.clear ();
        upload->fillIndices.clear ();
        upload->areaM2 = 0;
        upload->storeys = 0;
        perStorey.clear ();
    }
    ArchVizLog (
        "extraction: story slices - " + std::to_string (upload->storeys) + "/" + std::to_string (planes_.size ()) +
        " storeys cut, " + std::to_string (segments) + " union segments, " + std::to_string (upload->outline.size ()) +
        " outline vertices, " + std::to_string (upload->fill.size ()) + " fill vertices, " +
        std::to_string (upload->fillIndices.size ()) + " fill indices, " +
        (measured ? std::to_string (static_cast<int64_t> (upload->areaM2)) + " m2" : "area unavailable: fill refused"));
    SceneCmdQueue::Get ().PushStorySlices (std::move (upload));
    storeyslices::Publish (std::move (perStorey));

    // The buckets are the whole model's cross-sections; a pass that has pushed
    // them has no further use for them and the next pass refills from empty.
    loops_.clear ();
    loops_.resize (planes_.size ());
}

} // namespace archviz
} // namespace geomsrv
