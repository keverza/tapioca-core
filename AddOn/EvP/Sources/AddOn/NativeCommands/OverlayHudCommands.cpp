// NativeCommands/OverlayHudCommands -- what the user does on the overlays' HUD. See the
// header.
//
// ⚠️ THE EVENTS VERB IS GATE-FREE: NeedsMainThread() is false. It reads one mutex-guarded
// ring (ArchViz/OverlayHudEvents.hpp) and calls no ACAPI, and it must never call any: a
// script polls it ten times a second while the user presses the HUD on the main thread.
//
// ⚠️ THE STATE VERB IS NOT: the panels' state is the HUD engines', which live on the main
// thread, and setting the text size lays both views' HUDs out again.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/OverlayHudCommands.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/CommandRegistration.hpp"
#include "NativeCommands/CommandUtils.hpp"

#include "ArchViz/OverlayController.hpp"
#include "ArchViz/OverlayHudEvents.hpp"

#include <string>

namespace geomsrv {

namespace {

namespace control = archviz::overlaycontrol;
namespace hudevents = archviz::overlayhudevents;

GS::UniString Utf8 (const std::string& text)
{
    return GS::UniString (text.c_str (), CC_UTF8);
}

GS::ObjectState EventRecord (const hudevents::Event& event)
{
    GS::ObjectState os;
    os.Add ("seq", GS::Int64 (event.seq));
    os.Add ("timeMs", GS::Int64 (event.timeMs));
    os.Add ("view", Utf8 (event.view));
    os.Add ("kind", Utf8 (event.kind));
    os.Add ("layer", Utf8 (event.layer));
    os.Add ("panel", GS::Int32 (event.panel));
    os.Add ("title", Utf8 (event.title));
    os.Add ("id", Utf8 (event.id));
    os.Add ("item", GS::Int32 (event.item));
    os.Add ("value", event.value);
    os.Add ("text", Utf8 (event.text));
    os.Add ("final", event.final);
    return os;
}

class OverlayHudEventsCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "OverlayHudEvents";
    }

    bool NeedsMainThread () const override
    {
        return false;
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Int64 since = 0;
        GS::Int64 most = 256;
        if (params.Contains ("sinceSeq"))
            params.Get ("sinceSeq", since);
        if (params.Contains ("maxEvents"))
            params.Get ("maxEvents", most);
        const hudevents::Tail tail =
            hudevents::Since (since < 0 ? 0u : uint64_t (since), most < 1 ? size_t (1) : size_t (most));
        GS::Array<GS::ObjectState> events;
        for (const hudevents::Event& event : tail.events)
            events.Push (EventRecord (event));
        GS::ObjectState os;
        os.Add ("lastSeq", GS::Int64 (tail.lastSeq));
        // Reported, never hidden: a caller whose number fell off the ring reads the state
        // again (Tapioca.OverlayHud) rather than stitching an incomplete tail on.
        os.Add ("gap", tail.gap);
        os.Add ("events", events);
        return os;
    }
};

class OverlayHudCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "OverlayHud";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        double scale = 0.0;
        if (ReadReal (params, "fontScale", scale))
            control::SetHudFontScale (float (scale));
        if (params.Contains ("timeClicks")) {
            bool on = false;
            params.Get ("timeClicks", on);
            control::TimeHudClicks (on);
        }
        const control::HudReport report = control::Hud ();
        GS::Array<GS::ObjectState> panels;
        for (const control::HudPanel& panel : report.panels) {
            GS::ObjectState record;
            record.Add ("layer", Utf8 (panel.layer));
            record.Add ("panel", GS::Int32 (panel.panel));
            record.Add ("title", Utf8 (panel.title));
            record.Add ("docked", panel.docked);
            GS::Array<GS::ObjectState> values;
            for (const auto& held : panel.values) {
                GS::ObjectState value;
                value.Add ("id", Utf8 (held.first));
                value.Add ("value", held.second);
                values.Push (value);
            }
            record.Add ("values", values);
            panels.Push (record);
        }
        GS::ObjectState os;
        os.Add ("fontScale", double (report.fontScale));
        os.Add ("panels", panels);
        // Where a caller that reads the state starts reading the events from.
        os.Add ("lastSeq", GS::Int64 (hudevents::LastSeq ()));
        const control::ClickReport timing = control::HudClicks ();
        GS::Array<GS::ObjectState> clicks;
        for (const control::ClickRecord& click : timing.clicks) {
            GS::ObjectState record;
            record.Add ("target", Utf8 (click.target));
            record.Add ("windowClass", Utf8 (click.windowClass));
            record.Add ("ageMs", GS::Int32 (click.ageMilliseconds));
            record.Add ("complete", click.complete);
            record.Add ("busyMicroseconds", GS::Int32 (click.busyMicroseconds));
            record.Add ("firstIdleMicroseconds", GS::Int32 (click.firstIdleMicroseconds));
            record.Add ("bursts", GS::Int32 (click.bursts));
            record.Add ("layoutMicroseconds", GS::Int32 (click.layoutMicroseconds));
            record.Add ("layouts", GS::Int32 (click.layouts));
            record.Add ("redrawMicroseconds", GS::Int32 (click.redrawMicroseconds));
            record.Add ("redraws", GS::Int32 (click.redraws));
            clicks.Push (record);
        }
        GS::ObjectState clickTiming;
        clickTiming.Add ("armed", timing.armed);
        clickTiming.Add ("idles", GS::Int64 (timing.idles));
        clickTiming.Add ("clicks", clicks);
        os.Add ("clickTiming", clickTiming);
        return os;
    }
};

