// NativeCommands/OverlayCommands -- the overlays in Archicad's own views, as the
// product exposes them. See the header.
//
//   Tapioca.Overlay3D {action}   the 3D overlay: composed at the 3D window's Present
//   Tapioca.Overlay2D {action}   the 2D overlay: the floor plan, composed at its Present
//
// The caller's own geometry on both is NativeCommands/OverlayLayerCommands.
//
//   action  "on"      want it; it starts at once if its view is in front, otherwise
//                     when that view comes forward
//           "off"     stop wanting it; its renderer stops
//           "toggle"  what the menu item does
//           "state"   (the default) report, change nothing
//
// ⚠️ A REFUSED START IS AN ANSWER, NOT A TRANSPORT FAILURE. The response says
// `refused`, the refusal's `code` and whether it may clear by itself; the verb fails
// only when it is asked for something it does not know.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/OverlayCommands.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/CommandRegistration.hpp"

#include "ArchViz/InjectedOverlayRuntime.hpp"
#include "ArchViz/OverlayController.hpp"
#include "ArchViz/PlanOverlayRuntime.hpp"

#include <string>
#include <vector>

namespace geomsrv {

namespace {

namespace control = archviz::overlaycontrol;

GS::UniString Utf8 (const std::string& text)
{
    return GS::UniString (text.c_str (), CC_UTF8);
}

GS::UniString Text (uint64_t value)
{
    return GS::UniString (std::to_string (value).c_str (), CC_UTF8);
}

// The action, applied. False only for an action this verb does not know -- which the
// schema's enum refuses for a bus caller before this runs.
bool Apply (control::Overlay which, const GS::ObjectState& params, std::string& action, control::Outcome& outcome)
{
    action = "state";
    if (params.Contains ("action")) {
        GS::UniString text;
        params.Get ("action", text);
        action = text.ToCStr (0, MaxUSize, CC_UTF8).Get ();
    }
    if (action == "on")
        outcome = control::SetWanted (which, true, "api");
    else if (action == "off")
        outcome = control::SetWanted (which, false, "api");
    else if (action == "toggle")
        outcome = control::Toggle (which, "api");
    else if (action == "state")
        outcome = control::Describe (which);
    else
        return false;
    return true;
}

void AddOutcome (GS::ObjectState& os, const std::string& action, const control::Outcome& outcome)
{
    os.Add ("action", Utf8 (action));
    os.Add ("wanted", outcome.wanted);
    os.Add ("running", outcome.running);
    os.Add ("refused", !outcome.ok);
    os.Add ("code", Utf8 (outcome.code));
    os.Add ("message", Utf8 (outcome.message));
    os.Add ("retryable", outcome.retryable);
    os.Add ("view", Utf8 (control::ViewKindName (control::CurrentView ())));
}

class Overlay3DCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "Overlay3D";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        std::string action;
        control::Outcome outcome;
        if (!Apply (control::Overlay::ThreeD, params, action, outcome))
            return NativeCommandResult::Failure (
                EVP_FAIL (GS::UniString ("unknown action '") + Utf8 (action) + "'; expected on, off, toggle or state",
                          "driving the 3D overlay"));
        namespace runtime = archviz::overlayruntime;
        const runtime::Health health = runtime::GetHealth ();
        GS::ObjectState os;
        AddOutcome (os, action, outcome);
        // Whether it is drawing, and if not, where it stopped: `camera` is Locked
        // once the model's camera is found, `host` Ready once the building's own
        // geometry can hide the overlay. `OverlayRuntime` has the whole record.
        os.Add ("camera", Utf8 (runtime::CameraStateName (health.camera)));
        os.Add ("host", Utf8 (runtime::HostStateName (health.host)));
        os.Add ("presentInjections", Text (health.presentInjections));
        return os;
    }
};

class Overlay2DCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "Overlay2D";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        std::string action;
        control::Outcome outcome;
        if (!Apply (control::Overlay::TwoD, params, action, outcome))
            return NativeCommandResult::Failure (
                EVP_FAIL (GS::UniString ("unknown action '") + Utf8 (action) + "'; expected on, off, toggle or state",
                          "driving the 2D overlay"));
        const archviz::planruntime::Status status = archviz::planruntime::GetStatus ();
        GS::ObjectState os;
        AddOutcome (os, action, outcome);
        // What it draws and how it is keeping up: every canvas Present past the
        // first is drawn once prepared, and `drawnWithLastRead` counts the frames
        // whose read was refused (our own redraws) and drew with the one before.
        os.Add ("storey", (GS::Int32) status.storey);
        os.Add ("rings", (GS::Int32) status.rings);
        os.Add ("segments", (GS::Int32) status.segments);
        os.Add ("canvasPresents", Text (status.canvasPresents));
        os.Add ("drawn", Text (status.drawn));
        os.Add ("readsFresh", Text (status.readsFresh));
        os.Add ("drawnWithLastRead", Text (status.drawnWithLastRead));
        os.Add ("lastError", Utf8 (status.lastError));
        return os;
    }
};

// clang-format off
constexpr const char kOverlayActionInput[] = R"json({"type":"object","properties":{
    "action":{"type":"string","enum":["on","off","toggle","state"]}},
  "additionalProperties":false})json";

constexpr const char kOverlay3DOutput[] = R"json({"type":"object","properties":{
    "action":{"type":"string"},"wanted":{"type":"boolean"},"running":{"type":"boolean"},"refused":{"type":"boolean"},
    "code":{"type":"string"},"message":{"type":"string"},"retryable":{"type":"boolean"},"view":{"type":"string"},
    "camera":{"type":"string"},"host":{"type":"string"},"presentInjections":{"type":"string"}},
  "additionalProperties":false,
  "required":["action","wanted","running","refused","code","message","retryable","view","camera","host",
              "presentInjections"]})json";

constexpr const char kOverlay2DOutput[] = R"json({"type":"object","properties":{
    "action":{"type":"string"},"wanted":{"type":"boolean"},"running":{"type":"boolean"},"refused":{"type":"boolean"},
    "code":{"type":"string"},"message":{"type":"string"},"retryable":{"type":"boolean"},"view":{"type":"string"},
    "storey":{"type":"integer"},"rings":{"type":"integer","minimum":0},"segments":{"type":"integer","minimum":0},
    "canvasPresents":{"type":"string"},"drawn":{"type":"string"},"readsFresh":{"type":"string"},
    "drawnWithLastRead":{"type":"string"},"lastError":{"type":"string"}},
  "additionalProperties":false,
  "required":["action","wanted","running","refused","code","message","retryable","view","storey","rings","segments",
              "canvasPresents","drawn","readsFresh","drawnWithLastRead","lastError"]})json";

const NativeCommandRegistration kOverlayCommandRegistrations[] = {
    { "Overlay3D", &MakeRegisteredNativeCommand<Overlay3DCommand>, false, kOverlayActionInput, kOverlay3DOutput },
    { "Overlay2D", &MakeRegisteredNativeCommand<Overlay2DCommand>, false, kOverlayActionInput, kOverlay2DOutput },
};
// clang-format on

} // namespace

NativeCommandRegistrations GetOverlayCommandRegistrations ()
{
    return MakeRegistrationView (kOverlayCommandRegistrations);
}

} // namespace geomsrv
