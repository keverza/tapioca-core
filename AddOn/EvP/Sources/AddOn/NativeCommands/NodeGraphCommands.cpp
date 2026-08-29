#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/NodeGraphCommands.hpp"

#include "NodeGraph/GraphReports.hpp"
#include "NodeGraph/GraphRuntimeState.hpp"

#include <exception>
#include <string>

namespace geomsrv {
namespace {

namespace graph = evp::nodegraph;

// ---------------------------------------------------------------------------
// Value encoding
//
// One self-describing encoding for every graph value, so a client never has to
// know which node produced a port to read it. This is what makes the bridge
// frontend-agnostic: the previous shape carried a bare `numberValue` and could
// not express a string, a point or a reference at all.
//
// Two deliberate asymmetries:
//
//  * INBOUND (parameters) accepts scalars only. A graph is authored from small
//    typed values; nothing needs to push a mesh in over JSON.
//  * OUTBOUND summarizes. A mesh crosses as its counts, and a nested list
//    crosses as its size with `truncated` set. Model geometry does not cross the
//    browser bridge - it reaches the preview hosts through RetainedPreviewStore.
//
// The encoding is therefore two levels deep by construction, which also means
// the schemas below need no recursive $ref.
// ---------------------------------------------------------------------------

// How many items of a list are spelled out before the encoding reports a count
// instead. A client that needs more asks the node for its preview.
constexpr size_t kMaxEncodedListItems = 256;

// Every verb takes an optional graphId. Omitting it means the default graph, so
// a single-graph client needs to know nothing about the addressing.
constexpr const char kGraphInputSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1}},"additionalProperties":false,"required":[]})json";

constexpr const char kCatalogResponseSchema[] =
    R"json({"type":"object","properties":{"nodeTypes":{"type":"array","items":{"type":"object","properties":{"nodeType":{"type":"string"},"label":{"type":"string"},"category":{"type":"string"},"description":{"type":"string"},"executionDomain":{"type":"string","enum":["worker","archicadMainThread","renderThread"]},"effect":{"type":"string","enum":["pure","readModel","hostUiWrite"]},"generations":{"type":"array","items":{"type":"string","enum":["project","selection"]}},"inputs":{"type":"array","items":{"type":"object","properties":{"portId":{"type":"string"},"label":{"type":"string"},"valueType":{"type":"string"},"required":{"type":"boolean"},"acceptsMultiple":{"type":"boolean"}},"additionalProperties":false,"required":["portId","label","valueType","required","acceptsMultiple"]}},"outputs":{"type":"array","items":{"type":"object","properties":{"portId":{"type":"string"},"label":{"type":"string"},"valueType":{"type":"string"}},"additionalProperties":false,"required":["portId","label","valueType"]}},"parameters":{"type":"array","items":{"type":"object","properties":{"parameterId":{"type":"string"},"label":{"type":"string"},"valueType":{"type":"string"},"required":{"type":"boolean"},"defaultValue":{"$ref":"#/$defs/parameterValue"}},"additionalProperties":false,"required":["parameterId","label","valueType","required"]}}},"additionalProperties":false,"required":["nodeType","label","category","description","executionDomain","effect","generations","inputs","outputs","parameters"]}}},"additionalProperties":false,"required":["nodeTypes"],"$defs":{"leafValue":{"type":"object","properties":{"valueType":{"type":"string","enum":["absent","bool","integer","double","string","point3","polyline","polygon","mesh","archicadElementRef","list"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"}},"itemCount":{"type":"integer","minimum":0},"truncated":{"type":"boolean"}},"additionalProperties":false,"required":["valueType"]},"value":{"type":"object","properties":{"valueType":{"type":"string","enum":["absent","bool","integer","double","string","point3","polyline","polygon","mesh","archicadElementRef","list"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"}},"itemCount":{"type":"integer","minimum":0},"truncated":{"type":"boolean"},"items":{"type":"array","items":{"$ref":"#/$defs/leafValue"}}},"additionalProperties":false,"required":["valueType"]},"parameterValue":{"type":"object","properties":{"valueType":{"type":"string","enum":["bool","integer","double","string","point3","archicadElementRef"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"additionalProperties":false,"required":["valueType"]}}})json";

constexpr const char kStateResponseSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string"},"revision":{"type":"integer","minimum":0},"lastRunId":{"type":"integer","minimum":0},"lastEventSeq":{"type":"integer","minimum":0},"nodes":{"type":"array","items":{"type":"object","properties":{"nodeId":{"type":"string"},"nodeType":{"type":"string"},"parameters":{"type":"array","items":{"type":"object","properties":{"parameterId":{"type":"string"},"value":{"$ref":"#/$defs/value"}},"additionalProperties":false,"required":["parameterId","value"]}}},"additionalProperties":false,"required":["nodeId","nodeType","parameters"]}},"edges":{"type":"array","items":{"type":"object","properties":{"sourceNode":{"type":"string"},"sourcePort":{"type":"string"},"targetNode":{"type":"string"},"targetPort":{"type":"string"}},"additionalProperties":false,"required":["sourceNode","sourcePort","targetNode","targetPort"]}}},"additionalProperties":false,"required":["graphId","revision","lastRunId","lastEventSeq","nodes","edges"],"$defs":{"leafValue":{"type":"object","properties":{"valueType":{"type":"string","enum":["absent","bool","integer","double","string","point3","polyline","polygon","mesh","archicadElementRef","list"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"}},"itemCount":{"type":"integer","minimum":0},"truncated":{"type":"boolean"}},"additionalProperties":false,"required":["valueType"]},"value":{"type":"object","properties":{"valueType":{"type":"string","enum":["absent","bool","integer","double","string","point3","polyline","polygon","mesh","archicadElementRef","list"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"}},"itemCount":{"type":"integer","minimum":0},"truncated":{"type":"boolean"},"items":{"type":"array","items":{"$ref":"#/$defs/leafValue"}}},"additionalProperties":false,"required":["valueType"]},"parameterValue":{"type":"object","properties":{"valueType":{"type":"string","enum":["bool","integer","double","string","point3","archicadElementRef"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"additionalProperties":false,"required":["valueType"]}}})json";

