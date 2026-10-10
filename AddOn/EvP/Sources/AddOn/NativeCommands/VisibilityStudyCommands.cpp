#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/VisibilityStudyCommands.hpp"

#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/SceneCmdQueue.hpp"
#include "ArchViz/VisibilityStudyDisplay.hpp"
#include "ArchViz/VisibilityStudyCapture.hpp"
#include "Geometry/MeshStore.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/SunStudyCommandsSupport.hpp"
#include "SunStudy/VisibilityStudy.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>

namespace geomsrv {

namespace {

using sunstudysupport::ReadDouble;
using sunstudysupport::ReadInt;
using sunstudysupport::ReadString;
using sunstudysupport::ReadStringList;
using sunstudysupport::Text;

std::atomic<uint64_t> s_visibilityId { 1 };

bool ReadTriple (const GS::ObjectState& params, const char* key, double out[3])
{
    GS::Array<double> values;
    if (!params.Get (key, values) || values.GetSize () != 3)
        return false;
    for (USize i = 0; i < 3; ++i)
        out[i] = values[i];
    return true;
}

class RunVisibilityStudyCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "RunVisibilityStudy";
    }

    bool NeedsMainThread () const override
    {
        return false;
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl& processControl) const override
    {
        const auto snapshot = MeshStore::Get ().Current ();
        if (snapshot == nullptr)
            return NativeCommandResult::Failure ("no snapshot is live - call Tapioca.BuildSnapshot first");
        const uint64_t captureStamp = MeshStore::Get ().CaptureStamp ();
        if (!archviz::visibilitystudy::CaptureIsFresh (snapshot, captureStamp))
            return NativeCommandResult::Failure ("the model changed after capture; build a new snapshot");
        const auto from = ReadStringList (params, "fromElements");
        const auto to = ReadStringList (params, "toElements");

        evp::sunstudy::VisibilityStudyOptions options;
        const auto domain = ReadString (params, "domain", "patch");
        if (domain != "patch" && domain != "triangle")
            return NativeCommandResult::Failure ("domain must be patch or triangle");
        options.domain = domain == "patch" ? evp::sunstudy::SamplingDomain::SurfacePatch
                                           : evp::sunstudy::SamplingDomain::TriangleLegacy;
        options.explicitContext = params.Contains ("contextElements");
        options.contextElements = ReadStringList (params, "contextElements");
        const std::string origin = ReadString (params, "origin", "surfaces");
        if (origin == "point") {
            options.origin = evp::sunstudy::VisibilityOrigin::Point;
            if (!ReadTriple (params, "point", options.point) || !ReadTriple (params, "direction", options.direction))
                return NativeCommandResult::Failure ("origin='point' needs point and direction xyz triples");
        }
        else if (origin != "surfaces") {
            return NativeCommandResult::Failure ("origin must be surfaces or point");
        }
        options.spacing = ReadDouble (params, "grid", 2.0);
        options.normalOffset = ReadDouble (params, "normalOffset", 0.05);
        options.tmin = ReadDouble (params, "tmin", 0.001);
        options.coneDegrees = ReadDouble (params, "coneDegrees", 90.0);
        options.maxAimPoints = static_cast<size_t> (std::max<GS::Int32> (1, ReadInt (params, "maxAimPoints", 32)));
        options.maxParallel = static_cast<size_t> (std::max<GS::Int32> (0, ReadInt (params, "maxParallel", 0)));
        bool show = true;
        params.Get ("show", show);
        const uint64_t displayGeneration = show ? archviz::SceneCmdQueue::Get ().ClaimAnalysisDisplay (1) : 0;

        processControl.SetProcessName ("Tapioca: visibility study");
        auto result = evp::sunstudy::RunVisibilityStudy (snapshot, from, to, options,
                                                         [&processControl] { return processControl.TestBreak (); });
        if (!result.valid)
            return NativeCommandResult::Failure (Text (result.error));
        const auto current = MeshStore::Get ().Current ();
        if (current == nullptr || current->id != result.snapshotId ||
            !archviz::visibilitystudy::CaptureIsFresh (snapshot, captureStamp))
            return NativeCommandResult::Failure ("the model changed while visibility was calculating; run it again");
        result.id = "visibility-" + std::to_string (s_visibilityId.fetch_add (1));

        bool shown = false;
        uint32_t atlasWidth = result.AtlasWidth (), atlasHeight = result.AtlasHeight ();
        if (show) {
            std::string error;
            auto upload = archviz::BuildVisibilityStudyUpload (
                result, error, [&processControl] { return processControl.TestBreak (); });
            if (upload == nullptr)
                return NativeCommandResult::Failure (Text (error));
            if (!archviz::visibilitystudy::CaptureIsFresh (snapshot, captureStamp))
                return NativeCommandResult::Failure ("the model changed during display preparation; run it again");
            upload->captureStamp = captureStamp;
            shown = archviz::SceneCmdQueue::Get ().PushAnalysisAtlas (displayGeneration, std::move (upload));
            if (!shown && archviz::DiligentViewport::Get ().IsRunning ())
                return NativeCommandResult::Failure ("visibility display superseded by a later request");
        }

        bool includeValues = false;
        params.Get ("includeValues", includeValues);
        GS::Array<double> values;
        if (includeValues)
            for (const double value : result.values)
                values.Push (value);

        GS::ObjectState os;
        os.Add ("studyId", Text (result.id));
        os.Add ("origin", Text (origin));
        os.Add ("domain", Text (domain));
        os.Add ("patchCount", static_cast<GS::Int32> (result.patchGrid.spans.size ()));
        os.Add ("snapshotId", static_cast<GS::Int64> (result.snapshotId));
        os.Add ("sampleCount", static_cast<GS::Int32> (result.values.size ()));
        os.Add ("aimPointCount", static_cast<GS::Int32> (result.aimPointCount));
        os.Add ("rayCount", static_cast<GS::Int64> (result.rayCount));
        os.Add ("visibleSamples", static_cast<GS::Int32> (result.visibleSamples));
        os.Add ("meanVisibility", result.meanVisibility);
        os.Add ("analysisMilliseconds", result.analysisMilliseconds);
        os.Add ("atlasWidth", static_cast<GS::Int32> (atlasWidth));
        os.Add ("atlasHeight", static_cast<GS::Int32> (atlasHeight));
        os.Add ("shown", shown);
        os.Add ("values", values);
        return os;
    }
};

