#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/SunStudyDisplayCommands.hpp"

#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/CommandUtils.hpp"

#include "NativeCommands/SunStudyFollowerDriver.hpp"

#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/SceneCmdQueue.hpp"
#include "ArchViz/SunStudyOverlay.hpp"
#include "Geometry/MeshStore.hpp"
#include "SunStudy/SunStudyAtlas.hpp"
#include "SunStudy/SunStudyPatchAtlas.hpp"
#include "SunStudy/SunStudyPatchSampler.hpp"
#include "SunStudy/SunStudySampler.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <memory>
#include <string>
#include <vector>

namespace geomsrv {

namespace {

using evp::sunstudy::SunStudyStore;

std::string Utf8 (const GS::UniString& text)
{
    return std::string (text.ToCStr (0, MaxUSize, CC_UTF8).Get ());
}

GS::UniString Text (const std::string& text)
{
    return GS::UniString (text.c_str (), CC_UTF8);
}

// The study a verb operates on when it names none -- the same rule as every
// other sun study verb, and deliberately the same three lines: a shared helper
// would have to live in a header both files include, which for three lines is
// more coupling than the duplication costs.
std::string ReadStudyId (const GS::ObjectState& params)
{
    GS::UniString id;
    if (params.Get ("studyId", id) && !id.IsEmpty ())
        return Utf8 (id);

    const std::vector<std::string> ids = SunStudyStore::Get ().Ids ();
    return ids.empty () ? std::string () : ids.back ();
}

GS::Int32 ReadInt (const GS::ObjectState& params, const char* key, GS::Int32 fallback)
{
    GS::Int32 value = 0;
    return params.Get (key, value) ? value : fallback;
}

double ReadDouble (const GS::ObjectState& params, const char* key, double fallback)
{
    double value = 0.0;
    return params.Get (key, value) ? value : fallback;
}

// Tapioca.ShowSunStudy - put a completed study on the Diligent model, or take it
// off again.
//
// This is the PRODUCER half of the display path. It reads the study's atlas and
// per-face layouts, walks the same snapshot the study was sampled from, and
// hands the render thread one side buffer per element through SceneCmdQueue --
// which is where geometry has always flowed, and the reason this is a scene
// command rather than a return value.
//
// GATE-FREE, AND IT CALLS NO ACAPI TO STAY THAT WAY. Everything it needs is
// already resident: the study is in the store and the meshes are in MeshStore.
// Asking Archicad's main thread to show a result that is already computed would
// put a stutter in the one place the whole native core exists to avoid one.
class ShowSunStudyCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "ShowSunStudy";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override;
};

NativeCommandResult ShowSunStudyCommand::ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const
{
    bool show = true;
    params.Get ("show", show);
    if (!show) {
        // THE CLEAR IS UNCONDITIONAL AND NAMES NO STUDY. "Stop showing a
        // study" must work when the study has already been cancelled, when
        // the id is forgotten, and when the viewer is showing one this
        // caller never started -- otherwise a stale tint could only be
        // removed by restarting the viewport.
        archviz::SceneCmdQueue::Get ().PushClearSunStudy ();
        GS::ObjectState cleared;
        cleared.Add ("studyId", Text (std::string ()));
        cleared.Add ("shown", false);
        cleared.Add ("viewerRunning", archviz::DiligentViewport::Get ().IsRunning ());
        cleared.Add ("elements", (GS::Int32) 0);
        cleared.Add ("atlasWidth", (GS::Int32) 0);
        cleared.Add ("atlasHeight", (GS::Int32) 0);
        cleared.Add ("converged", false);
        cleared.Add ("hoursMax", 0.0);
        cleared.Add ("debug", (GS::Int32) 0);
        cleared.Add ("depth", (GS::Int32) 0);
        return cleared;
    }

    const std::string id = ReadStudyId (params);
    if (id.empty ())
        return NativeCommandResult::Failure ("no sun study to show - start one first");

    std::vector<evp::sunstudy::AtlasTile> tiles;
    std::vector<evp::sunstudy::FaceLayout> layouts;
    uint32_t width = 0;
    uint32_t height = 0;
    double spacing = 0.0;
    std::vector<float> image;
    double daylightHours = 0.0;
    bool converged = false;
    uint64_t generation = 0;
    std::string error;
    if (!SunStudyStore::Get ().DisplayData (id, tiles, layouts, width, height, spacing, image, daylightHours, converged,
                                            generation, error))
        return NativeCommandResult::Failure (Text (error));

    // What the study was RUN with, so a replacement can be run the same way.
    // ⚠️ FROM THE STUDY RECORD, NOT FROM THIS COMMAND'S PARAMETERS. ShowSunStudy
    // takes no date and no grid; a follower configured from what was typed here
    // would rerun a different study from the one on screen.
    evp::sunstudy::StudyRecord metadata;
    std::string describeError;
    const bool haveMetadata = SunStudyStore::Get ().Describe (id, metadata, describeError);

    std::shared_ptr<const Snapshot> snapshot = MeshStore::Get ().Current ();
    if (snapshot == nullptr)
        return NativeCommandResult::Failure ("no snapshot is live - call Tapioca.BuildSnapshot first");

    // THE FACE COUNTS MUST AGREE BEFORE ANYTHING IS BUILT. The study's faces
    // are the snapshot's triangles CONCATENATED in mesh order, so a snapshot
    // rebuilt since the study was started shifts every face base -- and a
    // shifted base does not fail, it hands each element some other element's
    // atlas tiles. The per-element topology hash catches the same fault one
    // element at a time; this catches it in one sentence and says what to do
    // about it.
    size_t snapshotFaces = 0;
    for (const Mesh& mesh : snapshot->meshes)
        snapshotFaces += mesh.TriangleCount ();
    if (snapshotFaces != tiles.size ()) {
        return NativeCommandResult::Failure (Text (
            "study '" + id + "' measured " + std::to_string (tiles.size ()) + " faces but the live snapshot has " +
            std::to_string (snapshotFaces) + " - the model was rebuilt under the study; start a new one"));
    }

    auto upload = std::make_unique<archviz::SunStudyAtlasUpload> ();
    upload->studyId = id;
    upload->version = generation;
    upload->width = width;
    upload->height = height;
    upload->texels = std::make_shared<const std::vector<float>> (std::move (image));
    // The study's own daylight length, not its measured maximum: two studies
    // of the same building must be comparable, and normalising each to its
    // own peak makes the darkest scheme look as sunny as the brightest.
    const double rampTop = ReadDouble (params, "hoursMax", daylightHours > 0.0 ? daylightHours : 1.0);
    upload->hoursMax = static_cast<float> (rampTop > 0.0 ? rampTop : 1.0);
    const GS::Int32 debug = ReadInt (params, "debug", 0);
    // Captured before the payload is handed over: `upload` is moved into the
    // queue below and must not be read after that.
    upload->debugMode = static_cast<uint32_t> (debug < 0 ? 0 : (debug > 3 ? 3 : debug));

    uint32_t faceBase = 0;
    size_t built = 0;
    for (const Mesh& mesh : snapshot->meshes) {
        archviz::SunStudyElementMap map;
        map.guid = mesh.guid;
        if (archviz::BuildSunStudyElementMap (tiles, layouts, spacing, mesh.triangles, mesh.triMaterial, faceBase,
                                              map)) {
            upload->elements.push_back (std::move (map));
            ++built;
        }
        faceBase += static_cast<uint32_t> (mesh.TriangleCount ());
    }

    // ⚠️ WHETHER A VIEWPORT EXISTS AT ALL, REPORTED BEFORE THE PUSH. A
    // SceneCmdQueue push cannot fail and cannot reply: the queue is a
    // singleton, so a command aimed at a viewer that is not running simply
    // waits in it until something calls Clear() -- which a viewport start
    // and a palette teardown both do. Without this lane the verb answers
    // "shown: true, elements: 6" for a model nobody has tinted, which is
    // precisely what happened on 2026-09-15 and what made five PASSes in the
    // smoke log describe a study that was never drawn.
    const bool viewerRunning = archviz::DiligentViewport::Get ().IsRunning ();

    GS::ObjectState os;
    os.Add ("studyId", Text (id));
    // ⚠️ `shown` NOW MEANS "A VIEWER WAS RUNNING TO SHOW IT", not "the push
    // succeeded". The push always succeeds and never meant anything.
    os.Add ("shown", viewerRunning);
    os.Add ("viewerRunning", viewerRunning);
    // ⚠️ RENAMED FROM `elements` IN SPIRIT: this is what the PRODUCER built,
    // not what the renderer bound. Tapioca.SunStudyOverlayState is the only
    // thing that can answer the second question.
    os.Add ("elements", (GS::Int32) built);
    os.Add ("atlasWidth", (GS::Int32) width);
    os.Add ("atlasHeight", (GS::Int32) height);
    // REPORTED, NOT ENFORCED. An unconverged study has a real atlas of the
    // hours resolved so far; showing it is legitimate and calling it final
    // is not, so the caller is told which it has rather than refused.
    os.Add ("converged", converged);
    os.Add ("hoursMax", (double) upload->hoursMax);
    os.Add ("debug", (GS::Int32) upload->debugMode);
    os.Add ("depth", (GS::Int32) upload->depthMode);
    const uint32_t adoptedDebug = upload->debugMode;
    const uint32_t adoptedDepth = upload->depthMode;

    archviz::SceneCmdQueue::Get ().PushSunStudyAtlas (std::move (upload));

    // ---- arm the follower, but only for a study a PERSON asked to see -------
    //
    // ⚠️ THIS IS THE ONLY PLACE AUTO-FOLLOW IS EVER TURNED ON, and it is
    // deliberately not "the viewer opened". A viewer that spontaneously began
    // analysing a building nobody asked about would burn the machine on every
    // open and would surprise the user with a heat map they never requested.
    // Until someone has run one study and displayed it, there is no active
    // configuration and the follower sits in NoStudy.
    //
    // ⚠️ AND THE DRIVER'S OWN RERUNS COME BACK THROUGH HERE. `follow=false` is
    // what stops a rerun from re-adopting itself and resetting the quiet period
    // it was started by; the driver passes it, a person never does.
    bool follow = true;
    params.Get ("follow", follow);
    if (follow && haveMetadata) {
        sunfollow::ActiveSunStudyConfig config;
        config.year = metadata.year;
        config.month = metadata.month;
        config.day = metadata.day;
        config.timestep = metadata.timestepMinutes;
        config.hourFrom = metadata.hourFrom;
        config.hourTo = metadata.hourTo;
        config.minAltitudeDeg = metadata.minAltitudeDegrees;
        config.grid = metadata.gridSpacing;
        config.patchDomain = metadata.IsPatchDomain ();
        config.analysisElements = metadata.analysisElements;
        config.contextElements = metadata.contextElements;
        config.debug = adoptedDebug;
        config.depth = adoptedDepth;
        config.hoursMax = rampTop;
        sunfollow::Adopt (id, config);
    }
    return os;
}

// ---------------------------------------------------------------------------
// Tapioca.SunStudyOverlayState - what the RENDERER did with the study.
//
// ⚠️ THIS IS THE ACKNOWLEDGEMENT ShowSunStudy CANNOT GIVE. Geometry reaches the
// renderer through SceneCmdQueue, which is a one-way hand-over with no reply by
// design -- the producer must not block on a render thread. The consequence is
// that the display verb can only report what it SENT, and "sent" and "drawn" are
// different things separated by a viewport that may not be running, a queue that
// something else may clear, and a per-element topology check that may refuse.
//
// On 2026-09-15 all three of those were invisible at once: five studies pushed,
// no viewport, an offscreen capture's startup clearing the queue, and a smoke log
// reporting PASS. This verb exists so that can never be silent again.
class SunStudyOverlayStateCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "SunStudyOverlayState";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        GS::ObjectState os;
        const bool running = archviz::DiligentViewport::Get ().IsRunning ();
        os.Add ("viewerRunning", running);
        if (!running) {
            // ⚠️ EVERY FIELD IS STILL ANSWERED. A caller that reads `drawing`
            // must get `false` here rather than a missing key, or "no viewer"
            // and "viewer with no study" become the same answer to it.
            os.Add ("studyId", Text (std::string ()));
            os.Add ("drawing", false);
            os.Add ("elementsNamed", (GS::Int32) 0);
            os.Add ("elementsAttached", (GS::Int32) 0);
            os.Add ("refusedTriangleCount", (GS::Int32) 0);
            os.Add ("refusedTopologyHash", (GS::Int32) 0);
            os.Add ("atlasWidth", (GS::Int32) 0);
            os.Add ("atlasHeight", (GS::Int32) 0);
            os.Add ("atlasUploads", (GS::Int32) 0);
            os.Add ("atlasBytesUploaded", (GS::Int32) 0);
            os.Add ("revision", (GS::Int32) 0);
            os.Add ("depth", (GS::Int32) 0);
            os.Add ("tintFrames", (GS::Int32) 0);
            os.Add ("tintElementsDrawn", (GS::Int32) 0);
            os.Add ("framesSkippedIncompleteBinding", (GS::Int32) 0);
            os.Add ("rejection", Text ("no Diligent viewport is running - nothing can be displaying a study"));
            return os;
        }