// `numberValue` is retained as a deprecated alias so the existing editor build
// keeps working across this change. New clients send `value`.
constexpr const char kEditInputSchema[] =
    R"json({"oneOf":[{"type":"object","properties":{"graphId":{"type":"string","minLength":1},"editKind":{"const":"addNode"},"nodeId":{"type":"string","minLength":1},"nodeType":{"type":"string","minLength":1},"parameters":{"type":"array","items":{"type":"object","properties":{"parameterId":{"type":"string","minLength":1},"value":{"$ref":"#/$defs/parameterValue"}},"additionalProperties":false,"required":["parameterId","value"]}},"numberValue":{"type":"number"}},"additionalProperties":false,"required":["editKind","nodeId","nodeType"]},{"type":"object","properties":{"graphId":{"type":"string","minLength":1},"editKind":{"const":"removeNode"},"nodeId":{"type":"string","minLength":1}},"additionalProperties":false,"required":["editKind","nodeId"]},{"type":"object","properties":{"graphId":{"type":"string","minLength":1},"editKind":{"enum":["connect","disconnect"]},"sourceNode":{"type":"string","minLength":1},"sourcePort":{"type":"string","minLength":1},"targetNode":{"type":"string","minLength":1},"targetPort":{"type":"string","minLength":1}},"additionalProperties":false,"required":["editKind","sourceNode","sourcePort","targetNode","targetPort"]},{"type":"object","properties":{"graphId":{"type":"string","minLength":1},"editKind":{"const":"setParam"},"nodeId":{"type":"string","minLength":1},"parameterId":{"type":"string","minLength":1},"value":{"$ref":"#/$defs/parameterValue"},"numberValue":{"type":"number"}},"additionalProperties":false,"required":["editKind","nodeId","parameterId"]}],"$defs":{"leafValue":{"type":"object","properties":{"valueType":{"type":"string","enum":["absent","bool","integer","double","string","point3","polyline","polygon","mesh","archicadElementRef","list"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"}},"itemCount":{"type":"integer","minimum":0},"truncated":{"type":"boolean"}},"additionalProperties":false,"required":["valueType"]},"value":{"type":"object","properties":{"valueType":{"type":"string","enum":["absent","bool","integer","double","string","point3","polyline","polygon","mesh","archicadElementRef","list"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"}},"itemCount":{"type":"integer","minimum":0},"truncated":{"type":"boolean"},"items":{"type":"array","items":{"$ref":"#/$defs/leafValue"}}},"additionalProperties":false,"required":["valueType"]},"parameterValue":{"type":"object","properties":{"valueType":{"type":"string","enum":["bool","integer","double","string","point3","archicadElementRef"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"additionalProperties":false,"required":["valueType"]}}})json";

constexpr const char kEditResponseSchema[] =
    R"json({"type":"object","properties":{"revision":{"type":"integer","minimum":1},"dirtyNodes":{"type":"array","items":{"type":"string"}}},"additionalProperties":false,"required":["revision","dirtyNodes"]})json";

// allowSideEffects defaults to FALSE and must be sent deliberately. A preview,
// a watch and an auto-evaluated branch all leave it out, which is what keeps a
// graph from changing the user's selection while they are editing.
constexpr const char kEvaluateInputSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1},"targets":{"type":"array","items":{"type":"string","minLength":1}},"mode":{"type":"string","enum":["incremental","forced"]},"allowSideEffects":{"type":"boolean"}},"additionalProperties":false,"required":[]})json";

constexpr const char kEvaluateResponseSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string"},"runId":{"type":"integer","minimum":0},"lastEventSeq":{"type":"integer","minimum":0},"revision":{"type":"integer","minimum":0},"succeeded":{"type":"boolean"},"cancelled":{"type":"boolean"},"error":{"type":"string"},"failedNode":{"type":"string"},"cyclicNodes":{"type":"array","items":{"type":"string"}},"plannedCount":{"type":"integer","minimum":0},"executedCount":{"type":"integer","minimum":0},"cacheHitCount":{"type":"integer","minimum":0},"failedCount":{"type":"integer","minimum":0},"blockedCount":{"type":"integer","minimum":0},"effectsCommitted":{"type":"boolean"},"skippedEffectNodes":{"type":"array","items":{"type":"string"}}},"additionalProperties":false,"required":["graphId","runId","lastEventSeq","revision","succeeded","cancelled","error","failedNode","cyclicNodes","plannedCount","executedCount","cacheHitCount","failedCount","blockedCount","effectsCommitted","skippedEffectNodes"]})json";

constexpr const char kCancelResponseSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string"},"cancelledRunId":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["graphId","cancelledRunId"]})json";

