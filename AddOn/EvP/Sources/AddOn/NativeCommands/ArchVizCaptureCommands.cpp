#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/ArchVizCaptureCommands.hpp"

#include "NativeCommands/ArchVizCaptureParams.hpp" // ReadCaptureCamera, ReadCaptureFrame, ReadCaptureOverlays
#include "NativeCommands/CommandRegistration.hpp"

#include "ArchViz/DiligentViewport.hpp"

#include <string>
#include <vector>

namespace geomsrv {
namespace {

namespace av = archviz;

class StartDiligentCaptureCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "StartDiligentCapture";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Int32 width = 0, height = 0;
        GS::UniString quality;
        params.Get ("width", width);
        params.Get ("height", height);
        params.Get ("renderQuality", quality);
        uint64_t id = 0;
        std::string error;
        if (!av::DiligentViewport::Get ().StartCapture (uint32_t (width), uint32_t (height), ReadCaptureCamera (params),
                                                        quality == "realistic" ? 1 : 0, ReadCaptureOverlays (params),
                                                        id, error))
            return NativeCommandResult::Failure (GS::UniString (error.c_str (), CC_UTF8));
        GS::ObjectState os;
        os.Add ("id", static_cast<GS::Int64> (id));
        os.Add ("status", "running");
        return os;
    }
};

// MANY CAMERAS, ONE EXTRACTION.
//
// ⚠️ THE REASON THIS EXISTS IS THE EXTRACTION, NOT THE CONVENIENCE.
// StartDiligentCapture clears the scene queue and walks the whole model every
// time it is called, so a list of eight viewpoints costs eight full extractions
// of a model that did not change between them - minutes each on a real project.
// This renders all of them from one.
//
// ⚠️ AND EACH FRAME CARRIES ITS OWN SUN, which the single-camera command
// has no field for at all. A camera list captured across a day is a sun study;
// without a per-frame sun every frame would be lit by whatever the viewer was
// last set to, and would look entirely plausible while being wrong.
//
// Frames are written to `outputDirectory` as `00.png`, `01.png` ... as each is
// encoded. The directory must exist; the caller makes it, because a renderer
// creating directories on a user's disk is a decision that belongs further up.
class StartDiligentCaptureBatchCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "StartDiligentCaptureBatch";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Int32 width = 0, height = 0;
        GS::UniString quality;
        GS::UniString directory;
        params.Get ("width", width);
        params.Get ("height", height);
        params.Get ("renderQuality", quality);
        params.Get ("outputDirectory", directory);

        GS::Array<GS::ObjectState> requested;
        params.Get ("cameras", requested);
        std::vector<av::CaptureFrame> frames;
        frames.reserve (requested.GetSize ());
        for (UIndex i = 0; i < requested.GetSize (); ++i)
            frames.push_back (ReadCaptureFrame (requested[i]));

        uint64_t id = 0;
        std::string error;
        if (!av::DiligentViewport::Get ().StartCaptureBatch (
                uint32_t (width), uint32_t (height), frames, quality == "realistic" ? 1 : 0,
                ReadCaptureOverlays (params), std::string (directory.ToCStr (0, MaxUSize, CC_UTF8).Get ()), id, error))
            return NativeCommandResult::Failure (GS::UniString (error.c_str (), CC_UTF8));

        GS::ObjectState os;
        os.Add ("id", static_cast<GS::Int64> (id));
        os.Add ("status", "running");
        os.Add ("frameCount", static_cast<GS::Int32> (frames.size ()));
        return os;
    }
};

class DiligentCaptureStateCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "DiligentCaptureState";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Int64 id = 0;
        params.Get ("id", id);
        const av::DiligentCaptureStats stats = av::DiligentViewport::Get ().CaptureStats ();
        if (stats.id != static_cast<uint64_t> (id))
            return NativeCommandResult::Failure ("the Diligent capture id is unknown or has expired");
        GS::ObjectState os;
        os.Add ("id", static_cast<GS::Int64> (stats.id));
        os.Add ("status", GS::UniString (stats.status.c_str (), CC_UTF8));
        os.Add ("stage", GS::UniString (stats.stage.c_str (), CC_UTF8));
        os.Add ("width", static_cast<GS::Int32> (stats.width));
        os.Add ("height", static_cast<GS::Int32> (stats.height));
        os.Add ("bytes", static_cast<GS::Int64> (stats.bytes));
        os.Add ("url", GS::UniString (stats.url.c_str (), CC_UTF8));
        os.Add ("failureMessage", GS::UniString (stats.failureMessage.c_str (), CC_UTF8));
        // ⚠️ A COUNT, NOT JUST A STAGE. "rendering" sitting unchanged for
        // four minutes is indistinguishable from a hang; "3 of 8" is the
        // difference between waiting and giving up. Present for a single capture
        // too, where it reads 1 of 1.
        os.Add ("frameCount", static_cast<GS::Int32> (stats.frameCount));
        os.Add ("framesDone", static_cast<GS::Int32> (stats.framesDone));
        GS::Array<GS::UniString> paths;
        for (const std::string& path : stats.paths)
            paths.Push (GS::UniString (path.c_str (), CC_UTF8));
        os.Add ("paths", paths);
        return os;
    }
};

class CancelDiligentCaptureCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "CancelDiligentCapture";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Int64 id = 0;
        params.Get ("id", id);
        GS::ObjectState os;
        os.Add ("cancelled", av::DiligentViewport::Get ().CancelCapture (static_cast<uint64_t> (id)));
        return os;
    }
};