        const archviz::DiligentViewportStats stats = archviz::DiligentViewport::Get ().Stats ();
        const archviz::SunStudyOverlayStatus& overlay = stats.sunStudy;
        os.Add ("studyId", Text (overlay.studyId));
        os.Add ("drawing", overlay.drawing);
        os.Add ("elementsNamed", (GS::Int32) overlay.elementsNamed);
        os.Add ("elementsAttached", (GS::Int32) overlay.elementsAttached);
        os.Add ("refusedTriangleCount", (GS::Int32) overlay.refusedTriangleCount);
        os.Add ("refusedTopologyHash", (GS::Int32) overlay.refusedTopologyHash);
        os.Add ("atlasWidth", (GS::Int32) overlay.atlasWidth);
        os.Add ("atlasHeight", (GS::Int32) overlay.atlasHeight);
        os.Add ("atlasUploads", (GS::Int32) overlay.atlasUploads);
        os.Add ("atlasBytesUploaded", (GS::Int32) overlay.atlasBytesUploaded);
        os.Add ("revision", (GS::Int32) overlay.version);
        os.Add ("depth", (GS::Int32) overlay.depthMode);
        os.Add ("tintFrames", (GS::Int32) overlay.tintFrames);
        os.Add ("tintElementsDrawn", (GS::Int32) overlay.tintElementsDrawn);
        // ⚠️ THE COUNTER THAT DECIDES WHERE TO LOOK NEXT. Non-zero means the
        // scene drew an element whose tint side buffer was not bound that frame:
        // a lifecycle fault. Zero while the picture still flickers means the
        // fault is in the depth state or in the pass underneath, not here.
        os.Add ("framesSkippedIncompleteBinding", (GS::Int32) overlay.framesSkippedIncompleteBinding);
        os.Add ("rejection", Text (overlay.rejection));
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.SunStudyPatchPreview - what the patch domain WOULD measure, on the
// live model, beside what the triangle domain does.
//
// ⚠️ IT RUNS NO STUDY AND DISPLAYS NOTHING. The migration from triangles to
// surfaces deliberately MOVES the sample points, so the two domains cannot be
// diffed sample for sample; what they must agree on is the physical surface
// area, and what has to be seen before migrating is how the counts and the
// packing actually change on a real building rather than on a fixture.
//
// ⚠️ AND IT IS THE ONLY WAY TO SEE THE ONE FAILURE THAT WOULD MAKE PATCH MODE
// POINTLESS. Patch merging rides on the extractor having WELDED coincident
// corners; if it ever stopped, every patch would be one triangle, the diagonal
// seams would come back and the incremental cache would re-key itself on every
// edit -- while the picture went on looking fine. `patches == triangles` in this
// report is that symptom, stated as a number.
class SunStudyPatchPreviewCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "SunStudyPatchPreview";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        std::shared_ptr<const Snapshot> snapshot = MeshStore::Get ().Current ();
        if (snapshot == nullptr)
            return NativeCommandResult::Failure ("no snapshot is live - call Tapioca.BuildSnapshot first");