const NativeCommandRegistration kVisibilityRegistrations[] = {
    { "RunVisibilityStudy", &MakeRegisteredNativeCommand<RunVisibilityStudyCommand>, false,
      R"json({
        "type":"object",
        "properties":{
          "origin":{"type":"string","enum":["surfaces","point"]},
          "domain":{"type":"string","enum":["patch","triangle"]},
          "fromElements":{"type":"array","items":{"type":"string","minLength":1}},
          "toElements":{"type":"array","items":{"type":"string","minLength":1}},
          "contextElements":{"type":"array","items":{"type":"string","minLength":1}},
          "point":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
          "direction":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
          "coneDegrees":{"type":"number","minimum":1,"maximum":179},
          "grid":{"type":"number","exclusiveMinimum":0},
          "normalOffset":{"type":"number","minimum":0},
          "tmin":{"type":"number","minimum":0},
          "maxAimPoints":{"type":"integer","minimum":1,"maximum":256},
          "maxParallel":{"type":"integer","minimum":0,"maximum":64},
          "show":{"type":"boolean"},
          "includeValues":{"type":"boolean"}
        },
        "required":["toElements"],
        "additionalProperties":false
      })json",
      R"json({
        "type":"object",
        "properties":{
          "studyId":{"type":"string"},
          "origin":{"type":"string"},
          "domain":{"type":"string","enum":["patch","triangle"]},
          "patchCount":{"type":"integer"},
          "snapshotId":{"type":"integer"},
          "sampleCount":{"type":"integer"},
          "aimPointCount":{"type":"integer"},
          "rayCount":{"type":"integer"},
          "visibleSamples":{"type":"integer"},
          "meanVisibility":{"type":"number"},
          "analysisMilliseconds":{"type":"number"},
          "atlasWidth":{"type":"integer"},
          "atlasHeight":{"type":"integer"},
          "shown":{"type":"boolean"},
          "values":{"type":"array","items":{"type":"number"}}
        },
        "additionalProperties":false,
        "required":["studyId","origin","domain","patchCount","snapshotId","sampleCount","aimPointCount","rayCount","visibleSamples",
                    "meanVisibility","analysisMilliseconds","atlasWidth","atlasHeight","shown","values"]
      })json" },
};

} // namespace

NativeCommandRegistrations GetVisibilityStudyCommandRegistrations ()
{
    return MakeRegistrationView (kVisibilityRegistrations);
}

} // namespace geomsrv