const NativeCommandRegistration registrations[] = {
    { "StartDiligentCapture", &MakeRegisteredNativeCommand<StartDiligentCaptureCommand>, false,
      R"json({"type":"object","properties":{"width":{"type":"integer","minimum":16,"maximum":8192},"height":{"type":"integer","minimum":16,"maximum":8192},"renderQuality":{"type":"string","enum":["fast","realistic"]},"storySlices":{"type":"boolean"},"storySliceFill":{"type":"boolean"},"storySliceOccluded":{"type":"string","enum":["hidden","dashed","solid"]},"storySliceWidthPixels":{"type":"number","exclusiveMinimum":0,"maximum":32},"storySliceRgba":{"type":"integer"},"storySliceFillRgba":{"type":"integer"},"camera":{"type":"object","properties":{"valid":{"type":"boolean"},"source":{"type":"string"},"orthographic":{"type":"boolean"},"viewMoving":{"type":"boolean"},"eyeX":{"type":"number"},"eyeY":{"type":"number"},"eyeZ":{"type":"number"},"targetX":{"type":"number"},"targetY":{"type":"number"},"targetZ":{"type":"number"},"viewConeDegreesHorizontal":{"type":"number","exclusiveMinimum":1,"exclusiveMaximum":179}},"additionalProperties":false,"required":["valid","source","orthographic","viewMoving","eyeX","eyeY","eyeZ","targetX","targetY","targetZ","viewConeDegreesHorizontal"]}},"additionalProperties":false,"required":["width","height","renderQuality","camera"]})json",
      R"json({"type":"object","properties":{"id":{"type":"integer","minimum":1},"status":{"type":"string","const":"running"}},"additionalProperties":false,"required":["id","status"]})json" },
    { "StartDiligentCaptureBatch", &MakeRegisteredNativeCommand<StartDiligentCaptureBatchCommand>, false,
      R"json({"type":"object","properties":{"width":{"type":"integer","minimum":16,"maximum":8192},"height":{"type":"integer","minimum":16,"maximum":8192},"renderQuality":{"type":"string","enum":["fast","realistic"]},"storySlices":{"type":"boolean"},"storySliceFill":{"type":"boolean"},"storySliceOccluded":{"type":"string","enum":["hidden","dashed","solid"]},"storySliceWidthPixels":{"type":"number","exclusiveMinimum":0,"maximum":32},"storySliceRgba":{"type":"integer"},"storySliceFillRgba":{"type":"integer"},"outputDirectory":{"type":"string","minLength":1},"cameras":{"type":"array","minItems":1,"maxItems":256,"items":{"type":"object","properties":{"valid":{"type":"boolean"},"source":{"type":"string"},"orthographic":{"type":"boolean"},"viewMoving":{"type":"boolean"},"eyeX":{"type":"number"},"eyeY":{"type":"number"},"eyeZ":{"type":"number"},"targetX":{"type":"number"},"targetY":{"type":"number"},"targetZ":{"type":"number"},"viewConeDegreesHorizontal":{"type":"number","exclusiveMinimum":1,"exclusiveMaximum":179},"sun":{"type":"object","properties":{"enabled":{"type":"boolean"},"azimuthDegrees":{"type":"number","minimum":-360,"maximum":360},"altitudeDegrees":{"type":"number","minimum":-90,"maximum":90}},"additionalProperties":false,"required":["enabled"]}},"additionalProperties":false,"required":["valid","source","orthographic","viewMoving","eyeX","eyeY","eyeZ","targetX","targetY","targetZ","viewConeDegreesHorizontal"]}}},"additionalProperties":false,"required":["width","height","renderQuality","outputDirectory","cameras"]})json",
      R"json({"type":"object","properties":{"id":{"type":"integer","minimum":1},"status":{"type":"string","const":"running"},"frameCount":{"type":"integer","minimum":1}},"additionalProperties":false,"required":["id","status","frameCount"]})json" },
    { "DiligentCaptureState", &MakeRegisteredNativeCommand<DiligentCaptureStateCommand>, false,
      R"json({"type":"object","properties":{"id":{"type":"integer","minimum":1}},"additionalProperties":false,"required":["id"]})json",
      R"json({"type":"object","properties":{"id":{"type":"integer","minimum":1},"status":{"type":"string","enum":["running","completed","failed","cancelled"]},"stage":{"type":"string"},"width":{"type":"integer","minimum":16},"height":{"type":"integer","minimum":16},"bytes":{"type":"integer","minimum":0},"url":{"type":"string"},"failureMessage":{"type":"string"},"frameCount":{"type":"integer","minimum":0},"framesDone":{"type":"integer","minimum":0},"paths":{"type":"array","items":{"type":"string","minLength":1}}},"additionalProperties":false,"required":["id","status","stage","width","height","bytes","url","failureMessage","frameCount","framesDone","paths"]})json" },
    { "CancelDiligentCapture", &MakeRegisteredNativeCommand<CancelDiligentCaptureCommand>, false,
      R"json({"type":"object","properties":{"id":{"type":"integer","minimum":1}},"additionalProperties":false,"required":["id"]})json",
      R"json({"type":"object","properties":{"cancelled":{"type":"boolean"}},"additionalProperties":false,"required":["cancelled"]})json" },
};

} // namespace

NativeCommandRegistrations GetArchVizCaptureCommandRegistrations ()
{
    return MakeRegistrationView (registrations);
}

} // namespace geomsrv
