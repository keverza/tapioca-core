#include "APIEnvir.h"
#include "ACAPinc.h"
#include "NativeCommands/TestCommands.hpp"
#include "AddOnCommands.hpp"
#include "AddOnVersion.hpp"
#include "TestBuildInfo.hpp"
#include "ArchViz/MassingSlices.hpp"
#include "ArchViz/SlabSliceSource.hpp"
#include "Metadata/MetadataStorage.hpp"
#include "Python/MainThreadGate.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <mutex>
#include <set>

namespace geomsrv {
namespace {
namespace av = archviz;
using Clock = std::chrono::steady_clock;
struct Job {
    std::mutex mutex;
    std::string id, state = "running", error;
    std::vector<std::string> guids;
    std::vector<uint64_t> stamps;
    av::ProjectStoreys storeys;
    av::massingslices::Result result;
    Clock::time_point started = Clock::now ();
    double elapsedMs = 0;
};
std::shared_ptr<Job> s_job;
std::future<void> s_worker;
uint64_t s_nextId = 0;

GS::UniString Text (const std::string& value)
{
    return GS::UniString (value.c_str (), CC_UTF8);
}
double Elapsed (Clock::time_point start)
{
    return std::chrono::duration<double, std::milli> (Clock::now () - start).count ();
}
bool WorkerBusy ()
{
    return s_worker.valid () && s_worker.wait_for (std::chrono::seconds (0)) != std::future_status::ready;
}

constexpr const char kEmpty[] = R"json({"type":"object","properties":{},"additionalProperties":false})json";
constexpr const char kPing[] =
    R"json({"type":"object","properties":{"version":{"type":"string"},"buildHash":{"type":"string"},"protocolVersion":{"type":"integer"},"ready":{"type":"boolean"},"workflows":{"type":"array","items":{"type":"string"}},"captureSupported":{"type":"boolean"}},"required":["version","buildHash","protocolVersion","ready","workflows","captureSupported"],"additionalProperties":false})json";
constexpr const char kRun[] =
    R"json({"type":"object","properties":{"workflow":{"type":"string","enum":["massing_feasibility"]},"inputs":{"type":"object","properties":{"elementGuids":{"type":"array","minItems":1,"maxItems":8,"uniqueItems":true,"items":{"type":"string","format":"uuid"}}},"required":["elementGuids"],"additionalProperties":false},"mode":{"type":"string","enum":["preview"]},"seed":{"type":"integer","minimum":0,"maximum":2147483647}},"required":["workflow","inputs","mode","seed"],"additionalProperties":false})json";
constexpr const char kStarted[] =
    R"json({"type":"object","properties":{"jobId":{"type":"string"}},"required":["jobId"],"additionalProperties":false})json";
constexpr const char kJobInput[] =
    R"json({"type":"object","properties":{"jobId":{"type":"string","minLength":1}},"required":["jobId"],"additionalProperties":false})json";
constexpr const char kStatus[] =
    R"json({"type":"object","properties":{"jobId":{"type":"string"},"state":{"type":"string","enum":["running","completed","failed","cancelled"]},"progress":{"type":"number","minimum":0,"maximum":1},"elapsedMs":{"type":"number"},"errorMessage":{"type":"string"},"result":{"type":"object","properties":{"basis":{"type":"string","enum":["unclipped_slab_prisms"]},"rawAreaM2":{"type":"number"},"rawVolumeM3":{"type":"number"},"floorCount":{"type":"integer"},"rows":{"type":"array","items":{"type":"object","properties":{"guid":{"type":"string"},"story":{"type":"integer"},"z":{"type":"number"},"height":{"type":"number"},"areaM2":{"type":"number"},"volumeM3":{"type":"number"}},"required":["guid","story","z","height","areaM2","volumeM3"],"additionalProperties":false}}},"required":["basis","rawAreaM2","rawVolumeM3","floorCount","rows"],"additionalProperties":false}},"required":["jobId","state","progress","elapsedMs","errorMessage"],"additionalProperties":false})json";
constexpr const char kDiagnostics[] =
    R"json({"type":"object","properties":{"buildHash":{"type":"string"},"workerBusy":{"type":"boolean"},"retainedJobs":{"type":"integer"},"gateQueueDepth":{"type":"integer"},"gateTimeouts":{"type":"integer"},"scope":{"type":"string"}},"required":["buildHash","workerBusy","retainedJobs","gateQueueDepth","gateTimeouts","scope"],"additionalProperties":false})json";
constexpr const char kReset[] =
    R"json({"type":"object","properties":{"workerBusy":{"type":"boolean"}},"required":["workerBusy"],"additionalProperties":false})json";
constexpr const char kCapture[] =
    R"json({"type":"object","properties":{"supported":{"type":"boolean","enum":[false]},"reason":{"type":"string"}},"required":["supported","reason"],"additionalProperties":false})json";

class TestPingCommand : public MainThreadCommand {
  public:
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        GS::ObjectState out;
        out.Add ("version", ADDON_VERSION);
        out.Add ("buildHash", TAPIOCA_TEST_BUILD_HASH);
        out.Add ("protocolVersion", 1);
        const auto gate = evp::MainThreadGate::Get ().Snapshot ();
        out.Add ("ready", gate.mainThreadKnown && !gate.shuttingDown);
        GS::Array<GS::UniString> workflows;
        workflows.Push ("massing_feasibility");
        out.Add ("workflows", workflows);
        out.Add ("captureSupported", false);
        return out;
    }
};

class TestRunWorkflowCommand : public MainThreadCommand {
  public:
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        if (WorkerBusy ())
            return NativeCommandResult::Failure (
                "A test calculation is still draining; poll diagnostics before retrying.");
        if (s_worker.valid ())
            s_worker.get ();
        GS::ObjectState inputs;
        GS::Array<GS::UniString> ids;
        params.Get ("inputs", inputs);
        inputs.Get ("elementGuids", ids);
        if (ids.IsEmpty () || ids.GetSize () > 8)
            return NativeCommandResult::Failure ("Supply 1..8 explicit slab GUIDs.");
        auto job = std::make_shared<Job> ();
        std::set<std::string> unique;
        for (const auto& id : ids) {
            const API_Guid guid = APIGuidFromString (id.ToCStr ().Get ());
            if (guid == APINULLGuid)
                return NativeCommandResult::Failure ("Invalid slab GUID.");
            const std::string canonical = APIGuidToString (guid).ToCStr ().Get ();
            if (!unique.insert (canonical).second)
                return NativeCommandResult::Failure ("Duplicate slab GUID.");
            job->guids.push_back (canonical);
            GS::Array<API_Guid> operators;
            const auto err = ACAPI_Element_SolidLink_GetOperators (guid, &operators);
            if (err != NoError || !operators.IsEmpty ())
                return NativeCommandResult::Failure ("MVP requires slabs with verified empty SEO operator lists.");
        }
        const auto storeys = av::ReadStoreys ();
        job->storeys = storeys;
        const auto reading = av::slabsource::Read (job->guids, storeys);
        if (!reading.skipped.empty ())
            return NativeCommandResult::Failure (
                Text (reading.skipped.front ().guid + ": " + reading.skipped.front ().reason));
        std::vector<av::massingslices::Input> snapshot;
        size_t points = 0;
        for (const auto& slab : reading.slabs) {
            if (slab.slopedEdges)
                return NativeCommandResult::Failure (
                    "MVP does not support sloped slab edges; an operated body is required.");
            points += slab.outer.xy.size ();
            for (const auto& hole : slab.holes)
                points += hole.xy.size ();
            if (points > 8192)
                return NativeCommandResult::Failure ("Test snapshot exceeds 4096 polygon points.");
            av::massingslices::Input input;
            input.slab = slab;
            bool present = false;
            std::string error;
            if (!metadata::storage::Read (slab.guid, input.metadata, present, error))
                return NativeCommandResult::Failure (Text (error));
            snapshot.push_back (std::move (input));
        }
        job->stamps = av::slabsource::Stamps (job->guids);
        job->id = std::to_string (++s_nextId);
        // Same production calculator as MassingSlicesModel; no selection, overlay publication,
        // project writes or ACAPI on this worker. Seed is accepted but this algorithm is non-random.
        s_worker = std::async (std::launch::async, [job, snapshot = std::move (snapshot), storeys] () {
            av::massingslices::Result result;
            std::string error;
            bool complete = false;
            try {
                complete = av::massingslices::Build (snapshot, storeys, nullptr, result, error) && result.complete;
                if (!complete && error.empty ())
                    error = "Production calculator returned an incomplete result.";
            }
            catch (const std::exception& ex) {
                error = ex.what ();
            }
            catch (...) {
                error = "Unknown exception in massing calculation.";
            }
            std::lock_guard lock (job->mutex);
            if (job->state != "running")
                return;
            job->elapsedMs = Elapsed (job->started);
            job->error = std::move (error);
            job->state = complete ? "completed" : "failed";
            job->result = std::move (result);
        });
        s_job = job;
        GS::ObjectState out;
        out.Add ("jobId", Text (job->id));
        return out;
    }
};