        double spacing = 2.0;
        params.Get ("grid", spacing);
        if (!(spacing > 0.0))
            spacing = 2.0;

        // The same concatenation StartSunStudy does, so the two see one model.
        std::vector<double> vertices;
        std::vector<uint32_t> triangles;
        std::vector<uint32_t> groups;
        std::vector<std::string> elementOf;
        for (const Mesh& mesh : snapshot->meshes) {
            const uint32_t base = static_cast<uint32_t> (vertices.size () / 3);
            const uint32_t group = static_cast<uint32_t> (elementOf.size ());
            elementOf.push_back (mesh.guid);
            vertices.insert (vertices.end (), mesh.vertices.begin (), mesh.vertices.end ());
            for (const uint32_t index : mesh.triangles)
                triangles.push_back (base + index);
            groups.resize (triangles.size () / 3, group);
        }
        if (triangles.empty ())
            return NativeCommandResult::Failure ("the snapshot has no triangles to measure");

        // ---- the patch domain ------------------------------------------------
        evp::sunstudy::PatchSamplerOptions patchOptions;
        patchOptions.spacing = spacing;
        const evp::sunstudy::PatchSampleGrid patchGrid =
            evp::sunstudy::BuildPatchSampleGrid (vertices.data (), vertices.size () / 3, triangles.data (),
                                                 triangles.size () / 3, groups.data (), elementOf, patchOptions);