constexpr const char kResultsResponseSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string"},"revision":{"type":"integer","minimum":0},"lastRunId":{"type":"integer","minimum":0},"lastEventSeq":{"type":"integer","minimum":0},"results":{"type":"array","items":{"type":"object","properties":{"nodeId":{"type":"string"},"status":{"type":"string","enum":["dirty","complete","failed","blocked","cancelled","skipped"]},"message":{"type":"string"},"durationMilliseconds":{"type":"number","minimum":0},"itemCount":{"type":"integer","minimum":0},"cacheHit":{"type":"boolean"},"evaluationCount":{"type":"integer","minimum":0},"runId":{"type":"integer","minimum":0},"previewAvailable":{"type":"boolean"},"outputs":{"type":"array","items":{"type":"object","properties":{"portId":{"type":"string"},"value":{"$ref":"#/$defs/value"}},"additionalProperties":false,"required":["portId","value"]}}},"additionalProperties":false,"required":["nodeId","status","message","durationMilliseconds","itemCount","cacheHit","evaluationCount","runId","previewAvailable","outputs"]}}},"additionalProperties":false,"required":["graphId","revision","lastRunId","lastEventSeq","results"],"$defs":{"leafValue":{"type":"object","properties":{"valueType":{"type":"string","enum":["absent","bool","integer","double","string","point3","polyline","polygon","mesh","archicadElementRef","list"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"}},"itemCount":{"type":"integer","minimum":0},"truncated":{"type":"boolean"}},"additionalProperties":false,"required":["valueType"]},"value":{"type":"object","properties":{"valueType":{"type":"string","enum":["absent","bool","integer","double","string","point3","polyline","polygon","mesh","archicadElementRef","list"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"}},"itemCount":{"type":"integer","minimum":0},"truncated":{"type":"boolean"},"items":{"type":"array","items":{"$ref":"#/$defs/leafValue"}}},"additionalProperties":false,"required":["valueType"]},"parameterValue":{"type":"object","properties":{"valueType":{"type":"string","enum":["bool","integer","double","string","point3","archicadElementRef"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"additionalProperties":false,"required":["valueType"]}}})json";

// The delta half of the synchronization contract. Paired with the lastEventSeq a
// snapshot carries, it needs no push channel - which is what lets a pytest
// script, a CLI and the editor consume one backend the same way.
constexpr const char kEventsInputSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1},"sinceSeq":{"type":"integer","minimum":0},"maxEvents":{"type":"integer","minimum":1,"maximum":4096}},"additionalProperties":false,"required":[]})json";

constexpr const char kEventsResponseSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string"},"lastSeq":{"type":"integer","minimum":0},"gap":{"type":"boolean"},"events":{"type":"array","items":{"type":"object","properties":{"seq":{"type":"integer","minimum":1},"kind":{"type":"string","enum":["runStarted","nodeQueued","nodeStarted","nodeCacheHit","nodeCompleted","nodeWarning","nodeFailed","nodeBlocked","nodeCancelled","runCompleted","runCancelled"]},"runId":{"type":"integer","minimum":0},"graphRevision":{"type":"integer","minimum":0},"nodeId":{"type":"string"},"timestampMs":{"type":"integer"},"message":{"type":"string"},"durationMilliseconds":{"type":"number","minimum":0},"itemCount":{"type":"integer","minimum":0},"plannedCount":{"type":"integer","minimum":0},"executedCount":{"type":"integer","minimum":0},"cacheHitCount":{"type":"integer","minimum":0},"failedCount":{"type":"integer","minimum":0},"blockedCount":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["seq","kind","runId","graphRevision","nodeId","timestampMs","message","durationMilliseconds","itemCount","plannedCount","executedCount","cacheHitCount","failedCount","blockedCount"]}}},"additionalProperties":false,"required":["graphId","lastSeq","gap","events"]})json";

constexpr const char kHistoryInputSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1},"maxRuns":{"type":"integer","minimum":1,"maximum":256}},"additionalProperties":false,"required":[]})json";

constexpr const char kHistoryResponseSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string"},"runs":{"type":"array","items":{"type":"object","properties":{"runId":{"type":"integer","minimum":0},"graphRevision":{"type":"integer","minimum":0},"startedAtMs":{"type":"integer"},"finishedAtMs":{"type":"integer"},"finished":{"type":"boolean"},"succeeded":{"type":"boolean"},"cancelled":{"type":"boolean"},"error":{"type":"string"},"failedNode":{"type":"string"},"plannedCount":{"type":"integer","minimum":0},"executedCount":{"type":"integer","minimum":0},"cacheHitCount":{"type":"integer","minimum":0},"failedCount":{"type":"integer","minimum":0},"blockedCount":{"type":"integer","minimum":0},"nodes":{"type":"array","items":{"type":"object","properties":{"nodeId":{"type":"string"},"status":{"type":"string","enum":["dirty","complete","failed","blocked","cancelled","skipped"]},"message":{"type":"string"},"durationMilliseconds":{"type":"number","minimum":0},"itemCount":{"type":"integer","minimum":0},"cacheHit":{"type":"boolean"}},"additionalProperties":false,"required":["nodeId","status","message","durationMilliseconds","itemCount","cacheHit"]}}},"additionalProperties":false,"required":["runId","graphRevision","startedAtMs","finishedAtMs","finished","succeeded","cancelled","error","failedNode","plannedCount","executedCount","cacheHitCount","failedCount","blockedCount","nodes"]}}},"additionalProperties":false,"required":["graphId","runs"]})json";

// Can this graph run right now, and if not, what exactly is wrong. Computed from
// the same single resolution pass as the compatibility report below.
constexpr const char kDependenciesResponseSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string"},"canEvaluate":{"type":"boolean"},"resolvedReferences":{"type":"integer","minimum":0},"unresolvedReferences":{"type":"integer","minimum":0},"nodesNeedingArchicad":{"type":"integer","minimum":0},"effectNodes":{"type":"integer","minimum":0},"findings":{"type":"array","items":{"type":"object","properties":{"severity":{"type":"string","enum":["warning","error"]},"nodeId":{"type":"string"},"kind":{"type":"string"},"detail":{"type":"string"}},"additionalProperties":false,"required":["severity","nodeId","kind","detail"]}}},"additionalProperties":false,"required":["graphId","canEvaluate","resolvedReferences","unresolvedReferences","nodesNeedingArchicad","effectNodes","findings"]})json";

// Can this graph LOAD - a different question from whether it can run. A graph
// with Archicad nodes is perfectly loadable with no project open.
constexpr const char kCompatibilityInputSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1},"formatVersion":{"type":"integer","minimum":0}},"additionalProperties":false,"required":[]})json";

