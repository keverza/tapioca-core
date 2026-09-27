// NativeCommands/ViewerDrawRecorderCommands -- Tapioca.ViewerDrawRecorder, the
// camera-independent record of every Archicad draw (ArchViz/Dxgi/DrawRecorder).
// A temporary diagnostic for the 2026-09-26/27 camera-lock regression: it needs
// the context hook installed -- the overlay started -- and nothing else, not a
// camera lock, a learned pass or a census group.
//
//   {frames: n}     arm: record every draw of the next n Presents (1..8)
//   {frames: n, afterModelFrames: m}
//                   ... starting once Archicad has redrawn the model m times:
//                   a moving camera, since a still one is not redrawn
//   {disarm: true}  stop and discard
//   {redraw: true}  also ask Archicad to redraw the 3D window, so a still
//                   camera still produces the frames a capture needs
//   {}              report; the records come back once the capture is done
//
// Each window comes back twice: `m`, copied before the draw, and `mAfter`, copied
// once it returned, with `kept` saying the binding was the same both times.
//
// The model's extracted bounds come back beside the records so the diagnostic
// can project known model points through every candidate matrix.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/ViewerDrawRecorderCommands.hpp"
#include "NativeCommands/CommandRegistration.hpp"

#include "ArchViz/Dxgi/ContextHook.hpp"
#include "ArchViz/Dxgi/DrawRecorder.hpp"
#include "ArchViz/Dxgi/HostOccluders.hpp"

#include <cstdio>
#include <string>

namespace geomsrv {

namespace av = geomsrv::archviz;
namespace rec = geomsrv::archviz::dxgi::drawrecorder;

namespace {

GS::UniString Hex (uint64_t value)
{
    char text[24] = {};
    std::snprintf (text, sizeof (text), "0x%llx", (unsigned long long) value);
    return GS::UniString (text, CC_UTF8);
}

GS::UniString Text (uint64_t value)
{
    return GS::UniString (std::to_string (value).c_str (), CC_UTF8);
}

const char* StateName (rec::State state)
{
    switch (state) {
        case rec::State::Idle:
            return "idle";
        case rec::State::Armed:
            return "armed";
        case rec::State::Capturing:
            return "capturing";
        case rec::State::Closing:
            return "closing";
        case rec::State::Done:
            return "done";
    }
    return "?";
}

const char* KindName (uint32_t kind)
{
    static const char* const kNames[] = { "indexed",          "direct", "indexedInstanced",
                                          "instanced",        "auto",   "indexedInstancedIndirect",
                                          "instancedIndirect" };
    return kind < sizeof (kNames) / sizeof (kNames[0]) ? kNames[kind] : "?";
}

const char* ReasonName (rec::Reason reason)
{
    switch (reason) {
        case rec::Reason::Admissible:
            return "admissible";
        case rec::Reason::CensusOff:
            return "censusOff";
        case rec::Reason::ViewUnbound:
            return "viewUnbound";
        case rec::Reason::ProjectionUnbound:
            return "projectionUnbound";
        case rec::Reason::ViewWindow:
            return "viewWindow";
        case rec::Reason::ProjectionWindow:
            return "projectionWindow";
    }
    return "?";
}

GS::ObjectState Row (const rec::DrawRecord& record)
{
    GS::ObjectState row;
    row.Add ("seq", (GS::Int32) record.sequence);
    row.Add ("frame", (GS::Int32) record.frame);
    row.Add ("kind", GS::UniString (KindName (record.kind), CC_UTF8));
    row.Add ("count", (GS::Int32) record.count);
    row.Add ("instances", (GS::Int32) record.instances);
    row.Add ("thread", (GS::Int32) record.thread);
    row.Add ("context", Hex (record.context));
    row.Add ("vs", Hex (record.vertexShader));
    row.Add ("ps", Hex (record.pixelShader));
    row.Add ("rtv", Hex (record.renderTarget));
    row.Add ("rtResource", Hex (record.renderResource));
    row.Add ("rtWidth", (GS::Int32) record.renderWidth);
    row.Add ("rtHeight", (GS::Int32) record.renderHeight);
    row.Add ("rtFormat", (GS::Int32) record.renderFormat);
    row.Add ("rtSamples", (GS::Int32) record.renderSamples);
    row.Add ("dsv", Hex (record.depthView));
    row.Add ("dsResource", Hex (record.depthResource));
    row.Add ("dsWidth", (GS::Int32) record.depthWidth);
    row.Add ("dsHeight", (GS::Int32) record.depthHeight);
    row.Add ("dsFormat", (GS::Int32) record.depthFormat);
    GS::Array<double> viewport;
    for (float value : record.viewport)
        viewport.Push (double (value));
    row.Add ("viewport", viewport);
    row.Add ("scenePass", Text (record.scenePass));
    row.Add ("passColour", Hex (record.passColour));
    row.Add ("passBoundaryHit", record.passBoundaryHit);
    row.Add ("signatureColour", Hex (record.signatureColour));
    row.Add ("signatureLearned", record.signatureLearned);
    row.Add ("modelGeneration", Text (record.modelGeneration));
    row.Add ("censusEnabled", record.censusEnabled);
    row.Add ("reason", GS::UniString (ReasonName (record.reason), CC_UTF8));
    GS::Array<GS::ObjectState> slots;
    for (size_t slot = 0; slot < rec::kWindows; ++slot) {
        GS::ObjectState window;
        window.Add ("buffer", Hex (record.buffer[slot]));
        window.Add ("bytes", (GS::Int32) record.bufferBytes[slot]);
        window.Add ("first", (GS::Int32) record.firstConstant[slot]);
        window.Add ("count", (GS::Int32) record.numConstants[slot]);
        window.Add ("copied", (GS::Int32) record.bytesCopied[slot]);
        GS::Array<double> matrix;
        for (float value : record.window[slot])
            matrix.Push (double (value));
        window.Add ("m", matrix);
        // The same window read again once the draw returned (DrawRecorder.hpp).
        window.Add ("copiedAfter", (GS::Int32) record.bytesCopiedAfter[slot]);
        window.Add ("kept", record.bindingKept[slot]);
        GS::Array<double> after;
        for (float value : record.windowAfter[slot])
            after.Push (double (value));
        window.Add ("mAfter", after);
        slots.Push (window);
    }
    row.Add ("slots", slots);
    return row;
}

class ViewerDrawRecorderCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "ViewerDrawRecorder";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        if (params.Contains ("frames")) {
            GS::Int32 frames = 2;
            params.Get ("frames", frames);
            GS::Int32 afterModelFrames = 0;
            if (params.Contains ("afterModelFrames"))
                params.Get ("afterModelFrames", afterModelFrames);
            rec::Arm (uint32_t (frames < 1 ? 1 : frames), uint32_t (afterModelFrames < 0 ? 0 : afterModelFrames));
        }
        else if (params.Contains ("disarm")) {
            bool disarm = false;
            params.Get ("disarm", disarm);
            if (disarm)
                rec::Disarm ();
        }
        // ⚠️ A STILL CAMERA DRAWS NOTHING, and a still camera is the only one
        // Archicad's own projection can be compared with exactly. A redraw makes
        // Archicad re-issue the scene without moving it.
        bool redraw = false;
        if (params.Contains ("redraw"))
            params.Get ("redraw", redraw);
        if (redraw)
            ACAPI_View_Redraw ();