        evp::sunstudy::SunStudyPatchAtlas patchAtlas;
        const evp::sunstudy::PatchAtlasUpdate fitted = patchAtlas.Fit (patchGrid);

        // ---- the triangle domain, as the oracle ------------------------------
        evp::sunstudy::SamplerOptions legacy;
        legacy.spacing = spacing;
        legacy.wantLayouts = true;
        const evp::sunstudy::SampleGrid triangleGrid = evp::sunstudy::BuildSampleGrid (
            vertices.data (), vertices.size () / 3, triangles.data (), triangles.size () / 3, groups.data (), legacy);
        const evp::sunstudy::SunStudyAtlas triangleAtlas = evp::sunstudy::BuildSunStudyAtlas (triangleGrid);

        double triangleArea = 0.0;
        for (const double area : triangleGrid.areas)
            triangleArea += area;

        size_t patchTexels = 0;
        for (const auto& entry : patchAtlas.Allocations ())
            patchTexels += static_cast<size_t> (entry.second.width) * entry.second.height;

        GS::ObjectState os;
        os.Add ("elements", (GS::Int32) snapshot->meshes.size ());
        os.Add ("triangles", (GS::Int32) (triangles.size () / 3));
        os.Add ("gridSpacing", spacing);

        os.Add ("patches", (GS::Int32) patchGrid.spans.size ());
        os.Add ("patchSamples", (GS::Int32) patchGrid.Count ());
        os.Add ("patchArea", patchGrid.TotalArea ());
        os.Add ("patchCentroidFallbacks", (GS::Int32) patchGrid.centroidPatches);
        os.Add ("patchAtlasWidth", (GS::Int32) patchAtlas.Width ());
        os.Add ("patchAtlasHeight", (GS::Int32) patchAtlas.Height ());
        os.Add ("patchAtlasTiles", (GS::Int32) patchAtlas.AllocationCount ());
        os.Add ("patchAtlasUsedTexels", (GS::Int32) patchTexels);
        os.Add ("patchAtlasResized", fitted.resized);

