#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/SunStudyDisplayCommands.hpp"

#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/CommandUtils.hpp"

#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/SceneCmdQueue.hpp"
#include "ArchViz/SunStudyOverlay.hpp"
#include "Geometry/MeshStore.hpp"
#include "SunStudy/SunStudyAtlas.hpp"
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

    archviz::SceneCmdQueue::Get ().PushSunStudyAtlas (std::move (upload));
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

const NativeCommandRegistration kSunStudyDisplayRegistrations[] = {
    { "ShowSunStudy", &MakeRegisteredNativeCommand<ShowSunStudyCommand>, false,
      R"json({
            "type":"object",
            "properties":{
                "studyId":{"type":"string"},
                "show":{"type":"boolean"},
                "hoursMax":{"type":"number"},
                "debug":{"type":"integer"},
                "depth":{"type":"integer"}
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
};

} // namespace

NativeCommandRegistrations GetSunStudyDisplayCommandRegistrations ()
{
    return MakeRegistrationView (kSunStudyDisplayRegistrations);
}

} // namespace geomsrv