        const rec::Status status = rec::GetStatus ();
        GS::ObjectState os;
        os.Add ("state", GS::UniString (StateName (status.state), CC_UTF8));
        os.Add ("hookInstalled", av::dxgi::ContextHookInstalled ());
        os.Add ("modelFramesWanted", (GS::Int32) status.modelFramesWanted);
        os.Add ("framesWanted", (GS::Int32) status.framesWanted);
        os.Add ("framesSeen", (GS::Int32) status.framesSeen);
        os.Add ("draws", (GS::Int32) status.draws);
        os.Add ("dropped", (GS::Int32) status.dropped);
        os.Add ("readbacksPending", (GS::Int32) status.readbacksPending);
        os.Add ("readbackFailures", (GS::Int32) status.readbackFailures);
        os.Add ("createFailures", (GS::Int32) status.createFailures);
        os.Add ("captures", Text (status.captures));

        const av::dxgi::hostocclusion::Stats host = av::dxgi::hostocclusion::GetStats ();
        os.Add ("boundsValid", host.boundsValid);
        GS::Array<double> boundsMin;
        GS::Array<double> boundsMax;
        for (size_t axis = 0; axis < 3; ++axis) {
            boundsMin.Push (double (host.boundsMin[axis]));
            boundsMax.Push (double (host.boundsMax[axis]));
        }
        os.Add ("boundsMin", boundsMin);
        os.Add ("boundsMax", boundsMax);