constexpr const char kCompatibilityResponseSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string"},"status":{"type":"string","enum":["compatible","needsMigration","unsupportedFormat","missingNodeType","missingCapability"]},"runtimeFormatVersion":{"type":"integer","minimum":1},"missingNodeTypes":{"type":"array","items":{"type":"string"}},"findings":{"type":"array","items":{"type":"object","properties":{"severity":{"type":"string","enum":["warning","error"]},"nodeId":{"type":"string"},"kind":{"type":"string"},"detail":{"type":"string"}},"additionalProperties":false,"required":["severity","nodeId","kind","detail"]}}},"additionalProperties":false,"required":["graphId","status","runtimeFormatVersion","missingNodeTypes","findings"]})json";

std::string Utf8 (const GS::UniString& value)
{
    return value.ToCStr (0, GS::MaxUSize, CC_UTF8).Get ();
}

GS::UniString Text (const std::string& value)
{
    return GS::UniString (value.c_str (), CC_UTF8);
}

const char* ValueTypeName (graph::ValueType valueType)
{
    constexpr const char* names[] = { "absent", "bool",     "integer", "double", "string",
                                      "point3", "polyline", "polygon", "mesh",   "archicadElementRef",
                                      "list" };
    return names[static_cast<size_t> (valueType)];
}

const char* DomainName (graph::ExecutionDomain domain)
{
    switch (domain) {
        case graph::ExecutionDomain::Worker:
            return "worker";
        case graph::ExecutionDomain::ArchicadMainThread:
            return "archicadMainThread";
        case graph::ExecutionDomain::RenderThread:
            return "renderThread";
    }
    return "worker";
}

const char* StatusName (graph::NodeExecutionState state)
{
    switch (state) {
        case graph::NodeExecutionState::Dirty:
            return "dirty";
        case graph::NodeExecutionState::Complete:
            return "complete";
        case graph::NodeExecutionState::Failed:
            return "failed";
        case graph::NodeExecutionState::Blocked:
            return "blocked";
        case graph::NodeExecutionState::Cancelled:
            return "cancelled";
        case graph::NodeExecutionState::Skipped:
            return "skipped";
    }
    return "dirty";
}

void AddPoints (GS::ObjectState& state, const std::vector<graph::Point3>& points)
{
    GS::Array<double> numbers;
    for (const graph::Point3& point : points) {
        numbers.Push (point.x);
        numbers.Push (point.y);
        numbers.Push (point.z);
    }
    state.Add ("numbers", numbers);
    state.Add ("itemCount", static_cast<GS::Int64> (points.size ()));
}

// `expand` is false for list members, which is what keeps the encoding two
// levels deep and the response schemas non-recursive.
GS::ObjectState EncodeValue (const graph::Value& value, bool expand)
{
    GS::ObjectState state;
    state.Add ("valueType", ValueTypeName (value.Type ()));
    switch (value.Type ()) {
        case graph::ValueType::Absent:
            break;
        case graph::ValueType::Bool:
            state.Add ("bool", std::get<bool> (value.DataValue ()));
            break;
        case graph::ValueType::Integer:
            state.Add ("number", static_cast<double> (std::get<int64_t> (value.DataValue ())));
            break;
        case graph::ValueType::Double:
            state.Add ("number", std::get<double> (value.DataValue ()));
            break;
        case graph::ValueType::String:
            state.Add ("text", Text (std::get<std::string> (value.DataValue ())));
            break;
        case graph::ValueType::Point3: {
            const graph::Point3& point = std::get<graph::Point3> (value.DataValue ());
            GS::Array<double> numbers;
            numbers.Push (point.x);
            numbers.Push (point.y);
            numbers.Push (point.z);
            state.Add ("numbers", numbers);
            break;
        }
        case graph::ValueType::Polyline:
            AddPoints (state, std::get<graph::Polyline> (value.DataValue ()).points);
            break;
        case graph::ValueType::Polygon:
            AddPoints (state, std::get<graph::Polygon> (value.DataValue ()).points);
            break;
        case graph::ValueType::Mesh: {
            // Counts only. Mesh data reaches the preview hosts, not the bridge.
            const auto& mesh = std::get<graph::Value::ImmutableMesh> (value.DataValue ());
            state.Add ("itemCount", static_cast<GS::Int64> (mesh ? mesh->vertices.size () : 0));
            state.Add ("truncated", true);
            break;
        }
        case graph::ValueType::ArchicadElementRef:
            state.Add ("text", Text (std::get<graph::ArchicadElementRef> (value.DataValue ()).guid));
            break;
        case graph::ValueType::List: {
            const graph::Value::List& list = std::get<graph::Value::List> (value.DataValue ());
            state.Add ("itemCount", static_cast<GS::Int64> (list.size ()));
            if (!expand || list.size () > kMaxEncodedListItems) {
                state.Add ("truncated", true);
                break;
            }
            GS::Array<GS::ObjectState> items;
            for (const graph::Value& item : list)
                items.Push (EncodeValue (item, false));
            state.Add ("items", items);
            break;
        }
    }
    return state;
}

// Inbound. Returns false when the payload names a type it does not carry a
// value for, rather than silently substituting a default.
bool DecodeParameterValue (const GS::ObjectState& state, graph::Value& out, std::string& error)
{
    GS::UniString valueTypeName;
    if (!state.Get ("valueType", valueTypeName)) {
        error = "the value is missing valueType";
        return false;
    }
    const std::string valueType = Utf8 (valueTypeName);

    if (valueType == "bool") {
        bool flag = false;
        if (!state.Get ("bool", flag)) {
            error = "a bool value requires 'bool'";
            return false;
        }
        out = graph::Value (flag);
        return true;
    }
    if (valueType == "integer" || valueType == "double") {
        double number = 0.0;
        if (!state.Get ("number", number)) {
            error = "a numeric value requires 'number'";
            return false;
        }
        out = valueType == "integer" ? graph::Value (static_cast<int64_t> (number)) : graph::Value (number);
        return true;
    }
    if (valueType == "string" || valueType == "archicadElementRef") {
        GS::UniString text;
        if (!state.Get ("text", text)) {
            error = "a text value requires 'text'";
            return false;
        }
        if (valueType == "string")
            out = graph::Value (Utf8 (text));
        else
            out = graph::Value (graph::ArchicadElementRef { Utf8 (text) });
        return true;
    }
    if (valueType == "point3") {
        GS::Array<double> numbers;
        if (!state.Get ("numbers", numbers) || numbers.GetSize () != 3) {
            error = "a point3 value requires three numbers";
            return false;
        }
        out = graph::Value (graph::Point3 { numbers[0], numbers[1], numbers[2] });
        return true;
    }
    error = "a parameter cannot carry the value type '" + valueType + "'";
    return false;
}