        os.Add ("triangleSamples", (GS::Int32) triangleGrid.Count ());
        os.Add ("triangleArea", triangleArea);
        os.Add ("triangleAtlasWidth", (GS::Int32) triangleAtlas.width);
        os.Add ("triangleAtlasHeight", (GS::Int32) triangleAtlas.height);
        os.Add ("triangleAtlasTiles", (GS::Int32) triangleAtlas.placedFaces);

        // ⚠️ THE SYMPTOM, NAMED. See the class note: equal counts mean the
        // extraction stopped welding and patch mode has silently become triangle
        // mode with extra steps.
        os.Add ("weldingLooksBroken", patchGrid.spans.size () >= triangles.size () / 3);
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.SunStudyFollowerState - the ANALYSIS lifecycle.
//
// ⚠️ DELIBERATELY SEPARATE FROM SunStudyOverlayState, WHICH REPORTS RENDERER
// FACTS. "The analysis is stale" and "the renderer is not drawing" are different
// sentences with different fixes, and a single surface that mixed them would
// make a study that is correctly hidden because the model moved look identical
// to one the viewer failed to bind.
class SunStudyFollowerStateCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "SunStudyFollowerState";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        const sunfollow::FollowerStats stats = sunfollow::State ();
        GS::ObjectState os;
        os.Add ("state", Text (StateText (stats.state)));
        os.Add ("autoFollow", stats.autoFollow);
        os.Add ("dirty", stats.dirty);
        os.Add ("dirtyReason", Text (ReasonText (stats.dirtyReason)));
        os.Add ("generation", (GS::Int32) stats.generation);
        os.Add ("studyId", Text (stats.studyId));
        os.Add ("studySnapshot", (GS::Int32) stats.studySnapshot);
        os.Add ("sceneSnapshot", (GS::Int32) stats.sceneSnapshot);
        os.Add ("millisecondsUntilStart", (GS::Int32) stats.millisecondsUntilStart);
        os.Add ("starts", (GS::Int32) stats.starts);
        os.Add ("acceptedCompletions", (GS::Int32) stats.acceptedCompletions);
        os.Add ("discardedCompletions", (GS::Int32) stats.discardedCompletions);
        os.Add ("automaticReruns", (GS::Int32) stats.automaticReruns);
        os.Add ("snapshotRebuilds", (GS::Int32) stats.snapshotRebuilds);
        os.Add ("lastError", Text (stats.lastError));
        // The sentence a person can act on: "scene snapshot 124 != study
        // snapshot 123" says which way to look; `dirty=true` does not.
        os.Add ("description", Text (stats.description));
        return os;
    }

  private:
    static const char* StateText (evp::sunstudy::SunStudyFollowState state)
    {
        switch (state) {
            case evp::sunstudy::SunStudyFollowState::NoStudy:
                return "NoStudy";
            case evp::sunstudy::SunStudyFollowState::Current:
                return "Current";
            case evp::sunstudy::SunStudyFollowState::DirtyVisible:
                return "DirtyVisible";
            case evp::sunstudy::SunStudyFollowState::Starting:
                return "Starting";
            case evp::sunstudy::SunStudyFollowState::UpdatingVisible:
                return "UpdatingVisible";
            case evp::sunstudy::SunStudyFollowState::Failed:
                return "Failed";
        }
        return "Unknown";
    }
    static const char* ReasonText (evp::sunstudy::SunStudyDirtyReason reason)
    {
        switch (reason) {
            case evp::sunstudy::SunStudyDirtyReason::Geometry:
                return "geometry";
            case evp::sunstudy::SunStudyDirtyReason::Sun:
                return "sun";
            case evp::sunstudy::SunStudyDirtyReason::Sampling:
                return "sampling";
            case evp::sunstudy::SunStudyDirtyReason::None:
                break;
        }
        return "none";
    }
};

