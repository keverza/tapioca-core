#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/PointCloudCommands.hpp"

#include "ArchViz/PointCloudLoader.hpp"
#include "Python/CloudCompareRunner.hpp"
#include "Python/MainThreadGate.hpp"
#include "Python/RunCancel.hpp"

#include <algorithm>
#include <atomic>
#include <memory>

namespace geomsrv {
namespace {

void AddPointCloudBounds (GS::ObjectState& os, const GS::UniString& pathText)
{
    const wchar_t* widePath = reinterpret_cast<const wchar_t*> (pathText.ToUStr ().Get ());
    if (widePath == nullptr)
        return;

    const archviz::PointCloudBoundsResult bounds = archviz::InspectPointCloudPly (widePath);
    if (!bounds.succeeded)
        return;

    GS::Array<double> boundsMin;
    GS::Array<double> boundsMax;
    for (size_t axis = 0; axis < 3; ++axis) {
        boundsMin.Push (bounds.boundsMin[axis]);
        boundsMax.Push (bounds.boundsMax[axis]);
    }
    os.Add ("pointCount", static_cast<GS::Int64> (bounds.points));
    os.Add ("boundsMin", boundsMin);
    os.Add ("boundsMax", boundsMax);
}

class RunCloudCompareCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "RunCloudCompare";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString executablePath, inputPath, outputPath;
        if (!params.Get ("executablePath", executablePath) || executablePath.IsEmpty () ||
            !params.Get ("inputPath", inputPath) || inputPath.IsEmpty () || !params.Get ("outputPath", outputPath) ||
            outputPath.IsEmpty ())
            return NativeCommandResult::Failure (
                EVP_FAIL ("executablePath, inputPath and outputPath are required", "Tapioca.RunCloudCompare"));

        GS::Array<double> cropPolygon;
        params.Get ("cropPolygon", cropPolygon);
        if ((cropPolygon.GetSize () % 2) != 0 || (!cropPolygon.IsEmpty () && cropPolygon.GetSize () < 6))
            return NativeCommandResult::Failure (EVP_FAIL (
                "cropPolygon must contain zero values or at least three XY pairs", "Tapioca.RunCloudCompare"));
        bool keepOutside = false;
        double subsampleStep = 0.0;
        params.Get ("keepOutside", keepOutside);
        params.Get ("subsampleStep", subsampleStep);
        if (subsampleStep < 0.0)
            return NativeCommandResult::Failure (
                EVP_FAIL ("subsampleStep cannot be negative", "Tapioca.RunCloudCompare"));

        const evp::CloudCompareResult result = evp::RunCloudCompareCli (
            executablePath, inputPath, outputPath, cropPolygon.IsEmpty () ? nullptr : cropPolygon.GetContent (),
            cropPolygon.GetSize () / 2, keepOutside, subsampleStep, evp::RunCancel::Get ().Generation ());
        GS::ObjectState os;
        os.Add ("succeeded", result.succeeded);
        os.Add ("cancelled", result.cancelled);
        os.Add ("exitCode", (GS::Int32) result.exitCode);
        os.Add ("transcript", result.transcript);
        os.Add ("outputPath", result.outputPath);
        os.Add ("logPath", result.logPath);
        os.Add ("failureReason", result.error);
        if (result.succeeded)
            AddPointCloudBounds (os, result.outputPath);
        return os;
    }
};

class LoadDiligentPointCloudCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "LoadDiligentPointCloud";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString path, layerId;
        if (!params.Get ("path", path) || path.IsEmpty () || !params.Get ("layerId", layerId) || layerId.IsEmpty ())
            return NativeCommandResult::Failure (
                EVP_FAIL ("path and layerId are required", "Tapioca.LoadDiligentPointCloud"));
        struct SurveyPlacement {
            std::atomic<bool> completed { false };
            GSErrCode geoError = NoError;
            GSErrCode transformError = NoError;
            double surveyPoint[3] = {};
            double projectToSurvey[12] = {};
        };
        auto placement = std::make_shared<SurveyPlacement> ();
        GS::UniString gateError;
        const bool gated = evp::MainThreadGate::Get ().Invoke (
            [placement] {
                API_GeoLocation geoLocation {};
                placement->geoError = ACAPI_GeoLocation_GetGeoLocation (&geoLocation);
                if (placement->geoError == NoError) {
                    placement->surveyPoint[0] = geoLocation.surveyPointPosition.x;
                    placement->surveyPoint[1] = geoLocation.surveyPointPosition.y;
                    placement->surveyPoint[2] = geoLocation.surveyPointPosition.z;
                }
                API_Tranmat transform {};
                placement->transformError = ACAPI_SurveyPoint_GetSurveyPointTransformation (&transform);
                if (placement->transformError == NoError)
                    std::copy (transform.tmx, transform.tmx + 12, placement->projectToSurvey);
                placement->completed.store (true);
            },
            evp::MainThreadGate::DefaultTimeoutMs, gateError);
        if (!gated || !placement->completed.load ())
            return NativeCommandResult::Failure (EVP_FAIL (gateError, "reading Archicad survey-point placement"));
        if (placement->geoError != NoError || placement->transformError != NoError) {
            return NativeCommandResult::Failure (GS::UniString::Printf (
                "Could not read Archicad Project Location (geo=%d, transform=%d).",
                static_cast<int> (placement->geoError), static_cast<int> (placement->transformError)));
        }

        const wchar_t* widePath = reinterpret_cast<const wchar_t*> (path.ToUStr ().Get ());
        const archviz::PointCloudLoadResult result = archviz::LoadPointCloudForDiligent (
            widePath == nullptr ? std::wstring () : std::wstring (widePath), layerId.ToCStr ().Get (),
            placement->projectToSurvey);
        if (!result.succeeded)
            return NativeCommandResult::Failure (GS::UniString (result.error.c_str (), CC_UTF8));
        GS::ObjectState os;
        os.Add ("layerId", layerId);
        os.Add ("sourceId", GS::UniString (result.sourceId.c_str (), CC_UTF8));
        os.Add ("points", static_cast<GS::Int64> (result.points));
        os.Add ("nodes", static_cast<GS::Int64> (result.nodes));
        os.Add ("queuedBytes", static_cast<GS::Int64> (result.queuedBytes));
        os.Add ("parseMilliseconds", result.parseMilliseconds);
        os.Add ("hierarchyMilliseconds", result.hierarchyMilliseconds);
        GS::Array<double> surveyPoint;
        GS::Array<double> projectOrigin;
        GS::Array<double> projectBoundsMin;
        GS::Array<double> projectBoundsMax;
        for (size_t axis = 0; axis < 3; ++axis) {
            surveyPoint.Push (placement->surveyPoint[axis]);
            projectOrigin.Push (result.projectOrigin[axis]);
            projectBoundsMin.Push (result.projectBoundsMin[axis]);
            projectBoundsMax.Push (result.projectBoundsMax[axis]);
        }
        os.Add ("surveyPointPosition", surveyPoint);
        os.Add ("projectOrigin", projectOrigin);
        os.Add ("projectBoundsMin", projectBoundsMin);
        os.Add ("projectBoundsMax", projectBoundsMax);
        os.Add ("coordinateUnit", "m");
        return os;
    }
};

const NativeCommandRegistration kPointCloudCommandRegistrations[] = {
    { "RunCloudCompare", &MakeRegisteredNativeCommand<RunCloudCompareCommand>, false,
      R"json({"type":"object","properties":{"executablePath":{"type":"string","minLength":1},"inputPath":{"type":"string","minLength":1},"outputPath":{"type":"string","minLength":1},"cropPolygon":{"type":"array","items":{"type":"number"},"minItems":0},"keepOutside":{"type":"boolean"},"subsampleStep":{"type":"number","minimum":0}},"additionalProperties":false,"required":["executablePath","inputPath","outputPath"]})json",
      R"json({"type":"object","properties":{"succeeded":{"type":"boolean"},"cancelled":{"type":"boolean"},"exitCode":{"type":"integer"},"transcript":{"type":"string"},"outputPath":{"type":"string"},"logPath":{"type":"string"},"failureReason":{"type":"string"},"pointCount":{"type":"integer","minimum":0},"boundsMin":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"boundsMax":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"additionalProperties":false,"required":["succeeded","cancelled","exitCode","transcript","outputPath","logPath","failureReason"]})json" },
    { "LoadDiligentPointCloud", &MakeRegisteredNativeCommand<LoadDiligentPointCloudCommand>, false,
      R"json({"type":"object","properties":{"path":{"type":"string","minLength":1},"layerId":{"type":"string","minLength":1}},"additionalProperties":false,"required":["path","layerId"]})json",
      R"json({"type":"object","properties":{"layerId":{"type":"string"},"sourceId":{"type":"string"},"points":{"type":"integer","minimum":0},"nodes":{"type":"integer","minimum":0},"queuedBytes":{"type":"integer","minimum":0},"parseMilliseconds":{"type":"number","minimum":0},"hierarchyMilliseconds":{"type":"number","minimum":0},"surveyPointPosition":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"projectOrigin":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"projectBoundsMin":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"projectBoundsMax":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"coordinateUnit":{"type":"string","enum":["m"]}},"additionalProperties":false,"required":["layerId","sourceId","points","nodes","queuedBytes","parseMilliseconds","hierarchyMilliseconds","surveyPointPosition","projectOrigin","projectBoundsMin","projectBoundsMax","coordinateUnit"]})json" },
};

} // namespace

NativeCommandRegistrations GetPointCloudCommandRegistrations ()
{
    return MakeRegistrationView (kPointCloudCommandRegistrations);
}

} // namespace geomsrv
