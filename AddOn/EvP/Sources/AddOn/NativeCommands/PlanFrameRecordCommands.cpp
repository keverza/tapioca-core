// NativeCommands/PlanFrameRecordCommands -- Tapioca.PlanFrameRecord, the floor-plan
// frame record (ArchViz/PlanFrameSession). A diagnostic: it needs the floor plan in
// front and every overlay off, and it observes only.
//
//   {start: true, seconds: n}   record n seconds (2..30) while the user pans and zooms
//   {stop: true}                end the recording early; what it has is analysed
//   {reset: true}               forget the last record and free its memory
//   {}                          status; once `done`, where the raw record was written
//
// The verdict is the diagnostic's: this verb returns counts and the paths of the raw
// record (logs\plan_frames\), so the analysis can change without a rebuild.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/PlanFrameRecordCommands.hpp"
#include "NativeCommands/CommandRegistration.hpp"

#include "ArchViz/PlanFrameSession.hpp"

#include <string>

namespace geomsrv {

namespace pf = geomsrv::archviz::planframes;

namespace {

GS::UniString Utf8 (const std::string& text)
{
    return GS::UniString (text.c_str (), CC_UTF8);
}

GS::UniString Text (uint64_t value)
{
    return GS::UniString (std::to_string (value).c_str (), CC_UTF8);
}

class PlanFrameRecordCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "PlanFrameRecord";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        std::string error;
        bool start = false;
        if (params.Contains ("start"))
            params.Get ("start", start);
        bool stop = false;
        if (params.Contains ("stop"))
            params.Get ("stop", stop);
        bool reset = false;
        if (params.Contains ("reset"))
            params.Get ("reset", reset);
        if (reset)
            pf::Reset ();
        if (stop)
            pf::Stop ("stopped by the diagnostic");
        if (start) {
            GS::Int32 seconds = 8;
            if (params.Contains ("seconds"))
                params.Get ("seconds", seconds);
            pf::Start (uint32_t (seconds < 2 ? 2 : seconds), error);
        }

        const pf::SessionStatus status = pf::GetStatus ();
        GS::ObjectState os;
        os.Add ("state", Utf8 (pf::SessionStateName (status.state)));
        os.Add ("error", Utf8 (error));
        os.Add ("reason", Utf8 (status.reason));
        os.Add ("seconds", (GS::Int32) status.seconds);
        os.Add ("elapsedMs", Text (status.elapsedMs));
        os.Add ("presents", Text (status.presents));
        os.Add ("presentsDropped", Text (status.presentsDropped));
        os.Add ("targetPresents", Text (status.targetPresents));
        os.Add ("framesSubmitted", Text (status.framesSubmitted));
        os.Add ("framesReady", Text (status.framesReady));
        os.Add ("slotsBusy", Text (status.slotsBusy));
        os.Add ("readbackFailures", Text (status.readbackFailures));
        os.Add ("createFailures", Text (status.createFailures));
        os.Add ("unsupportedFormat", Text (status.unsupportedFormat));
        os.Add ("targetChanges", Text (status.targetChanges));
        os.Add ("format", (GS::Int32) status.format);
        os.Add ("samplesEntry", Text (status.samplesEntry));
        os.Add ("samplesExit", Text (status.samplesExit));
        os.Add ("samplesTimer", Text (status.samplesTimer));
        os.Add ("samplesInvalid", Text (status.samplesInvalid));
        os.Add ("samplesTorn", Text (status.samplesTorn));
        os.Add ("samplesDropped", Text (status.samplesDropped));
        os.Add ("canvasMessages", Text (status.canvasMessages));
        os.Add ("retrievedMessages", Text (status.retrievedMessages));
        os.Add ("idleRedraws", (GS::Int32) status.idleRedraws);
        os.Add ("mainThread", Text (status.mainThread));
        os.Add ("dpi", status.dpi);
        os.Add ("canvasClass", Utf8 (status.canvasClass));
        os.Add ("canvasWidth", (GS::Int32) status.canvasWidth);
        os.Add ("canvasHeight", (GS::Int32) status.canvasHeight);
        os.Add ("target", Utf8 (status.target));
        os.Add ("targetClass", Utf8 (status.targetClass));
        os.Add ("targetRelation", Utf8 (status.targetRelation));
        os.Add ("pairs", (GS::Int32) status.pairs);
        os.Add ("pairsValid", (GS::Int32) status.pairsValid);
        os.Add ("analysisMs", Text (status.analysisMs));
        os.Add ("jsonPath", Utf8 (status.jsonPath));
        os.Add ("framesPath", Utf8 (status.framesPath));
        return os;
    }
};

// clang-format off
const NativeCommandRegistration kPlanFrameRecordCommandRegistrations[] = {
    { "PlanFrameRecord", &MakeRegisteredNativeCommand<PlanFrameRecordCommand>, false,
      R"json({"type":"object","properties":{"start":{"type":"boolean"},"seconds":{"type":"integer","minimum":2,"maximum":30},"stop":{"type":"boolean"},"reset":{"type":"boolean"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{
        "state":{"type":"string","enum":["idle","recording","closing","analysing","done","failed","?"]},
        "error":{"type":"string"},"reason":{"type":"string"},"seconds":{"type":"integer"},"elapsedMs":{"type":"string"},
        "presents":{"type":"string"},"presentsDropped":{"type":"string"},"targetPresents":{"type":"string"},
        "framesSubmitted":{"type":"string"},"framesReady":{"type":"string"},"slotsBusy":{"type":"string"},
        "readbackFailures":{"type":"string"},"createFailures":{"type":"string"},"unsupportedFormat":{"type":"string"},
        "targetChanges":{"type":"string"},"format":{"type":"integer"},
        "samplesEntry":{"type":"string"},"samplesExit":{"type":"string"},"samplesTimer":{"type":"string"},
        "samplesInvalid":{"type":"string"},"samplesTorn":{"type":"string"},"samplesDropped":{"type":"string"},
        "canvasMessages":{"type":"string"},"retrievedMessages":{"type":"string"},"idleRedraws":{"type":"integer"},"mainThread":{"type":"string"},
        "dpi":{"type":"number"},"canvasClass":{"type":"string"},"canvasWidth":{"type":"integer"},"canvasHeight":{"type":"integer"},
        "target":{"type":"string"},"targetClass":{"type":"string"},"targetRelation":{"type":"string"},
        "pairs":{"type":"integer"},"pairsValid":{"type":"integer"},"analysisMs":{"type":"string"},
        "jsonPath":{"type":"string"},"framesPath":{"type":"string"}},
      "additionalProperties":false,
      "required":["state","error","reason","seconds","elapsedMs","presents","presentsDropped","targetPresents","framesSubmitted",
                  "framesReady","slotsBusy","readbackFailures","createFailures","unsupportedFormat","targetChanges","format",
                  "samplesEntry","samplesExit","samplesTimer","samplesInvalid","samplesTorn","samplesDropped","canvasMessages",
                  "retrievedMessages","idleRedraws","mainThread","dpi","canvasClass","canvasWidth","canvasHeight","target","targetClass",
                  "targetRelation","pairs","pairsValid","analysisMs","jsonPath","framesPath"]})json" },
};
// clang-format on

} // namespace

NativeCommandRegistrations GetPlanFrameRecordCommandRegistrations ()
{
    return MakeRegistrationView (kPlanFrameRecordCommandRegistrations);
}

} // namespace geomsrv