class TestGetStatusCommand : public MainThreadCommand {
  public:
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString id;
        params.Get ("jobId", id);
        const auto job = s_job;
        if (!job || id != Text (job->id))
            return NativeCommandResult::Failure ("Unknown or reset test job.");
        const auto stamps = av::slabsource::Stamps (job->guids);
        const auto storeys = av::ReadStoreys ();
        bool unchanged =
            stamps == job->stamps && storeys.levels == job->storeys.levels && storeys.indices == job->storeys.indices;
        for (const auto& guid : job->guids) {
            GS::Array<API_Guid> operators;
            unchanged =
                ACAPI_Element_SolidLink_GetOperators (APIGuidFromString (guid.c_str ()), &operators) == NoError &&
                operators.IsEmpty () && unchanged;
        }
        std::lock_guard lock (job->mutex);
        if (!unchanged) {
            job->state = "failed";
            job->error = "Source geometry, SEO or stories changed; rerun from the known fixture.";
        }
        GS::ObjectState out;
        out.Add ("jobId", Text (job->id));
        out.Add ("state", Text (job->state));
        out.Add ("progress", job->state == "running" ? 0.0 : 1.0);
        out.Add ("elapsedMs", job->state == "running" ? Elapsed (job->started) : job->elapsedMs);
        out.Add ("errorMessage", Text (job->error));
        if (job->state == "completed") {
            GS::ObjectState result;
            result.Add ("basis", "unclipped_slab_prisms");
            result.Add ("rawAreaM2", job->result.rawArea);
            result.Add ("rawVolumeM3", job->result.rawVolume);
            result.Add ("floorCount", int (job->result.rows.size ()));
            GS::Array<GS::ObjectState> rows;
            for (const auto& row : job->result.rows) {
                GS::ObjectState record;
                record.Add ("guid", Text (row.guid));
                record.Add ("story", row.story);
                record.Add ("z", row.z);
                record.Add ("height", row.floorHeight);
                record.Add ("areaM2", row.rawArea);
                record.Add ("volumeM3", row.rawVolume);
                rows.Push (record);
            }
            result.Add ("rows", rows);
            out.Add ("result", result);
        }
        return out;
    }
};