// clang-format off
constexpr const char kEventsInput[] = R"json({"type":"object","properties":{
    "sinceSeq":{"type":"integer","minimum":0,"description":"The last event already seen; 0 for every event held."},
    "maxEvents":{"type":"integer","minimum":1,"maximum":512}},
  "additionalProperties":false})json";

constexpr const char kEventsOutput[] = R"json({"type":"object","properties":{
    "lastSeq":{"type":"integer","minimum":0},
    "gap":{"type":"boolean","description":"Events after sinceSeq were dropped: read Tapioca.OverlayHud again."},
    "events":{"type":"array","items":{"type":"object","properties":{
        "seq":{"type":"integer","minimum":1},
        "timeMs":{"type":"integer","minimum":0,"description":"Wall clock, milliseconds since 1970 UTC."},
        "view":{"type":"string","enum":["3d","plan"]},
        "kind":{"type":"string","description":"dock, section, fontScale, or a control's kind."},
        "layer":{"type":"string"},"panel":{"type":"integer"},"title":{"type":"string"},
        "id":{"type":"string"},"item":{"type":"integer"},
        "value":{"type":"number"},"text":{"type":"string"},
        "final":{"type":"boolean","description":"False while a slider is still being dragged."}},
      "additionalProperties":false,
      "required":["seq","timeMs","view","kind","layer","panel","title","id","item","value","text","final"]}}},
  "additionalProperties":false,"required":["lastSeq","gap","events"]})json";

constexpr const char kHudInput[] = R"json({"type":"object","properties":{
    "fontScale":{"type":"number","minimum":0.5,"maximum":3,"description":"The HUD's text size; the nearest of its steps, 0.8 to 2."},
    "timeClicks":{"type":"boolean","description":"Arm (and clear) or disarm the click meter: how long each left press keeps the main thread busy, on the HUD or on any other window, a DG palette's among them."}},
  "additionalProperties":false})json";

constexpr const char kHudOutput[] = R"json({"type":"object","properties":{
    "fontScale":{"type":"number"},
    "panels":{"type":"array","items":{"type":"object","properties":{
        "layer":{"type":"string"},"panel":{"type":"integer","minimum":0},"title":{"type":"string"},
        "docked":{"type":"boolean"},
        "values":{"type":"array","description":"Its controls' values as the user left them, the tab bar's too.","items":{"type":"object","properties":{
            "id":{"type":"string"},"value":{"type":"number"}},
          "additionalProperties":false,"required":["id","value"]}}},
      "additionalProperties":false,"required":["layer","panel","title","docked","values"]}},
    "lastSeq":{"type":"integer","minimum":0},
    "clickTiming":{"type":"object","properties":{
        "armed":{"type":"boolean"},
        "idles":{"type":"integer","minimum":0,"description":"How often the main thread went idle while armed; 0: nothing could be timed."},
        "clicks":{"type":"array","items":{"type":"object","properties":{
            "target":{"type":"string","enum":["hud","view","other"]},"windowClass":{"type":"string"},
            "ageMs":{"type":"integer","minimum":0,"description":"Before the newest press."},
            "complete":{"type":"boolean","description":"Its half second is over."},
            "busyMicroseconds":{"type":"integer","minimum":0,"description":"The main thread busy in the half second after the press."},
            "firstIdleMicroseconds":{"type":"integer","minimum":0,"description":"From the press to the thread's first idle; 0: not yet."},
            "bursts":{"type":"integer","minimum":0},
            "layoutMicroseconds":{"type":"integer","minimum":0},"layouts":{"type":"integer","minimum":0},
            "redrawMicroseconds":{"type":"integer","minimum":0},"redraws":{"type":"integer","minimum":0}},
          "additionalProperties":false,
          "required":["target","windowClass","ageMs","complete","busyMicroseconds","firstIdleMicroseconds","bursts",
                      "layoutMicroseconds","layouts","redrawMicroseconds","redraws"]}}},
      "additionalProperties":false,"required":["armed","idles","clicks"]}},
  "additionalProperties":false,"required":["fontScale","panels","lastSeq","clickTiming"]})json";

const NativeCommandRegistration kOverlayHudCommandRegistrations[] = {
    { "OverlayHudEvents", &MakeRegisteredNativeCommand<OverlayHudEventsCommand>, false, kEventsInput, kEventsOutput },
    { "OverlayHud", &MakeRegisteredNativeCommand<OverlayHudCommand>, false, kHudInput, kHudOutput },
};
// clang-format on

} // namespace

NativeCommandRegistrations GetOverlayHudCommandRegistrations ()
{
    return MakeRegistrationView (kOverlayHudCommandRegistrations);
}

} // namespace geomsrv
