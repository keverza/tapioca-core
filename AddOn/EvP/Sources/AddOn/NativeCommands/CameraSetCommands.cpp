#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/CameraSetCommands.hpp"

#include "NativeCommands/CameraSetStore.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "ProjectEnv/ArchicadCamera.hpp"

namespace geomsrv {
namespace {

GS::ObjectState CaptureCameraState (const ArchicadCamera& camera)
{
    GS::ObjectState state;
    state.Add ("valid", camera.valid);
    state.Add ("source", GS::UniString (camera.source.c_str (), CC_UTF8));
    state.Add ("orthographic", false);
    state.Add ("viewMoving", false);
    state.Add ("eyeX", camera.eye[0]);
    state.Add ("eyeY", camera.eye[1]);
    state.Add ("eyeZ", camera.eye[2]);
    state.Add ("targetX", camera.target[0]);
    state.Add ("targetY", camera.target[1]);
    state.Add ("targetZ", camera.target[2]);
    state.Add ("viewConeDegreesHorizontal", camera.viewConeDegreesHorizontal);
    GS::ObjectState sun;
    sun.Add ("enabled", camera.hasSun);
    sun.Add ("azimuthDegrees", camera.sunAzimuthDegrees);
    sun.Add ("altitudeDegrees", camera.sunAltitudeDegrees);
    state.Add ("sun", sun);
    return state;
}

class GetCameraSetCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "GetCameraSet";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString name;
        if (!params.Get ("name", name) || !CameraSetStore::Get ().IsDeclared (name))
            return NativeCommandResult::Failure ("camera set is not declared for the active command");
        GS::Array<GS::ObjectState> cameras;
        for (const ArchicadCamera& camera : CameraSetStore::Get ().Values (name))
            cameras.Push (CaptureCameraState (camera));
        GS::ObjectState response;
        response.Add ("cameras", cameras);
        response.Add ("count", static_cast<GS::Int32> (cameras.GetSize ()));
        return response;
    }
};

class ModifyCameraSetCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "ModifyCameraSet";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString name, action;
        if (!params.Get ("name", name) || !params.Get ("action", action) || !CameraSetStore::Get ().IsDeclared (name))
            return NativeCommandResult::Failure ("camera set is not declared for the active command");

        GS::Int64 index = -1;
        params.Get ("index", index);
        const bool needsSelection = action == "update" || action == "remove" || action == "restore";
        if (needsSelection && index < 0)
            return NativeCommandResult::Failure ("select a camera first");

        GS::UniString error;
        bool inFront = false;
        GS::Int32 changed = 0;
        if (action == "add" || action == "update") {
            ArchicadCamera camera;
            std::string cameraError;
            if (!ReadArchicadCamera (camera, cameraError))
                return NativeCommandResult::Failure (GS::UniString (cameraError.c_str (), CC_UTF8));
            if (!camera.valid)
                return NativeCommandResult::Failure (GS::UniString (camera.source.c_str (), CC_UTF8));
            const bool ok = action == "add"
                                ? CameraSetStore::Get ().Add (name, camera, error)
                                : CameraSetStore::Get ().Update (name, static_cast<size_t> (index), camera, error);
            if (!ok)
                return NativeCommandResult::Failure (error);
            changed = 1;
        }
        else if (action == "remove") {
            if (!CameraSetStore::Get ().Remove (name, static_cast<size_t> (index), error))
                return NativeCommandResult::Failure (error);
            changed = 1;
        }
        else if (action == "restore") {
            const std::vector<ArchicadCamera> cameras = CameraSetStore::Get ().Values (name);
            if (static_cast<size_t> (index) >= cameras.size ())
                return NativeCommandResult::Failure ("the selected camera no longer exists");
            std::string cameraError;
            if (!WriteArchicadCamera (cameras[static_cast<size_t> (index)], inFront, cameraError))
                return NativeCommandResult::Failure (GS::UniString (cameraError.c_str (), CC_UTF8));
        }
        else if (action == "clear") {
            changed = static_cast<GS::Int32> (CameraSetStore::Get ().Values (name).size ());
            if (!CameraSetStore::Get ().Clear (name, error))
                return NativeCommandResult::Failure (error);
        }
        else {
            return NativeCommandResult::Failure ("action must be add, update, remove, restore, or clear");
        }

        GS::ObjectState response;
        response.Add ("count", static_cast<GS::Int32> (CameraSetStore::Get ().Values (name).size ()));
        response.Add ("changed", changed);
        response.Add ("threeDWindowInFront", inFront);
        return response;
    }
};

const NativeCommandRegistration registrations[] = {
    { "GetCameraSet", &MakeRegisteredNativeCommand<GetCameraSetCommand>, false,
      R"json({"type":"object","properties":{"name":{"type":"string","minLength":1}},"additionalProperties":false,"required":["name"]})json",
      R"json({"type":"object","properties":{"cameras":{"type":"array","items":{"type":"object","properties":{"valid":{"type":"boolean"},"source":{"type":"string"},"orthographic":{"type":"boolean"},"viewMoving":{"type":"boolean"},"eyeX":{"type":"number"},"eyeY":{"type":"number"},"eyeZ":{"type":"number"},"targetX":{"type":"number"},"targetY":{"type":"number"},"targetZ":{"type":"number"},"viewConeDegreesHorizontal":{"type":"number","exclusiveMinimum":1,"exclusiveMaximum":179},"sun":{"type":"object","properties":{"enabled":{"type":"boolean"},"azimuthDegrees":{"type":"number","minimum":-360,"maximum":360},"altitudeDegrees":{"type":"number","minimum":-90,"maximum":90}},"additionalProperties":false,"required":["enabled"]}},"additionalProperties":false,"required":["valid","source","orthographic","viewMoving","eyeX","eyeY","eyeZ","targetX","targetY","targetZ","viewConeDegreesHorizontal","sun"]}},"count":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["cameras","count"]})json" },
    { "ModifyCameraSet", &MakeRegisteredNativeCommand<ModifyCameraSetCommand>, false,
      R"json({"type":"object","properties":{"name":{"type":"string","minLength":1},"action":{"type":"string","enum":["add","update","remove","restore","clear"]},"index":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["name","action"]})json",
      R"json({"type":"object","properties":{"count":{"type":"integer","minimum":0},"changed":{"type":"integer","minimum":0},"threeDWindowInFront":{"type":"boolean"}},"additionalProperties":false,"required":["count","changed","threeDWindowInFront"]})json" },
};

} // namespace

NativeCommandRegistrations GetCameraSetCommandRegistrations ()
{
    return MakeRegistrationView (registrations);
}

} // namespace geomsrv