        GS::Array<GS::ObjectState> rows;
        if (status.state == rec::State::Done) {
            // Static: a full capture is far larger than a main-thread stack frame
            // should carry, and this verb runs on the main thread only.
            static rec::DrawRecord records[rec::kMaxDraws];
            const size_t count = rec::CopyRecords (records, rec::kMaxDraws);
            for (size_t i = 0; i < count; ++i)
                rows.Push (Row (records[i]));
        }
        os.Add ("records", rows);
        return os;
    }
};

// clang-format off
const NativeCommandRegistration kViewerDrawRecorderCommandRegistrations[] = {
    { "ViewerDrawRecorder", &MakeRegisteredNativeCommand<ViewerDrawRecorderCommand>, false,
      R"json({"type":"object","properties":{"frames":{"type":"integer","minimum":1,"maximum":8},"afterModelFrames":{"type":"integer","minimum":0,"maximum":120},"disarm":{"type":"boolean"},"redraw":{"type":"boolean"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{
        "state":{"type":"string","enum":["idle","armed","capturing","closing","done"]},
        "hookInstalled":{"type":"boolean"},"modelFramesWanted":{"type":"integer"},"framesWanted":{"type":"integer"},"framesSeen":{"type":"integer"},
        "draws":{"type":"integer"},"dropped":{"type":"integer"},"readbacksPending":{"type":"integer"},
        "readbackFailures":{"type":"integer"},"createFailures":{"type":"integer"},"captures":{"type":"string"},
        "boundsValid":{"type":"boolean"},
        "boundsMin":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
        "boundsMax":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
        "records":{"type":"array","items":{"type":"object","properties":{
          "seq":{"type":"integer"},"frame":{"type":"integer"},
          "kind":{"type":"string","enum":["indexed","direct","indexedInstanced","instanced","auto","indexedInstancedIndirect","instancedIndirect","?"]},
          "count":{"type":"integer"},"instances":{"type":"integer"},"thread":{"type":"integer"},
          "context":{"type":"string"},"vs":{"type":"string"},"ps":{"type":"string"},
          "rtv":{"type":"string"},"rtResource":{"type":"string"},"rtWidth":{"type":"integer"},"rtHeight":{"type":"integer"},
          "rtFormat":{"type":"integer"},"rtSamples":{"type":"integer"},
          "dsv":{"type":"string"},"dsResource":{"type":"string"},"dsWidth":{"type":"integer"},"dsHeight":{"type":"integer"},
          "dsFormat":{"type":"integer"},
          "viewport":{"type":"array","items":{"type":"number"},"minItems":4,"maxItems":4},
          "scenePass":{"type":"string"},"passColour":{"type":"string"},"passBoundaryHit":{"type":"boolean"},
          "signatureColour":{"type":"string"},"signatureLearned":{"type":"boolean"},"modelGeneration":{"type":"string"},
          "censusEnabled":{"type":"boolean"},
          "reason":{"type":"string","enum":["admissible","censusOff","viewUnbound","projectionUnbound","viewWindow","projectionWindow","?"]},
          "slots":{"type":"array","minItems":4,"maxItems":4,"items":{"type":"object","properties":{
            "buffer":{"type":"string"},"bytes":{"type":"integer"},"first":{"type":"integer"},"count":{"type":"integer"},
            "copied":{"type":"integer"},"m":{"type":"array","items":{"type":"number"},"minItems":16,"maxItems":16},
            "copiedAfter":{"type":"integer"},"kept":{"type":"boolean"},
            "mAfter":{"type":"array","items":{"type":"number"},"minItems":16,"maxItems":16}},
            "additionalProperties":false,"required":["buffer","bytes","first","count","copied","m","copiedAfter","kept","mAfter"]}}},
          "additionalProperties":false,
          "required":["seq","frame","kind","count","instances","thread","context","vs","ps","rtv","rtResource","rtWidth",
                      "rtHeight","rtFormat","rtSamples","dsv","dsResource","dsWidth","dsHeight","dsFormat","viewport",
                      "scenePass","passColour","passBoundaryHit","signatureColour","signatureLearned","modelGeneration",
                      "censusEnabled","reason","slots"]}}},
      "additionalProperties":false,
      "required":["state","hookInstalled","modelFramesWanted","framesWanted","framesSeen","draws","dropped","readbacksPending",
                  "readbackFailures","createFailures","captures","boundsValid","boundsMin","boundsMax","records"]})json" },
};
// clang-format on

} // namespace

NativeCommandRegistrations GetViewerDrawRecorderCommandRegistrations ()
{
    return MakeRegistrationView (kViewerDrawRecorderCommandRegistrations);
}

} // namespace geomsrv