// ---------------------------------------------------------------------------

const NativeCommandRegistration kSunStudyDisplayRegistrations[] = {
    { "ShowSunStudy", &MakeRegisteredNativeCommand<ShowSunStudyCommand>, false,
      R"json({
            "type":"object",
            "properties":{
                "studyId":{"type":"string"},
                "show":{"type":"boolean"},
                "hoursMax":{"type":"number"},
                "debug":{"type":"integer"},
                "depth":{"type":"integer"},
                "follow":{"type":"boolean"}
            },
            "additionalProperties":false
        })json",
      R"json({
            "type":"object",
            "properties":{
                "studyId":{"type":"string"},
                "shown":{"type":"boolean"},
                "viewerRunning":{"type":"boolean"},
                "elements":{"type":"integer"},
                "atlasWidth":{"type":"integer"},
                "atlasHeight":{"type":"integer"},
                "converged":{"type":"boolean"},
                "hoursMax":{"type":"number"},
                "debug":{"type":"integer"},
                "depth":{"type":"integer"}
            },
            "additionalProperties":false,
            "required":["studyId","shown","elements"]
        })json" },
    { "SunStudyOverlayState", &MakeRegisteredNativeCommand<SunStudyOverlayStateCommand>, false,
      R"json({
            "type":"object",
            "properties":{},
            "additionalProperties":false
        })json",
      R"json({
            "type":"object",
            "properties":{
                "viewerRunning":{"type":"boolean"},
                "studyId":{"type":"string"},
                "drawing":{"type":"boolean"},
                "elementsNamed":{"type":"integer"},
                "elementsAttached":{"type":"integer"},
                "refusedTriangleCount":{"type":"integer"},
                "refusedTopologyHash":{"type":"integer"},
                "atlasWidth":{"type":"integer"},
                "atlasHeight":{"type":"integer"},
                "atlasUploads":{"type":"integer"},
                "atlasBytesUploaded":{"type":"integer"},
                "revision":{"type":"integer"},
                "depth":{"type":"integer"},
                "tintFrames":{"type":"integer"},
                "tintElementsDrawn":{"type":"integer"},
                "framesSkippedIncompleteBinding":{"type":"integer"},
                "rejection":{"type":"string"}
            },
            "additionalProperties":false,
            "required":["viewerRunning","studyId","drawing","elementsNamed","elementsAttached"]
        })json" },
    { "SunStudyPatchPreview", &MakeRegisteredNativeCommand<SunStudyPatchPreviewCommand>, false,
      R"json({
            "type":"object",
            "properties":{"grid":{"type":"number"}},
            "additionalProperties":false
        })json",
      R"json({
            "type":"object",
            "properties":{
                "elements":{"type":"integer"},
                "triangles":{"type":"integer"},
                "gridSpacing":{"type":"number"},
                "patches":{"type":"integer"},
                "patchSamples":{"type":"integer"},
                "patchArea":{"type":"number"},
                "patchCentroidFallbacks":{"type":"integer"},
                "patchAtlasWidth":{"type":"integer"},
                "patchAtlasHeight":{"type":"integer"},
                "patchAtlasTiles":{"type":"integer"},
                "patchAtlasUsedTexels":{"type":"integer"},
                "patchAtlasResized":{"type":"boolean"},
                "triangleSamples":{"type":"integer"},
                "triangleArea":{"type":"number"},
                "triangleAtlasWidth":{"type":"integer"},
                "triangleAtlasHeight":{"type":"integer"},
                "triangleAtlasTiles":{"type":"integer"},
                "weldingLooksBroken":{"type":"boolean"}
            },
            "additionalProperties":false,
            "required":["elements","triangles","patches","patchSamples","patchArea","triangleSamples","triangleArea"]
        })json" },
    { "SunStudyFollowerState", &MakeRegisteredNativeCommand<SunStudyFollowerStateCommand>, false,
      R"json({
            "type":"object",
            "properties":{},
            "additionalProperties":false
        })json",
      R"json({
            "type":"object",
            "properties":{
                "state":{"type":"string"},
                "autoFollow":{"type":"boolean"},
                "dirty":{"type":"boolean"},
                "dirtyReason":{"type":"string"},
                "generation":{"type":"integer"},
                "studyId":{"type":"string"},
                "studySnapshot":{"type":"integer"},
                "sceneSnapshot":{"type":"integer"},
                "millisecondsUntilStart":{"type":"integer"},
                "starts":{"type":"integer"},
                "acceptedCompletions":{"type":"integer"},
                "discardedCompletions":{"type":"integer"},
                "automaticReruns":{"type":"integer"},
                "snapshotRebuilds":{"type":"integer"},
                "lastError":{"type":"string"},
                "description":{"type":"string"}
            },
            "additionalProperties":false,
            "required":["state","autoFollow","dirty","generation","studySnapshot","sceneSnapshot"]
        })json" },
};

} // namespace

NativeCommandRegistrations GetSunStudyDisplayCommandRegistrations ()
{
    return MakeRegistrationView (kSunStudyDisplayRegistrations);
}

} // namespace geomsrv
