#include "NativeCommands/SurfaceTakeoffControllerCommands.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/SurfaceTakeoffCommands.hpp"
#include "Geometry/SurfaceTakeoffController.hpp"

namespace geomsrv {
namespace {
GS::UniString Text (const std::string& value)
{
    return GS::UniString (value.c_str (), CC_UTF8);
}
const char kStartOutput[] = R"json({
 "type":"object","additionalProperties":false,
 "properties":{
  "state":{"const":"pending"},"generation":{"type":"integer","minimum":1},
  "snapshotId":{"type":"integer","minimum":1},"captureStamp":{"type":"integer","minimum":1}
 },"required":["state","generation","snapshotId","captureStamp"]
})json";
const char kPollOutput[] = R"json({
 "type":"object","additionalProperties":false,
 "properties":{
  "state":{"enum":["empty","pending","current","stale","refused","cancelled"]},
  "generation":{"type":"integer","minimum":0},"error":{"type":"string"},
  "hasLastGood":{"type":"boolean"},
  "lastGoodGeneration":{"type":"integer","minimum":1},
  "lastGoodSnapshotId":{"type":"integer","minimum":1},
  "lastGoodCaptureStamp":{"type":"integer","minimum":1},
  "lastGoodResult":{"type":"object"}
 },"required":["state","generation","error","hasLastGood"]
})json";
const char kEmptyInput[] = R"json({"type":"object","properties":{},"additionalProperties":false})json";
const char kPublishInput[] = R"json({
 "type":"object","additionalProperties":false,
 "properties":{"generation":{"type":"integer","minimum":1}},"required":["generation"]
})json";
const char kPublishOutput[] = R"json({
 "type":"object","additionalProperties":false,
 "properties":{
  "overlayPublished":{"const":true},"generation":{"type":"integer","minimum":1},
  "snapshotId":{"type":"integer","minimum":1},"captureStamp":{"type":"integer","minimum":1},
  "result":{"type":"object"}
 },"required":["overlayPublished","generation","snapshotId","captureStamp","result"]
})json";
const char kCancelInput[] = R"json({
 "type":"object","additionalProperties":false,
 "properties":{"generation":{"type":"integer","minimum":1}}
})json";
const char kCancelOutput[] =
    R"json({"type":"object","properties":{"cancelled":{"type":"boolean"}},"required":["cancelled"],"additionalProperties":false})json";

class StartTakeoffCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "StartExposedSurfaceTakeoff";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Int64 id = 0, stamp = 0;
        params.Get ("snapshotId", id);
        params.Get ("captureStamp", stamp);
        const auto snapshot = MeshStore::Get ().Current ();
        if (!snapshot || snapshot->id != uint64_t (id) || snapshot->captureStamp != uint64_t (stamp) ||
            MeshStore::Get ().CaptureStamp () != snapshot->captureStamp)
            return NativeCommandResult::Failure (Text ("Snapshot missing or stale; prepare a fresh capture."));
        auto calculation = SurfaceTakeoffRequestFromNative (params);
        calculation.publishOverlay = false;
        surfacetakeoff::ControllerRequest request;
        request.snapshot = snapshot;
        request.execute = [calculation] (const std::atomic<bool>& cancelled) {
            auto result = CalculateExteriorSurfaceResult (calculation, [&cancelled] { return cancelled.load (); });
            surfacetakeoff::ControllerOutcome outcome;
            outcome.ok = result.result.has_value ();
            if (result.result)
                outcome.payload = std::make_shared<surfacetakeoff::Result> (std::move (*result.result));
            outcome.error = std::move (result.error);
            return outcome;
        };
        std::string error;
        auto& controller = geomsrv::surfacetakeoff::SharedSurfaceTakeoffController ();
        const uint64_t generation = controller.Submit (std::move (request), error);
        if (generation == 0)
            return NativeCommandResult::Failure (Text (error));
        GS::ObjectState output;
        output.Add ("state", Text ("pending"));
        output.Add ("generation", GS::Int64 (generation));
        output.Add ("snapshotId", id);
        output.Add ("captureStamp", stamp);
        return output;
    }
};

class PollTakeoffCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "PollExposedSurfaceTakeoff";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        const auto view = geomsrv::surfacetakeoff::SharedSurfaceTakeoffController ().Poll ();
        GS::ObjectState output;
        output.Add ("state", Text (ControllerStateName (view.state)));
        output.Add ("generation", GS::Int64 (view.generation));
        output.Add ("error", Text (view.error));
        output.Add ("hasLastGood", view.lastGood.has_value ());
        if (view.lastGood) {
            output.Add ("lastGoodGeneration", GS::Int64 (view.lastGood->generation));
            output.Add ("lastGoodSnapshotId", GS::Int64 (view.lastGood->snapshotId));
            output.Add ("lastGoodCaptureStamp", GS::Int64 (view.lastGood->captureStamp));
            const auto result = std::static_pointer_cast<const surfacetakeoff::Result> (view.lastGood->payload);
            if (result)
                output.Add ("lastGoodResult", ToNativeSurfaceTakeoffResult (*result).data);
        }
        return output;
    }
};

class PublishTakeoffCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "PublishExposedSurfaceTakeoff";
    }
    bool NeedsMainThread () const override
    {
        return true;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Int64 generation = 0;
        params.Get ("generation", generation);
        const auto view = geomsrv::surfacetakeoff::SharedSurfaceTakeoffController ().Poll ();
        if (view.state != surfacetakeoff::ControllerState::Current || !view.lastGood || generation <= 0 ||
            view.lastGood->generation != uint64_t (generation) || view.lastGood->generation != view.generation)
            return NativeCommandResult::Failure (Text ("The requested controller result is not current."));
        const auto result = std::static_pointer_cast<const surfacetakeoff::Result> (view.lastGood->payload);
        if (!result)
            return NativeCommandResult::Failure (Text ("The controller result has no immutable surface payload."));
        NativeCommandResult published;
        std::string error;
        auto& controller = geomsrv::surfacetakeoff::SharedSurfaceTakeoffController ();
        const bool committed = controller.CommitPublication (
            uint64_t (generation),
            [&] (std::string& commitError) {
                published = PublishExteriorSurfaceResult (*result, uint64_t (generation));
                if (!published.ok)
                    commitError = published.error.ToCStr (0, MaxUSize, CC_UTF8).Get ();
                return published.ok;
            },
            error);
        if (!committed)
            return NativeCommandResult::Failure (Text (error));
        return published;
    }
};

class CancelTakeoffCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "CancelExposedSurfaceTakeoff";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Int64 generation = 0;
        params.Get ("generation", generation);
        GS::ObjectState output;
        output.Add ("cancelled",
                    geomsrv::surfacetakeoff::SharedSurfaceTakeoffController ().Cancel (uint64_t (generation)));
        return output;
    }
};

const NativeCommandRegistration kRegistrations[] = {
    { "StartExposedSurfaceTakeoff", &MakeRegisteredNativeCommand<StartTakeoffCommand>, false,
      SurfaceTakeoffInputSchema (), kStartOutput },
    { "PollExposedSurfaceTakeoff", &MakeRegisteredNativeCommand<PollTakeoffCommand>, false, kEmptyInput, kPollOutput },
    { "PublishExposedSurfaceTakeoff", &MakeRegisteredNativeCommand<PublishTakeoffCommand>, false, kPublishInput,
      kPublishOutput },
    { "CancelExposedSurfaceTakeoff", &MakeRegisteredNativeCommand<CancelTakeoffCommand>, true, kCancelInput,
      kCancelOutput }
};
} // namespace

NativeCommandRegistrations GetSurfaceTakeoffControllerCommandRegistrations ()
{
    return MakeRegistrationView (kRegistrations);
}
} // namespace geomsrv