class TestGetDiagnosticsCommand : public MainThreadCommand {
  public:
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        GS::ObjectState out;
        const auto gate = evp::MainThreadGate::Get ().Snapshot ();
        out.Add ("buildHash", TAPIOCA_TEST_BUILD_HASH);
        out.Add ("workerBusy", WorkerBusy ());
        out.Add ("retainedJobs", s_job ? 1 : 0);
        out.Add ("gateQueueDepth", GS::Int64 (gate.queueDepth));
        out.Add ("gateTimeouts", GS::Int64 (gate.timeouts));
        out.Add ("scope", "read-only slab prism metrics; no setbacks, facade, capture or overlay controls");
        return out;
    }
};
class TestResetCommand : public MainThreadCommand {
  public:
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        ResetTestJobs ();
        GS::ObjectState out;
        out.Add ("workerBusy", WorkerBusy ());
        return out;
    }
};
class TestCaptureCommand : public MainThreadCommand {
  public:
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        GS::ObjectState out;
        out.Add ("supported", false);
        out.Add ("reason", "Viewport and overlay capture is not part of the read-only MVP.");
        return out;
    }
};

const NativeCommandRegistration registrations[] = {
    { "TestPing", &MakeRegisteredNativeCommand<TestPingCommand>, true, kEmpty, kPing },
    { "TestRunWorkflow", &MakeRegisteredNativeCommand<TestRunWorkflowCommand>, false, kRun, kStarted },
    { "TestGetStatus", &MakeRegisteredNativeCommand<TestGetStatusCommand>, true, kJobInput, kStatus },
    { "TestGetDiagnostics", &MakeRegisteredNativeCommand<TestGetDiagnosticsCommand>, true, kEmpty, kDiagnostics },
    { "TestReset", &MakeRegisteredNativeCommand<TestResetCommand>, true, kEmpty, kReset },
    { "TestCapture", &MakeRegisteredNativeCommand<TestCaptureCommand>, true, kEmpty, kCapture },
};

// Compatibility namespace requested by the live runner; domain implementation and schemas
// remain in the canonical Tapioca registry, also callable as Tapioca.TestPing, etc.
class TestJsonCommand final : public MainThreadCommand {
  public:
    explicit TestJsonCommand (const NativeCommandRegistration& registration) : registration (registration)
    {
    }
    GS::String GetNamespace () const override
    {
        return "TapiocaTest";
    }
    GS::String GetName () const override
    {
        return registration.name + 4;
    }
    GS::Optional<GS::UniString> GetInputParametersSchema () const override
    {
        return GS::UniString (registration.inputSchema);
    }
    GS::Optional<GS::UniString> GetResponseSchema () const override
    {
        return GS::UniString (registration.responseSchema);
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        auto result = std::make_shared<NativeCommandResult> ();
        const GS::String name (registration.name);
        GS::UniString error;
        if (!evp::MainThreadGate::Get ().Invoke (
                [result, params, name] () { *result = ExecuteNativeCommand (name, params); },
                evp::MainThreadGate::DefaultTimeoutMs, error))
            return NativeCommandResult::Failure (error);
        return std::move (*result);
    }

  private:
    const NativeCommandRegistration& registration;
};
} // namespace

NativeCommandRegistrations GetTestCommandRegistrations ()
{
    return MakeRegistrationView (registrations);
}
GSErrCode InstallTestJsonCommands ()
{
    for (const auto& registration : registrations) {
        const auto err =
            ACAPI_AddOnAddOnCommunication_InstallAddOnCommandHandler (GS::NewOwned<TestJsonCommand> (registration));
        if (err != NoError)
            return err;
    }
    return NoError;
}
void ResetTestJobs ()
{
    if (s_job) {
        std::lock_guard lock (s_job->mutex);
        s_job->state = "cancelled";
    }
    s_job.reset ();
}
void ShutdownTestJobs ()
{
    ResetTestJobs ();
    if (s_worker.valid ())
        s_worker.get ();
}
} // namespace geomsrv