graph::GraphId ReadGraphId (const GS::ObjectState& params)
{
    GS::UniString graphId;
    if (!params.Get ("graphId", graphId) || graphId.IsEmpty ())
        return graph::kDefaultGraphId;
    return Utf8 (graphId);
}

graph::Edge ReadEdge (const GS::ObjectState& params)
{
    GS::UniString sourceNode, sourcePort, targetNode, targetPort;
    params.Get ("sourceNode", sourceNode);
    params.Get ("sourcePort", sourcePort);
    params.Get ("targetNode", targetNode);
    params.Get ("targetPort", targetPort);
    return { Utf8 (sourceNode), Utf8 (sourcePort), Utf8 (targetNode), Utf8 (targetPort) };
}

// Containment rule 1: no graph operation propagates an exception into
// Archicad's call stack. Every verb below runs through this one wrapper rather
// than relying on each handler to remember a try/catch.
class GateFreeGraphCommand : public MainThreadCommand {
  public:
    bool NeedsMainThread () const override
    {
        return false;
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl& control) const final
    {
        try {
            return ExecuteGraph (params, control);
        }
        catch (const std::exception& exception) {
            return NativeCommandResult::Failure (GS::UniString ("the graph runtime raised an error: ", CC_UTF8) +
                                                 GS::UniString (exception.what (), CC_UTF8));
        }
        catch (...) {
            return NativeCommandResult::Failure (GS::UniString ("the graph runtime raised an unknown error", CC_UTF8));
        }
    }

  protected:
    virtual NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl& control) const = 0;
};

class GraphGetNodeTypesCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        GS::Array<GS::ObjectState> records;
        const graph::NodeRegistry registry = graph::GraphRuntimeState::Get ().Catalog ();
        for (const auto& [nodeTypeId, nodeType] : registry.Types ()) {
            GS::ObjectState record;
            record.Add ("nodeType", Text (nodeTypeId));
            record.Add ("label", Text (nodeType.label));
            record.Add ("category", Text (nodeType.category));
            record.Add ("description", Text (nodeType.description));
            record.Add ("executionDomain", DomainName (nodeType.executionDomain));
            record.Add ("effect", graph::EffectKindName (nodeType.effect));
            GS::Array<GS::UniString> generations;
            for (const graph::GenerationDomain domain : nodeType.generations.Domains ())
                generations.Push (GS::UniString (graph::GenerationDomainName (domain), CC_UTF8));
            record.Add ("generations", generations);
            GS::Array<GS::ObjectState> inputs, outputs, parameters;
            for (const graph::PortSchema& port : nodeType.inputs) {
                GS::ObjectState state;
                state.Add ("portId", Text (port.id));
                state.Add ("label", Text (port.label));
                state.Add ("valueType", ValueTypeName (port.valueType));
                state.Add ("required", port.required);
                state.Add ("acceptsMultiple", port.acceptsMultiple);
                inputs.Push (std::move (state));
            }
            for (const graph::PortSchema& port : nodeType.outputs) {
                GS::ObjectState state;
                state.Add ("portId", Text (port.id));
                state.Add ("label", Text (port.label));
                state.Add ("valueType", ValueTypeName (port.valueType));
                outputs.Push (std::move (state));
            }
            for (const graph::ParameterSchema& parameter : nodeType.parameters) {
                GS::ObjectState state;
                state.Add ("parameterId", Text (parameter.id));
                state.Add ("label", Text (parameter.label));
                state.Add ("valueType", ValueTypeName (parameter.valueType));
                state.Add ("required", parameter.required);
                if (parameter.defaultValue.has_value ())
                    state.Add ("defaultValue", EncodeValue (*parameter.defaultValue, false));
                parameters.Push (std::move (state));
            }
            record.Add ("inputs", inputs);
            record.Add ("outputs", outputs);
            record.Add ("parameters", parameters);
            records.Push (std::move (record));
        }
        GS::ObjectState response;
        response.Add ("nodeTypes", records);
        return response;
    }
};

class GraphGetStateCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        const graph::GraphId graphId = ReadGraphId (params);
        const graph::GraphDocument document = graph::GraphRuntimeState::Get ().Document (graphId);
        const graph::ResultsSnapshot snapshot = graph::GraphRuntimeState::Get ().Results (graphId);
        GS::Array<GS::ObjectState> nodes, edges;
        for (const auto& [nodeId, node] : document.Nodes ()) {
            GS::ObjectState record;
            record.Add ("nodeId", Text (nodeId));
            record.Add ("nodeType", Text (node.nodeType));
            GS::Array<GS::ObjectState> parameters;
            for (const auto& [parameterId, value] : node.parameters) {
                GS::ObjectState parameter;
                parameter.Add ("parameterId", Text (parameterId));
                parameter.Add ("value", EncodeValue (value, true));
                parameters.Push (std::move (parameter));
            }
            record.Add ("parameters", parameters);
            nodes.Push (std::move (record));
        }
        for (const graph::Edge& edge : document.Edges ()) {
            GS::ObjectState record;
            record.Add ("sourceNode", Text (edge.sourceNode));
            record.Add ("sourcePort", Text (edge.sourcePort));
            record.Add ("targetNode", Text (edge.targetNode));
            record.Add ("targetPort", Text (edge.targetPort));
            edges.Push (std::move (record));
        }
        GS::ObjectState response;
        response.Add ("graphId", Text (graphId));
        response.Add ("revision", static_cast<GS::Int64> (document.Revision ()));
        response.Add ("lastRunId", static_cast<GS::Int64> (snapshot.lastRunId));
        response.Add ("lastEventSeq", static_cast<GS::Int64> (snapshot.lastEventSeq));
        response.Add ("nodes", nodes);
        response.Add ("edges", edges);
        return response;
    }
};

class GraphApplyEditCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString editKindValue;
        params.Get ("editKind", editKindValue);
        const std::string editKind = Utf8 (editKindValue);
        graph::GraphEdit edit;
        std::string error;

        if (editKind == "addNode") {
            GS::UniString nodeId, nodeType;
            params.Get ("nodeId", nodeId);
            params.Get ("nodeType", nodeType);
            graph::Node node { Utf8 (nodeId), Utf8 (nodeType) };

            GS::Array<GS::ObjectState> parameters;
            if (params.Get ("parameters", parameters)) {
                for (const GS::ObjectState& parameter : parameters) {
                    GS::UniString parameterId;
                    GS::ObjectState valueState;
                    graph::Value value;
                    if (!parameter.Get ("parameterId", parameterId) || !parameter.Get ("value", valueState) ||
                        !DecodeParameterValue (valueState, value, error))
                        return NativeCommandResult::Failure (Text (error.empty () ? "invalid parameter" : error));
                    node.parameters.insert_or_assign (Utf8 (parameterId), std::move (value));
                }
            }
            double numberValue = 0.0;
            if (params.Get ("numberValue", numberValue))
                node.parameters.insert_or_assign ("value", graph::Value (numberValue));
            edit.data = graph::AddNodeEdit { std::move (node) };
        }
        else if (editKind == "removeNode") {
            GS::UniString nodeId;
            params.Get ("nodeId", nodeId);
            edit.data = graph::RemoveNodeEdit { Utf8 (nodeId) };
        }
        else if (editKind == "connect") {
            edit.data = graph::ConnectEdit { ReadEdge (params) };
        }
        else if (editKind == "disconnect") {
            edit.data = graph::DisconnectEdit { ReadEdge (params) };
        }
        else {
            GS::UniString nodeId, parameterId;
            params.Get ("nodeId", nodeId);
            params.Get ("parameterId", parameterId);
            graph::Value value;
            GS::ObjectState valueState;
            double numberValue = 0.0;
            if (params.Get ("value", valueState)) {
                if (!DecodeParameterValue (valueState, value, error))
                    return NativeCommandResult::Failure (Text (error));
            }
            else if (params.Get ("numberValue", numberValue)) {
                value = graph::Value (numberValue);
            }
            else {
                return NativeCommandResult::Failure (Text ("setParam requires a value"));
            }
            edit.data = graph::SetParameterEdit { Utf8 (nodeId), Utf8 (parameterId), std::move (value) };
        }

        const graph::EditResult result = graph::GraphRuntimeState::Get ().Apply (ReadGraphId (params), edit);
        if (!result.accepted)
            return NativeCommandResult::Failure (Text (result.error));
        GS::Array<GS::UniString> dirtyNodes;
        for (const graph::NodeId& nodeId : result.dirtyNodes)
            dirtyNodes.Push (Text (nodeId));
        GS::ObjectState response;
        response.Add ("revision", static_cast<GS::Int64> (result.revision));
        response.Add ("dirtyNodes", dirtyNodes);
        return response;
    }
};

class GraphEvaluateCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        graph::EvaluationRequest request;
        GS::Array<GS::UniString> targets;
        if (params.Get ("targets", targets)) {
            for (const GS::UniString& target : targets)
                request.targets.push_back (Utf8 (target));
        }
        GS::UniString mode;
        if (params.Get ("mode", mode) && Utf8 (mode) == "forced")
            request.mode = graph::EvaluationMode::Forced;
        // Absent means refused. A client has to ask for side effects in so many
        // words; nothing infers them from context.
        bool allowSideEffects = false;
        if (params.Get ("allowSideEffects", allowSideEffects))
            request.allowSideEffects = allowSideEffects;

        const graph::EvaluationSummary summary =
            graph::GraphRuntimeState::Get ().Evaluate (ReadGraphId (params), request);

        // A failed graph is a REPORTED outcome, not a failed command. The
        // difference matters: a client has to be able to render which node
        // failed and why, and a command failure carries only a string.
        GS::Array<GS::UniString> cyclicNodes;
        for (const graph::NodeId& nodeId : summary.cyclicNodes)
            cyclicNodes.Push (Text (nodeId));
        GS::ObjectState response;
        response.Add ("graphId", Text (summary.graphId));
        response.Add ("runId", static_cast<GS::Int64> (summary.runId));
        response.Add ("lastEventSeq", static_cast<GS::Int64> (summary.lastEventSeq));
        response.Add ("revision", static_cast<GS::Int64> (summary.revision));
        response.Add ("succeeded", summary.succeeded);
        response.Add ("cancelled", summary.cancelled);
        response.Add ("error", Text (summary.error));
        response.Add ("failedNode", Text (summary.failedNode));
        response.Add ("cyclicNodes", cyclicNodes);
        response.Add ("plannedCount", static_cast<GS::Int64> (summary.plannedCount));
        response.Add ("executedCount", static_cast<GS::Int64> (summary.executedCount));
        response.Add ("cacheHitCount", static_cast<GS::Int64> (summary.cacheHitCount));
        response.Add ("failedCount", static_cast<GS::Int64> (summary.failedCount));
        response.Add ("blockedCount", static_cast<GS::Int64> (summary.blockedCount));
        response.Add ("effectsCommitted", summary.effectsCommitted);
        GS::Array<GS::UniString> skipped;
        for (const graph::NodeId& nodeId : summary.skippedEffectNodes)
            skipped.Push (Text (nodeId));
        response.Add ("skippedEffectNodes", skipped);
        return response;
    }
};

class GraphCancelCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        const graph::GraphId graphId = ReadGraphId (params);
        GS::ObjectState response;
        response.Add ("graphId", Text (graphId));
        response.Add ("cancelledRunId", static_cast<GS::Int64> (graph::GraphRuntimeState::Get ().Cancel (graphId)));
        return response;
    }
};

class GraphGetNodeResultsCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        const graph::ResultsSnapshot snapshot = graph::GraphRuntimeState::Get ().Results (ReadGraphId (params));
        GS::Array<GS::ObjectState> results;
        for (const graph::RuntimeNodeResult& nodeResult : snapshot.nodes) {
            const graph::NodeStatus& status = nodeResult.status;
            const std::shared_ptr<const graph::NodeResult>& result = nodeResult.result;
            GS::ObjectState record;
            record.Add ("nodeId", Text (nodeResult.nodeId));
            record.Add ("status", StatusName (status.state));
            record.Add ("message", Text (status.message));
            record.Add ("durationMilliseconds", status.durationMilliseconds);
            record.Add ("itemCount", static_cast<GS::Int64> (status.itemCount));
            record.Add ("cacheHit", status.cacheHit);
            record.Add ("evaluationCount", static_cast<GS::Int64> (status.evaluationCount));
            record.Add ("runId", static_cast<GS::Int64> (status.runId));
            record.Add ("previewAvailable", false);
            GS::Array<GS::ObjectState> outputs;
            if (result) {
                for (const auto& [portId, value] : result->outputs) {
                    GS::ObjectState output;
                    output.Add ("portId", Text (portId));
                    output.Add ("value", EncodeValue (value, true));
                    outputs.Push (std::move (output));
                }
            }
            record.Add ("outputs", outputs);
            results.Push (std::move (record));
        }
        GS::ObjectState response;
        response.Add ("graphId", Text (snapshot.graphId));
        response.Add ("revision", static_cast<GS::Int64> (snapshot.revision));
        response.Add ("lastRunId", static_cast<GS::Int64> (snapshot.lastRunId));
        response.Add ("lastEventSeq", static_cast<GS::Int64> (snapshot.lastEventSeq));
        response.Add ("results", results);
        return response;
    }
};

// The delta half of the synchronization contract. A client pairs the
// lastEventSeq from a snapshot with this call and needs no push channel at all,
// which is what lets a pytest script, a CLI and the editor share one backend.
class GraphGetEventsCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        const graph::GraphId graphId = ReadGraphId (params);
        GS::Int64 sinceSeq = 0;
        GS::Int64 maxEvents = 512;
        params.Get ("sinceSeq", sinceSeq);
        params.Get ("maxEvents", maxEvents);

        const graph::RunEventLog::Tail tail = graph::GraphRuntimeState::Get ().Events (
            graphId, static_cast<graph::EventSeq> (sinceSeq < 0 ? 0 : sinceSeq),
            static_cast<size_t> (maxEvents < 1 ? 1 : maxEvents));

        GS::Array<GS::ObjectState> events;
        for (const graph::RunEvent& event : tail.events) {
            GS::ObjectState record;
            record.Add ("seq", static_cast<GS::Int64> (event.seq));
            record.Add ("kind", graph::RunEventKindName (event.kind));
            record.Add ("runId", static_cast<GS::Int64> (event.runId));
            record.Add ("graphRevision", static_cast<GS::Int64> (event.graphRevision));
            record.Add ("nodeId", Text (event.nodeId));
            record.Add ("timestampMs", static_cast<GS::Int64> (event.timestampMs));
            record.Add ("message", Text (event.message));
            record.Add ("durationMilliseconds", event.durationMilliseconds);
            record.Add ("itemCount", static_cast<GS::Int64> (event.itemCount));
            record.Add ("plannedCount", static_cast<GS::Int64> (event.plannedCount));
            record.Add ("executedCount", static_cast<GS::Int64> (event.executedCount));
            record.Add ("cacheHitCount", static_cast<GS::Int64> (event.cacheHitCount));
            record.Add ("failedCount", static_cast<GS::Int64> (event.failedCount));
            record.Add ("blockedCount", static_cast<GS::Int64> (event.blockedCount));
            events.Push (std::move (record));
        }

        GS::ObjectState response;
        response.Add ("graphId", Text (graphId));
        response.Add ("lastSeq", static_cast<GS::Int64> (tail.lastSeq));
        // Reported, never hidden: a client whose sequence fell off the ring must
        // re-snapshot rather than stitch an incomplete tail onto stale state.
        response.Add ("gap", tail.gap);
        response.Add ("events", events);
        return response;
    }
};

class GraphGetRunHistoryCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        const graph::GraphId graphId = ReadGraphId (params);
        GS::Int64 maxRuns = 16;
        params.Get ("maxRuns", maxRuns);

        GS::Array<GS::ObjectState> runs;
        for (const graph::RunRecord& record :
             graph::GraphRuntimeState::Get ().RecentRuns (graphId, static_cast<size_t> (maxRuns < 1 ? 1 : maxRuns))) {
            GS::ObjectState run;
            run.Add ("runId", static_cast<GS::Int64> (record.runId));
            run.Add ("graphRevision", static_cast<GS::Int64> (record.graphRevision));
            run.Add ("startedAtMs", static_cast<GS::Int64> (record.startedAtMs));
            run.Add ("finishedAtMs", static_cast<GS::Int64> (record.finishedAtMs));
            run.Add ("finished", record.finished);
            run.Add ("succeeded", record.succeeded);
            run.Add ("cancelled", record.cancelled);
            run.Add ("error", Text (record.error));
            run.Add ("failedNode", Text (record.failedNode));
            run.Add ("plannedCount", static_cast<GS::Int64> (record.plannedCount));
            run.Add ("executedCount", static_cast<GS::Int64> (record.executedCount));
            run.Add ("cacheHitCount", static_cast<GS::Int64> (record.cacheHitCount));
            run.Add ("failedCount", static_cast<GS::Int64> (record.failedCount));
            run.Add ("blockedCount", static_cast<GS::Int64> (record.blockedCount));
            GS::Array<GS::ObjectState> nodes;
            for (const graph::NodeRunRecord& node : record.nodes) {
                GS::ObjectState entry;
                entry.Add ("nodeId", Text (node.nodeId));
                entry.Add ("status", StatusName (node.finalState));
                entry.Add ("message", Text (node.message));
                entry.Add ("durationMilliseconds", node.durationMilliseconds);
                entry.Add ("itemCount", static_cast<GS::Int64> (node.itemCount));
                entry.Add ("cacheHit", node.cacheHit);
                nodes.Push (std::move (entry));
            }
            run.Add ("nodes", nodes);
            runs.Push (std::move (run));
        }

        GS::ObjectState response;
        response.Add ("graphId", Text (graphId));
        response.Add ("runs", runs);
        return response;
    }
};

GS::Array<GS::ObjectState> FindingStates (const std::vector<graph::GraphFinding>& findings)
{
    GS::Array<GS::ObjectState> states;
    for (const graph::GraphFinding& finding : findings) {
        GS::ObjectState state;
        state.Add ("severity", graph::FindingSeverityName (finding.severity));
        state.Add ("nodeId", Text (finding.nodeId));
        state.Add ("kind", Text (finding.kind));
        state.Add ("detail", Text (finding.detail));
        states.Push (std::move (state));
    }
    return states;
}

// "Can this graph run right now." Answering before evaluating is the difference
// between a list a user can act on and a GUID in an error message halfway
// through a run.
class GraphGetDependenciesCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        const graph::GraphId graphId = ReadGraphId (params);
        const graph::GraphDependencyReport report = graph::GraphRuntimeState::Get ().Dependencies (graphId);
        GS::ObjectState response;
        response.Add ("graphId", Text (graphId));
        response.Add ("canEvaluate", report.canEvaluate);
        response.Add ("resolvedReferences", static_cast<GS::Int64> (report.resolvedReferences));
        response.Add ("unresolvedReferences", static_cast<GS::Int64> (report.unresolvedReferences));
        response.Add ("nodesNeedingArchicad", static_cast<GS::Int64> (report.nodesNeedingArchicad));
        response.Add ("effectNodes", static_cast<GS::Int64> (report.effectNodes));
        response.Add ("findings", FindingStates (report.findings));
        return response;
    }
};

// "Can this graph load." Deliberately a different answer: a graph full of
// Archicad nodes is perfectly loadable with no project open, and refusing to
// open a file for editing because it cannot run would be wrong.
class GraphGetCompatibilityCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        const graph::GraphId graphId = ReadGraphId (params);
        GS::Int64 formatVersion = 0;
        params.Get ("formatVersion", formatVersion);

        const graph::CompatibilityReport report = graph::GraphRuntimeState::Get ().Compatibility (
            graphId, static_cast<uint32_t> (formatVersion < 0 ? 0 : formatVersion));

        GS::Array<GS::UniString> missing;
        for (const std::string& nodeType : report.missingNodeTypes)
            missing.Push (Text (nodeType));

        GS::ObjectState response;
        response.Add ("graphId", Text (graphId));
        response.Add ("status", graph::CompatibilityStatusName (report.status));
        response.Add ("runtimeFormatVersion", static_cast<GS::Int64> (graph::kGraphFormatVersion));
        response.Add ("missingNodeTypes", missing);
        response.Add ("findings", FindingStates (report.findings));
        return response;
    }
};

const NativeCommandRegistration registrations[] = {
    { "GraphGetNodeTypes", &MakeRegisteredNativeCommand<GraphGetNodeTypesCommand>, false, kGraphInputSchema,
      kCatalogResponseSchema },
    { "GraphGetState", &MakeRegisteredNativeCommand<GraphGetStateCommand>, false, kGraphInputSchema,
      kStateResponseSchema },
    { "GraphApplyEdit", &MakeRegisteredNativeCommand<GraphApplyEditCommand>, false, kEditInputSchema,
      kEditResponseSchema },
    { "GraphEvaluate", &MakeRegisteredNativeCommand<GraphEvaluateCommand>, false, kEvaluateInputSchema,
      kEvaluateResponseSchema },
    { "GraphCancel", &MakeRegisteredNativeCommand<GraphCancelCommand>, false, kGraphInputSchema,
      kCancelResponseSchema },
    { "GraphGetNodeResults", &MakeRegisteredNativeCommand<GraphGetNodeResultsCommand>, false, kGraphInputSchema,
      kResultsResponseSchema },
    { "GraphGetEvents", &MakeRegisteredNativeCommand<GraphGetEventsCommand>, false, kEventsInputSchema,
      kEventsResponseSchema },
    { "GraphGetRunHistory", &MakeRegisteredNativeCommand<GraphGetRunHistoryCommand>, false, kHistoryInputSchema,
      kHistoryResponseSchema },
    { "GraphGetDependencies", &MakeRegisteredNativeCommand<GraphGetDependenciesCommand>, false, kGraphInputSchema,
      kDependenciesResponseSchema },
    { "GraphGetCompatibility", &MakeRegisteredNativeCommand<GraphGetCompatibilityCommand>, false,
      kCompatibilityInputSchema, kCompatibilityResponseSchema },
};

} // namespace

NativeCommandRegistrations GetNodeGraphCommandRegistrations ()
{
    return MakeRegistrationView (registrations);
}

} // namespace geomsrv
