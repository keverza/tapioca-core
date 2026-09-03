#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/NodeGraphCommands.hpp"

#include "NativeCommands/NodeGraphCommandSupport.hpp"
#include "NativeCommands/NodeGraphValueEncoding.hpp"
#include "NodeGraph/ElementClassification.hpp"

#include "NodeGraph/GraphReports.hpp"
#include "NodeGraph/GraphRuntimeState.hpp"
#include "NodeGraph/NodeLifting.hpp"
#include "NodeGraph/ValueText.hpp"

#include <string>

namespace geomsrv {
namespace {

// ---------------------------------------------------------------------------
// The bridge's two asymmetric halves
//
// The OUTBOUND encoding moved to NodeGraphValueEncoding once a second verb
// needed it; read the contract there. What stays here is the INBOUND half and
// the schemas, and the asymmetry between them is deliberate:
//
//  * INBOUND (parameters) accepts scalars only. A graph is authored from small
//    typed values; nothing needs to push a mesh in over JSON.
//  * OUTBOUND summarizes, and is bounded by the runtime rather than by the
//    client. Model geometry does not cross this bridge - it reaches the preview
//    hosts through RetainedPreviewStore.
//
// The outbound encoding is two levels deep by construction, which is why the
// schemas below need no recursive $ref.
// ---------------------------------------------------------------------------

constexpr const char kGraphInputSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1}},"additionalProperties":false,"required":[]})json";

constexpr const char kCatalogResponseSchema[] =
    R"json({"type":"object","properties":{"nodeTypes":{"type":"array","items":{"type":"object","properties":{"nodeType":{"type":"string"},"label":{"type":"string"},"category":{"type":"string"},"description":{"type":"string"},"executionDomain":{"type":"string","enum":["worker","archicadMainThread","renderThread"]},"effect":{"type":"string","enum":["pure","readModel","hostUiWrite"]},"display":{"type":"string","enum":["ports","text","preview","selectionSet","script"]},"bypassMappings":{"type":"array","items":{"type":"object","properties":{"inputId":{"type":"string"},"outputId":{"type":"string"}},"additionalProperties":false,"required":["inputId","outputId"]}},"holdCapable":{"type":"boolean"},"instancePorts":{"type":"boolean"},"generations":{"type":"array","items":{"type":"string","enum":["project","selection"]}},"inputs":{"type":"array","items":{"type":"object","properties":{"portId":{"type":"string"},"label":{"type":"string"},"valueType":{"type":"string"},"required":{"type":"boolean"},"acceptsMultiple":{"type":"boolean"}},"additionalProperties":false,"required":["portId","label","valueType","required","acceptsMultiple"]}},"outputs":{"type":"array","items":{"type":"object","properties":{"portId":{"type":"string"},"label":{"type":"string"},"valueType":{"type":"string"}},"additionalProperties":false,"required":["portId","label","valueType"]}},"parameters":{"type":"array","items":{"type":"object","properties":{"parameterId":{"type":"string"},"label":{"type":"string"},"valueType":{"type":"string"},"required":{"type":"boolean"},"defaultValue":{"$ref":"#/$defs/value"},"ui":{"type":"object","properties":{"widget":{"type":"string","enum":["auto","number","slider","boolean","select","text","vector","point","color","readOnly","previewTarget","libraryPart"]},"section":{"type":"string"},"order":{"type":"integer"},"help":{"type":"string"},"unit":{"type":"string"},"minimum":{"type":"number"},"maximum":{"type":"number"},"step":{"type":"number","exclusiveMinimum":0},"decimals":{"type":"integer","minimum":0,"maximum":15},"minimumParameter":{"type":"string"},"maximumParameter":{"type":"string"},"stepParameter":{"type":"string"},"decimalsParameter":{"type":"string"},"components":{"type":"array","items":{"type":"string"}},"options":{"type":"array","items":{"type":"object","properties":{"label":{"type":"string"},"value":{"$ref":"#/$defs/value"}},"additionalProperties":false,"required":["label","value"]}},"optionSource":{"type":"string","enum":["none","layer","pen","fill","lineType","surface","buildingMaterial","composite","profile"]}},"additionalProperties":false,"required":["widget","section","order","help","unit","components","options","optionSource"]}},"additionalProperties":false,"required":["parameterId","label","valueType","required"]}}},"additionalProperties":false,"required":["nodeType","label","category","description","executionDomain","effect","display","bypassMappings","holdCapable","instancePorts","generations","inputs","outputs","parameters"]}},"elementTypes":{"type":"array","items":{"type":"object","properties":{"id":{"type":"string"},"label":{"type":"string"},"plural":{"type":"string"},"container":{"type":"boolean"}},"additionalProperties":false,"required":["id","label","plural","container"]}}},"additionalProperties":false,"required":["nodeTypes","elementTypes"],"$defs":{"leafValue":{"type":"object","properties":{"valueType":{"type":"string","enum":["absent","bool","integer","double","string","point3","polyline","polygon","mesh","archicadElementRef","list"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"}},"itemCount":{"type":"integer","minimum":0},"truncated":{"type":"boolean"}},"additionalProperties":false,"required":["valueType"]},"value":{"type":"object","properties":{"valueType":{"type":"string","enum":["absent","bool","integer","double","string","point3","polyline","polygon","mesh","archicadElementRef","list"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"}},"indices":{"type":"array","items":{"type":"integer","minimum":0}},"itemCount":{"type":"integer","minimum":0},"truncated":{"type":"boolean"},"items":{"type":"array","items":{"$ref":"#/$defs/leafValue"}}},"additionalProperties":false,"required":["valueType"]},"parameterValue":{"type":"object","properties":{"valueType":{"type":"string","enum":["bool","integer","double","string","point3","archicadElementRef"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"additionalProperties":false,"required":["valueType"]}}})json";

constexpr const char kStateResponseSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string"},"revision":{"type":"integer","minimum":0},"lastRunId":{"type":"integer","minimum":0},"lastEventSeq":{"type":"integer","minimum":0},"nodes":{"type":"array","items":{"type":"object","properties":{"nodeId":{"type":"string"},"nodeType":{"type":"string"},"executionMode":{"type":"string","enum":["enabled","disabled","bypassed","holding"]},"inputModifiers":{"type":"array","items":{"type":"object","properties":{"portId":{"type":"string"},"modifier":{"type":"string","enum":["none","flatten","graft","simplify","reverse","round","normalise"]}},"additionalProperties":false,"required":["portId","modifier"]}},"parameters":{"type":"array","items":{"type":"object","properties":{"parameterId":{"type":"string"},"value":{"$ref":"#/$defs/value"}},"additionalProperties":false,"required":["parameterId","value"]}},"inputs":{"type":"array","items":{"type":"object","properties":{"portId":{"type":"string"},"label":{"type":"string"},"valueType":{"type":"string"},"required":{"type":"boolean"},"acceptsMultiple":{"type":"boolean"}},"additionalProperties":false,"required":["portId","label","valueType","required","acceptsMultiple"]}},"outputs":{"type":"array","items":{"type":"object","properties":{"portId":{"type":"string"},"label":{"type":"string"},"valueType":{"type":"string"}},"additionalProperties":false,"required":["portId","label","valueType"]}}},"additionalProperties":false,"required":["nodeId","nodeType","executionMode","parameters"]}},"edges":{"type":"array","items":{"type":"object","properties":{"sourceNode":{"type":"string"},"sourcePort":{"type":"string"},"targetNode":{"type":"string"},"targetPort":{"type":"string"}},"additionalProperties":false,"required":["sourceNode","sourcePort","targetNode","targetPort"]}}},"additionalProperties":false,"required":["graphId","revision","lastRunId","lastEventSeq","nodes","edges"],"$defs":{"leafValue":{"type":"object","properties":{"valueType":{"type":"string","enum":["absent","bool","integer","double","string","point3","polyline","polygon","mesh","archicadElementRef","list"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"}},"itemCount":{"type":"integer","minimum":0},"truncated":{"type":"boolean"}},"additionalProperties":false,"required":["valueType"]},"value":{"type":"object","properties":{"valueType":{"type":"string","enum":["absent","bool","integer","double","string","point3","polyline","polygon","mesh","archicadElementRef","list"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"}},"indices":{"type":"array","items":{"type":"integer","minimum":0}},"itemCount":{"type":"integer","minimum":0},"truncated":{"type":"boolean"},"items":{"type":"array","items":{"$ref":"#/$defs/leafValue"}}},"additionalProperties":false,"required":["valueType"]},"parameterValue":{"type":"object","properties":{"valueType":{"type":"string","enum":["bool","integer","double","string","point3","archicadElementRef"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"additionalProperties":false,"required":["valueType"]}}})json";

// `numberValue` is retained as a deprecated alias so the existing editor build
// keeps working across this change. New clients send `value`.
constexpr const char kEditInputSchema[] =
    R"json({"oneOf":[{"type":"object","properties":{"graphId":{"type":"string","minLength":1},"editKind":{"const":"addNode"},"nodeId":{"type":"string","minLength":1},"nodeType":{"type":"string","minLength":1},"parameters":{"type":"array","items":{"type":"object","properties":{"parameterId":{"type":"string","minLength":1},"value":{"$ref":"#/$defs/parameterValue"}},"additionalProperties":false,"required":["parameterId","value"]}},"numberValue":{"type":"number"}},"additionalProperties":false,"required":["editKind","nodeId","nodeType"]},{"type":"object","properties":{"graphId":{"type":"string","minLength":1},"editKind":{"const":"removeNode"},"nodeId":{"type":"string","minLength":1}},"additionalProperties":false,"required":["editKind","nodeId"]},{"type":"object","properties":{"graphId":{"type":"string","minLength":1},"editKind":{"enum":["connect","disconnect"]},"sourceNode":{"type":"string","minLength":1},"sourcePort":{"type":"string","minLength":1},"targetNode":{"type":"string","minLength":1},"targetPort":{"type":"string","minLength":1}},"additionalProperties":false,"required":["editKind","sourceNode","sourcePort","targetNode","targetPort"]},{"type":"object","properties":{"graphId":{"type":"string","minLength":1},"editKind":{"const":"setParam"},"nodeId":{"type":"string","minLength":1},"parameterId":{"type":"string","minLength":1},"value":{"$ref":"#/$defs/parameterValue"},"numberValue":{"type":"number"}},"additionalProperties":false,"required":["editKind","nodeId","parameterId"]},{"type":"object","properties":{"graphId":{"type":"string","minLength":1},"editKind":{"const":"setExecutionMode"},"nodeId":{"type":"string","minLength":1},"mode":{"type":"string","enum":["enabled","disabled","bypassed","holding"]}},"additionalProperties":false,"required":["editKind","nodeId","mode"]},{"type":"object","properties":{"graphId":{"type":"string","minLength":1},"editKind":{"const":"setPortModifier"},"nodeId":{"type":"string","minLength":1},"portId":{"type":"string","minLength":1},"modifier":{"type":"string","enum":["none","flatten","graft","simplify","reverse","round","normalise"]}},"additionalProperties":false,"required":["editKind","nodeId","portId","modifier"]},{"type":"object","properties":{"graphId":{"type":"string","minLength":1},"editKind":{"const":"releaseHolding"},"nodeId":{"type":"string","minLength":1}},"additionalProperties":false,"required":["editKind","nodeId"]}],"$defs":{"leafValue":{"type":"object","properties":{"valueType":{"type":"string","enum":["absent","bool","integer","double","string","point3","polyline","polygon","mesh","archicadElementRef","list"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"}},"itemCount":{"type":"integer","minimum":0},"truncated":{"type":"boolean"}},"additionalProperties":false,"required":["valueType"]},"value":{"type":"object","properties":{"valueType":{"type":"string","enum":["absent","bool","integer","double","string","point3","polyline","polygon","mesh","archicadElementRef","list"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"}},"indices":{"type":"array","items":{"type":"integer","minimum":0}},"itemCount":{"type":"integer","minimum":0},"truncated":{"type":"boolean"},"items":{"type":"array","items":{"$ref":"#/$defs/leafValue"}}},"additionalProperties":false,"required":["valueType"]},"parameterValue":{"type":"object","properties":{"valueType":{"type":"string","enum":["bool","integer","double","string","point3","archicadElementRef"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"additionalProperties":false,"required":["valueType"]}}})json";

constexpr const char kEditResponseSchema[] =
    R"json({"type":"object","properties":{"revision":{"type":"integer","minimum":1},"dirtyNodes":{"type":"array","items":{"type":"string"}}},"additionalProperties":false,"required":["revision","dirtyNodes"]})json";

constexpr const char kEraseElementsInputSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1},"nodeIds":{"type":"array","items":{"type":"string","minLength":1}},"edges":{"type":"array","items":{"type":"object","properties":{"sourceNode":{"type":"string","minLength":1},"sourcePort":{"type":"string","minLength":1},"targetNode":{"type":"string","minLength":1},"targetPort":{"type":"string","minLength":1}},"additionalProperties":false,"required":["sourceNode","sourcePort","targetNode","targetPort"]}}},"additionalProperties":false,"required":["nodeIds","edges"]})json";

// allowSideEffects defaults to FALSE and must be sent deliberately. A preview,
// a watch and an auto-evaluated branch all leave it out, which is what keeps a
// graph from changing the user's selection while they are editing.
constexpr const char kEvaluateInputSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1},"targets":{"type":"array","items":{"type":"string","minLength":1}},"mode":{"type":"string","enum":["incremental","forced"]},"allowSideEffects":{"type":"boolean"},"maxParallel":{"type":"integer","minimum":0,"maximum":64}},"additionalProperties":false,"required":[]})json";

constexpr const char kEvaluateResponseSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string"},"runId":{"type":"integer","minimum":0},"lastEventSeq":{"type":"integer","minimum":0},"revision":{"type":"integer","minimum":0},"succeeded":{"type":"boolean"},"cancelled":{"type":"boolean"},"error":{"type":"string"},"failedNode":{"type":"string"},"cyclicNodes":{"type":"array","items":{"type":"string"}},"plannedCount":{"type":"integer","minimum":0},"executedCount":{"type":"integer","minimum":0},"cacheHitCount":{"type":"integer","minimum":0},"failedCount":{"type":"integer","minimum":0},"blockedCount":{"type":"integer","minimum":0},"effectsCommitted":{"type":"boolean"},"skippedEffectNodes":{"type":"array","items":{"type":"string"}},"parallelism":{"type":"object","properties":{"workerThreads":{"type":"integer","minimum":0},"maxParallel":{"type":"integer","minimum":0},"peakConcurrency":{"type":"integer","minimum":0},"wallClockMs":{"type":"number"},"workMs":{"type":"number"},"speedup":{"type":"number"},"levels":{"type":"array","items":{"type":"object","properties":{"levelIndex":{"type":"integer","minimum":0},"executedCount":{"type":"integer","minimum":0},"workerNodeCount":{"type":"integer","minimum":0},"hostNodeCount":{"type":"integer","minimum":0},"peakConcurrency":{"type":"integer","minimum":0},"wallClockMs":{"type":"number"},"workMs":{"type":"number"}},"additionalProperties":false,"required":["levelIndex","executedCount","workerNodeCount","hostNodeCount","peakConcurrency","wallClockMs","workMs"]}}},"additionalProperties":false,"required":["workerThreads","maxParallel","peakConcurrency","wallClockMs","workMs","speedup","levels"]}},"additionalProperties":false,"required":["graphId","runId","lastEventSeq","revision","succeeded","cancelled","error","failedNode","cyclicNodes","plannedCount","executedCount","cacheHitCount","failedCount","blockedCount","effectsCommitted","skippedEffectNodes","parallelism"]})json";

constexpr const char kCancelResponseSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string"},"cancelledRunId":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["graphId","cancelledRunId"]})json";

constexpr const char kResultsResponseSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string"},"revision":{"type":"integer","minimum":0},"lastRunId":{"type":"integer","minimum":0},"lastEventSeq":{"type":"integer","minimum":0},"results":{"type":"array","items":{"type":"object","properties":{"nodeId":{"type":"string"},"status":{"type":"string","enum":["pending","running","success","error","blocked","disabled","bypassed","holding","cancelled"]},"code":{"type":"string"},"message":{"type":"string"},"durationMilliseconds":{"type":"number","minimum":0},"itemCount":{"type":"integer","minimum":0},"cacheHit":{"type":"boolean"},"evaluationCount":{"type":"integer","minimum":0},"runId":{"type":"integer","minimum":0},"previewAvailable":{"type":"boolean"},"outputs":{"type":"array","items":{"type":"object","properties":{"portId":{"type":"string"},"value":{"$ref":"#/$defs/value"},"text":{"type":"string"},"summary":{"type":"string"},"itemType":{"type":"string"},"branchCount":{"type":"integer","minimum":0},"branchesTruncated":{"type":"boolean"},"branches":{"type":"array","items":{"type":"object","properties":{"path":{"type":"string"},"segments":{"type":"array","items":{"type":"integer","minimum":0}},"itemCount":{"type":"integer","minimum":0},"truncated":{"type":"boolean"},"value":{"$ref":"#/$defs/value"}},"additionalProperties":false,"required":["path","segments","itemCount","truncated","value"]}}},"additionalProperties":false,"required":["portId","value","text","summary","itemType","branchCount","branchesTruncated","branches"]}}},"additionalProperties":false,"required":["nodeId","status","code","message","durationMilliseconds","itemCount","cacheHit","evaluationCount","runId","previewAvailable","outputs"]}}},"additionalProperties":false,"required":["graphId","revision","lastRunId","lastEventSeq","results"],"$defs":{"leafValue":{"type":"object","properties":{"valueType":{"type":"string","enum":["absent","bool","integer","double","string","point3","polyline","polygon","mesh","archicadElementRef","list"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"}},"itemCount":{"type":"integer","minimum":0},"truncated":{"type":"boolean"}},"additionalProperties":false,"required":["valueType"]},"value":{"type":"object","properties":{"valueType":{"type":"string","enum":["absent","bool","integer","double","string","point3","polyline","polygon","mesh","archicadElementRef","list"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"}},"indices":{"type":"array","items":{"type":"integer","minimum":0}},"itemCount":{"type":"integer","minimum":0},"truncated":{"type":"boolean"},"items":{"type":"array","items":{"$ref":"#/$defs/leafValue"}}},"additionalProperties":false,"required":["valueType"]},"parameterValue":{"type":"object","properties":{"valueType":{"type":"string","enum":["bool","integer","double","string","point3","archicadElementRef"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"additionalProperties":false,"required":["valueType"]}}})json";

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
    R"json({"type":"object","properties":{"graphId":{"type":"string"},"runs":{"type":"array","items":{"type":"object","properties":{"runId":{"type":"integer","minimum":0},"graphRevision":{"type":"integer","minimum":0},"startedAtMs":{"type":"integer"},"finishedAtMs":{"type":"integer"},"finished":{"type":"boolean"},"succeeded":{"type":"boolean"},"cancelled":{"type":"boolean"},"error":{"type":"string"},"failedNode":{"type":"string"},"plannedCount":{"type":"integer","minimum":0},"executedCount":{"type":"integer","minimum":0},"cacheHitCount":{"type":"integer","minimum":0},"failedCount":{"type":"integer","minimum":0},"blockedCount":{"type":"integer","minimum":0},"nodes":{"type":"array","items":{"type":"object","properties":{"nodeId":{"type":"string"},"status":{"type":"string","enum":["pending","running","success","error","blocked","disabled","bypassed","holding","cancelled"]},"message":{"type":"string"},"durationMilliseconds":{"type":"number","minimum":0},"itemCount":{"type":"integer","minimum":0},"cacheHit":{"type":"boolean"}},"additionalProperties":false,"required":["nodeId","status","message","durationMilliseconds","itemCount","cacheHit"]}}},"additionalProperties":false,"required":["runId","graphRevision","startedAtMs","finishedAtMs","finished","succeeded","cancelled","error","failedNode","plannedCount","executedCount","cacheHitCount","failedCount","blockedCount","nodes"]}}},"additionalProperties":false,"required":["graphId","runs"]})json";

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

// Stage F1: ONE projection, and it is the runtime's own. This used to be a
// second switch spelling the same states differently for the browser, which is
// how `dirty` and `pending` came to be the same thing with two names.
const char* StatusName (graph::NodeExecutionState state)
{
    return graph::NodeExecutionStateName (state);
}

// UI-1. The parameter's edit descriptor, as the catalog projects it.
//
// The optional numeric bounds are OMITTED when absent rather than sent as a
// sentinel: zero is a legal minimum, and a client cannot tell a real zero from a
// stand-in. GS::ObjectState cannot represent JSON null, so absence is the only
// honest encoding available - which is the same rule the rest of this bridge
// follows for defaultValue.
GS::ObjectState EncodeParameterUi (const graph::ParameterUi& ui)
{
    GS::ObjectState state;
    state.Add ("widget", graph::ParameterWidgetName (ui.widget));
    state.Add ("section", GraphText (ui.section));
    state.Add ("order", static_cast<GS::Int64> (ui.order));
    state.Add ("help", GraphText (ui.help));
    state.Add ("unit", GraphText (ui.unit));
    if (ui.minimum.has_value ())
        state.Add ("minimum", *ui.minimum);
    if (ui.maximum.has_value ())
        state.Add ("maximum", *ui.maximum);
    if (ui.step.has_value ())
        state.Add ("step", *ui.step);
    if (ui.decimals.has_value ())
        state.Add ("decimals", static_cast<GS::Int64> (*ui.decimals));
    if (!ui.minimumParameter.empty ())
        state.Add ("minimumParameter", GraphText (ui.minimumParameter));
    if (!ui.maximumParameter.empty ())
        state.Add ("maximumParameter", GraphText (ui.maximumParameter));
    if (!ui.stepParameter.empty ())
        state.Add ("stepParameter", GraphText (ui.stepParameter));
    if (!ui.decimalsParameter.empty ())
        state.Add ("decimalsParameter", GraphText (ui.decimalsParameter));
    GS::Array<GS::UniString> components;
    for (const std::string& component : ui.components)
        components.Push (GraphText (component));
    state.Add ("components", components);
    GS::Array<GS::ObjectState> options;
    for (const graph::ParameterOption& option : ui.options) {
        GS::ObjectState encoded;
        encoded.Add ("label", GraphText (option.label));
        encoded.Add ("value", EncodeValue (option.value, false));
        options.Push (std::move (encoded));
    }
    state.Add ("options", options);
    state.Add ("optionSource", graph::ParameterOptionSourceName (ui.optionSource));
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
    const std::string valueType = GraphUtf8 (valueTypeName);

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
            out = graph::Value (GraphUtf8 (text));
        else
            out = graph::Value (graph::ArchicadElementRef { GraphUtf8 (text) });
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

graph::Edge ReadEdge (const GS::ObjectState& params)
{
    GS::UniString sourceNode, sourcePort, targetNode, targetPort;
    params.Get ("sourceNode", sourceNode);
    params.Get ("sourcePort", sourcePort);
    params.Get ("targetNode", targetNode);
    params.Get ("targetPort", targetPort);
    return { GraphUtf8 (sourceNode), GraphUtf8 (sourcePort), GraphUtf8 (targetNode), GraphUtf8 (targetPort) };
}

class GraphGetNodeTypesCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        GS::Array<GS::ObjectState> records;
        const graph::NodeRegistry registry = graph::GraphRuntimeState::Get ().Catalog ();
        for (const auto& [nodeTypeId, nodeType] : registry.Types ()) {
            GS::ObjectState record;
            record.Add ("nodeType", GraphText (nodeTypeId));
            record.Add ("label", GraphText (nodeType.label));
            record.Add ("category", GraphText (nodeType.category));
            record.Add ("description", GraphText (nodeType.description));
            record.Add ("executionDomain", DomainName (nodeType.executionDomain));
            record.Add ("effect", graph::EffectKindName (nodeType.effect));
            record.Add ("display", graph::NodeDisplayName (nodeType.display));
            // Stage F3/F4. The editor greys "Bypass" and "Hold" per node type
            // from THIS, rather than attempting the edit to find out: an action
            // that is offered and then refused is worse than one that was never
            // offered. The mappings themselves ship too, so a client can say
            // WHICH port a bypass would forward.
            GS::Array<GS::ObjectState> bypassMappings;
            for (const graph::BypassMapping& mapping : nodeType.bypassMappings) {
                GS::ObjectState state;
                state.Add ("inputId", GraphText (mapping.inputId));
                state.Add ("outputId", GraphText (mapping.outputId));
                bypassMappings.Push (std::move (state));
            }
            record.Add ("bypassMappings", bypassMappings);
            record.Add ("holdCapable", nodeType.holdCapable);
            // â ï¸ A TYPE THAT SETS THIS SHIPS EMPTY `inputs` AND `outputs`, AND
            // THAT IS NOT A CATALOG BUG. Its ports are authored per node - in a
            // script file the user edits outside Archicad - so the catalog has
            // nothing to say about them and the STATE projection carries them
            // instead, per node. A client that drew ports from the catalog alone
            // would render every script node as portless.
            record.Add ("instancePorts", nodeType.instancePorts);
            GS::Array<GS::UniString> generations;
            for (const graph::GenerationDomain domain : nodeType.generations.Domains ())
                generations.Push (GS::UniString (graph::GenerationDomainName (domain), CC_UTF8));
            record.Add ("generations", generations);
            GS::Array<GS::ObjectState> inputs, outputs, parameters;
            for (const graph::PortSchema& port : nodeType.inputs) {
                GS::ObjectState state;
                state.Add ("portId", GraphText (port.id));
                state.Add ("label", GraphText (port.label));
                state.Add ("valueType", GraphValueTypeName (port.valueType));
                state.Add ("required", port.required);
                state.Add ("acceptsMultiple", port.acceptsMultiple);
                inputs.Push (std::move (state));
            }
            for (const graph::PortSchema& port : nodeType.outputs) {
                GS::ObjectState state;
                state.Add ("portId", GraphText (port.id));
                state.Add ("label", GraphText (port.label));
                state.Add ("valueType", GraphValueTypeName (port.valueType));
                outputs.Push (std::move (state));
            }
            for (const graph::ParameterSchema& parameter : nodeType.parameters) {
                GS::ObjectState state;
                state.Add ("parameterId", GraphText (parameter.id));
                state.Add ("label", GraphText (parameter.label));
                state.Add ("valueType", GraphValueTypeName (parameter.valueType));
                state.Add ("required", parameter.required);
                if (parameter.defaultValue.has_value ())
                    state.Add ("defaultValue", EncodeValue (*parameter.defaultValue, false));
                // UI-1's descriptor, and it is ABSENT rather than defaulted when
                // the type declares none - a client must be able to tell "this
                // parameter says nothing about how it is edited" from "this
                // parameter asked for the fallback", because the first is what
                // every parameter registered before this change means.
                if (parameter.ui.has_value ())
                    state.Add ("ui", EncodeParameterUi (*parameter.ui));
                parameters.Push (std::move (state));
            }
            record.Add ("inputs", inputs);
            record.Add ("outputs", outputs);
            record.Add ("parameters", parameters);
            records.Push (std::move (record));
        }
        // ⚠️ THE ELEMENT TYPE ORDER SHIPS WITH THE CATALOG, because the client
        // has to STACK the containers and cannot invent an order. Grouping by
        // first appearance would make the same model, clicked in a different
        // order, produce a different panel; a hard-coded order in the browser
        // would be a second copy of the table that goes stale after a build.
        // It is small, static, and fetched once with everything else.
        GS::Array<GS::ObjectState> elementTypes;
        for (const graph::ElementTypeDescriptor& type : graph::ElementTypeCatalog ()) {
            GS::ObjectState state;
            state.Add ("id", GraphText (type.id));
            state.Add ("label", GraphText (type.label));
            state.Add ("plural", GraphText (type.plural));
            state.Add ("container", type.container);
            elementTypes.Push (std::move (state));
        }

        GS::ObjectState response;
        response.Add ("nodeTypes", records);
        response.Add ("elementTypes", elementTypes);
        return response;
    }
};

class GraphGetStateCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        const graph::GraphId graphId = ReadGraphIdParam (params);
        const graph::GraphDocument document = graph::GraphRuntimeState::Get ().Document (graphId);
        const graph::ResultsSnapshot snapshot = graph::GraphRuntimeState::Get ().Results (graphId);
        const graph::NodeRegistry registry = graph::GraphRuntimeState::Get ().Catalog ();
        GS::Array<GS::ObjectState> nodes, edges;
        for (const auto& [nodeId, node] : document.Nodes ()) {
            GS::ObjectState record;
            record.Add ("nodeId", GraphText (nodeId));
            record.Add ("nodeType", GraphText (node.nodeType));
            // Stage F: the mode is DOCUMENT state, so it belongs in the state
            // projection beside the parameters and not in the per-run results.
            // A client that read it from the results would lose it the moment
            // the run cache was dropped.
            record.Add ("executionMode", GS::UniString (graph::ExecutionModeName (node.executionMode), CC_UTF8));
            // Only the ports that carry one, so a graph using no modifiers sends
            // nothing extra and a client can treat absence as "none".
            GS::Array<GS::ObjectState> modifiers;
            for (const auto& [portId, modifier] : node.inputModifiers) {
                if (modifier == graph::PortModifier::None)
                    continue;
                GS::ObjectState encodedModifier;
                encodedModifier.Add ("portId", GraphText (portId));
                encodedModifier.Add ("modifier", GS::UniString (graph::PortModifierName (modifier), CC_UTF8));
                modifiers.Push (std::move (encodedModifier));
            }
            record.Add ("inputModifiers", modifiers);
            GS::Array<GS::ObjectState> parameters;
            for (const auto& [parameterId, value] : node.parameters) {
                GS::ObjectState parameter;
                parameter.Add ("parameterId", GraphText (parameterId));
                parameter.Add ("value", EncodeValue (value, true));
                parameters.Push (std::move (parameter));
            }
            record.Add ("parameters", parameters);
            // Instance ports, for the types whose catalog entry has none. Absent
            // rather than empty on every other type, so a client can tell "this
            // node authors its own ports and currently has none" - a script file
            // that failed to parse - from "ask the catalog".
            if (const graph::NodeType* type = registry.Find (node.nodeType); type != nullptr && type->instancePorts) {
                GS::Array<GS::ObjectState> nodeInputs, nodeOutputs;
                for (const graph::PortSchema& port : node.dynamicInputs) {
                    GS::ObjectState state;
                    state.Add ("portId", GraphText (port.id));
                    state.Add ("label", GraphText (port.label));
                    state.Add ("valueType", GraphValueTypeName (port.valueType));
                    state.Add ("required", port.required);
                    state.Add ("acceptsMultiple", port.acceptsMultiple);
                    nodeInputs.Push (std::move (state));
                }
                for (const graph::PortSchema& port : node.dynamicOutputs) {
                    GS::ObjectState state;
                    state.Add ("portId", GraphText (port.id));
                    state.Add ("label", GraphText (port.label));
                    state.Add ("valueType", GraphValueTypeName (port.valueType));
                    nodeOutputs.Push (std::move (state));
                }
                record.Add ("inputs", nodeInputs);
                record.Add ("outputs", nodeOutputs);
            }
            nodes.Push (std::move (record));
        }
        for (const graph::Edge& edge : document.Edges ()) {
            GS::ObjectState record;
            record.Add ("sourceNode", GraphText (edge.sourceNode));
            record.Add ("sourcePort", GraphText (edge.sourcePort));
            record.Add ("targetNode", GraphText (edge.targetNode));
            record.Add ("targetPort", GraphText (edge.targetPort));
            edges.Push (std::move (record));
        }
        GS::ObjectState response;
        response.Add ("graphId", GraphText (graphId));
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
        const std::string editKind = GraphUtf8 (editKindValue);
        graph::GraphEdit edit;
        std::string error;

        if (editKind == "addNode") {
            GS::UniString nodeId, nodeType;
            params.Get ("nodeId", nodeId);
            params.Get ("nodeType", nodeType);
            graph::Node node { GraphUtf8 (nodeId), GraphUtf8 (nodeType) };

            GS::Array<GS::ObjectState> parameters;
            if (params.Get ("parameters", parameters)) {
                for (const GS::ObjectState& parameter : parameters) {
                    GS::UniString parameterId;
                    GS::ObjectState valueState;
                    graph::Value value;
                    if (!parameter.Get ("parameterId", parameterId) || !parameter.Get ("value", valueState) ||
                        !DecodeParameterValue (valueState, value, error))
                        return NativeCommandResult::Failure (GraphText (error.empty () ? "invalid parameter" : error));
                    node.parameters.insert_or_assign (GraphUtf8 (parameterId), std::move (value));
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
            edit.data = graph::RemoveNodeEdit { GraphUtf8 (nodeId) };
        }
        else if (editKind == "connect") {
            edit.data = graph::ConnectEdit { ReadEdge (params) };
        }
        else if (editKind == "disconnect") {
            edit.data = graph::DisconnectEdit { ReadEdge (params) };
        }
        // Stage F5. Deliberately ON THIS ENDPOINT rather than as verbs of their
        // own: a mode change and a release move the document exactly as a wire
        // does, so they get the same validation, the same revision and the same
        // dirty-closure answer. Two endpoints would be two answers.
        else if (editKind == "setExecutionMode") {
            GS::UniString nodeId, modeName;
            params.Get ("nodeId", nodeId);
            params.Get ("mode", modeName);
            graph::ExecutionMode mode = graph::ExecutionMode::Enabled;
            if (!graph::ParseExecutionMode (GraphUtf8 (modeName), mode))
                return NativeCommandResult::Failure (GraphText ("unknown execution mode: " + GraphUtf8 (modeName)));
            edit.data = graph::SetExecutionModeEdit { GraphUtf8 (nodeId), mode };
        }
        // Here for the same reason setExecutionMode is: a modifier moves the
        // document exactly as a wire does, so it gets the same validation, the
        // same revision and the same dirty-closure answer.
        else if (editKind == "setPortModifier") {
            GS::UniString nodeId, portId, modifierName;
            params.Get ("nodeId", nodeId);
            params.Get ("portId", portId);
            params.Get ("modifier", modifierName);
            graph::PortModifier modifier = graph::PortModifier::None;
            if (!graph::ParsePortModifier (GraphUtf8 (modifierName), modifier))
                return NativeCommandResult::Failure (GraphText ("unknown port modifier: " + GraphUtf8 (modifierName)));
            edit.data = graph::SetPortModifierEdit { GraphUtf8 (nodeId), GraphUtf8 (portId), modifier };
        }
        else if (editKind == "releaseHolding") {
            GS::UniString nodeId;
            params.Get ("nodeId", nodeId);
            edit.data = graph::ReleaseHoldingEdit { GraphUtf8 (nodeId) };
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
                    return NativeCommandResult::Failure (GraphText (error));
            }
            else if (params.Get ("numberValue", numberValue)) {
                value = graph::Value (numberValue);
            }
            else {
                return NativeCommandResult::Failure (GraphText ("setParam requires a value"));
            }
            edit.data = graph::SetParameterEdit { GraphUtf8 (nodeId), GraphUtf8 (parameterId), std::move (value) };
        }

        const graph::EditResult result = graph::GraphRuntimeState::Get ().Apply (ReadGraphIdParam (params), edit);
        if (!result.accepted) {
            // The code goes into the failure text until Phase 0 gives this
            // endpoint a structured rejection envelope. It is prefixed rather
            // than dropped so a rejection is already greppable, and so the
            // client migration to a `code` field is a parse change rather than
            // a runtime change.
            const std::string reported = result.code.empty () ? result.error : "[" + result.code + "] " + result.error;
            return NativeCommandResult::Failure (GraphText (reported));
        }
        GS::Array<GS::UniString> dirtyNodes;
        for (const graph::NodeId& nodeId : result.dirtyNodes)
            dirtyNodes.Push (GraphText (nodeId));
        GS::ObjectState response;
        response.Add ("revision", static_cast<GS::Int64> (result.revision));
        response.Add ("dirtyNodes", dirtyNodes);
        return response;
    }
};

class GraphEraseElementsCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Array<GS::UniString> nodeIds;
        GS::Array<GS::ObjectState> edgeStates;
        params.Get ("nodeIds", nodeIds);
        params.Get ("edges", edgeStates);

        graph::RemoveElementsEdit remove;
        remove.nodeIds.reserve (nodeIds.GetSize ());
        remove.edges.reserve (edgeStates.GetSize ());
        for (const GS::UniString& nodeId : nodeIds)
            remove.nodeIds.push_back (GraphUtf8 (nodeId));
        for (const GS::ObjectState& edge : edgeStates)
            remove.edges.push_back (ReadEdge (edge));

        const graph::EditResult result =
            graph::GraphRuntimeState::Get ().Apply (ReadGraphIdParam (params), graph::GraphEdit { std::move (remove) });
        if (!result.accepted)
            return NativeCommandResult::Failure (GraphText (result.error));

        GS::Array<GS::UniString> dirtyNodes;
        for (const graph::NodeId& nodeId : result.dirtyNodes)
            dirtyNodes.Push (GraphText (nodeId));
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
                request.targets.push_back (GraphUtf8 (target));
        }
        GS::UniString mode;
        if (params.Get ("mode", mode) && GraphUtf8 (mode) == "forced")
            request.mode = graph::EvaluationMode::Forced;
        // Absent means refused. A client has to ask for side effects in so many
        // words; nothing infers them from context.
        bool allowSideEffects = false;
        if (params.Get ("allowSideEffects", allowSideEffects))
            request.allowSideEffects = allowSideEffects;
        // 0 keeps the runtime's own choice. Present so a client can run the same
        // graph sequentially and in parallel and compare, which is how ADR-007's
        // gate is answered rather than asserted.
        GS::Int64 maxParallel = 0;
        if (params.Get ("maxParallel", maxParallel) && maxParallel > 0)
            request.maxParallel = static_cast<size_t> (maxParallel);

        const graph::EvaluationSummary summary =
            graph::GraphRuntimeState::Get ().Evaluate (ReadGraphIdParam (params), request);

        // A failed graph is a REPORTED outcome, not a failed command. The
        // difference matters: a client has to be able to render which node
        // failed and why, and a command failure carries only a string.
        GS::Array<GS::UniString> cyclicNodes;
        for (const graph::NodeId& nodeId : summary.cyclicNodes)
            cyclicNodes.Push (GraphText (nodeId));
        GS::ObjectState response;
        response.Add ("graphId", GraphText (summary.graphId));
        response.Add ("runId", static_cast<GS::Int64> (summary.runId));
        response.Add ("lastEventSeq", static_cast<GS::Int64> (summary.lastEventSeq));
        response.Add ("revision", static_cast<GS::Int64> (summary.revision));
        response.Add ("succeeded", summary.succeeded);
        response.Add ("cancelled", summary.cancelled);
        response.Add ("error", GraphText (summary.error));
        response.Add ("failedNode", GraphText (summary.failedNode));
        response.Add ("cyclicNodes", cyclicNodes);
        response.Add ("plannedCount", static_cast<GS::Int64> (summary.plannedCount));
        response.Add ("executedCount", static_cast<GS::Int64> (summary.executedCount));
        response.Add ("cacheHitCount", static_cast<GS::Int64> (summary.cacheHitCount));
        response.Add ("failedCount", static_cast<GS::Int64> (summary.failedCount));
        response.Add ("blockedCount", static_cast<GS::Int64> (summary.blockedCount));
        response.Add ("effectsCommitted", summary.effectsCommitted);
        GS::Array<GS::UniString> skipped;
        for (const graph::NodeId& nodeId : summary.skippedEffectNodes)
            skipped.Push (GraphText (nodeId));
        response.Add ("skippedEffectNodes", skipped);

        GS::Array<GS::ObjectState> levels;
        for (const graph::LevelMetrics& level : summary.parallelism.levels) {
            GS::ObjectState record;
            record.Add ("levelIndex", static_cast<GS::Int64> (level.levelIndex));
            record.Add ("executedCount", static_cast<GS::Int64> (level.executedCount));
            record.Add ("workerNodeCount", static_cast<GS::Int64> (level.workerNodeCount));
            record.Add ("hostNodeCount", static_cast<GS::Int64> (level.hostNodeCount));
            record.Add ("peakConcurrency", static_cast<GS::Int64> (level.peakConcurrency));
            record.Add ("wallClockMs", level.wallClockMs);
            record.Add ("workMs", level.workMs);
            levels.Push (record);
        }
        GS::ObjectState parallelism;
        parallelism.Add ("workerThreads", static_cast<GS::Int64> (summary.parallelism.workerThreads));
        parallelism.Add ("maxParallel", static_cast<GS::Int64> (summary.parallelism.maxParallel));
        parallelism.Add ("peakConcurrency", static_cast<GS::Int64> (summary.parallelism.peakConcurrency));
        parallelism.Add ("wallClockMs", summary.parallelism.wallClockMs);
        parallelism.Add ("workMs", summary.parallelism.workMs);
        parallelism.Add ("speedup", summary.parallelism.Speedup ());
        parallelism.Add ("levels", levels);
        response.Add ("parallelism", parallelism);
        return response;
    }
};

class GraphCancelCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        const graph::GraphId graphId = ReadGraphIdParam (params);
        GS::ObjectState response;
        response.Add ("graphId", GraphText (graphId));
        response.Add ("cancelledRunId", static_cast<GS::Int64> (graph::GraphRuntimeState::Get ().Cancel (graphId)));
        return response;
    }
};

class GraphGetNodeResultsCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        const graph::ResultsSnapshot snapshot = graph::GraphRuntimeState::Get ().Results (ReadGraphIdParam (params));
        GS::Array<GS::ObjectState> results;
        for (const graph::RuntimeNodeResult& nodeResult : snapshot.nodes) {
            const graph::NodeStatus& status = nodeResult.status;
            const std::shared_ptr<const graph::NodeResult>& result = nodeResult.result;
            GS::ObjectState record;
            record.Add ("nodeId", GraphText (nodeResult.nodeId));
            record.Add ("status", StatusName (status.state));
            // The stable half of the pair. `message` is prose for a person and
            // may be reworded; a client that needs to BRANCH reads this.
            record.Add ("code", GraphText (status.code));
            record.Add ("message", GraphText (status.message));
            record.Add ("durationMilliseconds", status.durationMilliseconds);
            record.Add ("itemCount", static_cast<GS::Int64> (status.itemCount));
            record.Add ("cacheHit", status.cacheHit);
            record.Add ("evaluationCount", static_cast<GS::Int64> (status.evaluationCount));
            record.Add ("runId", static_cast<GS::Int64> (status.runId));
            record.Add ("previewAvailable", false);
            GS::Array<GS::ObjectState> outputs;
            if (result) {
                for (const auto& [portId, tree] : result->outputs)
                    outputs.Push (EncodeProjectedOutput (portId, tree));
            }
            record.Add ("outputs", outputs);
            results.Push (std::move (record));
        }
        GS::ObjectState response;
        response.Add ("graphId", GraphText (snapshot.graphId));
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
        const graph::GraphId graphId = ReadGraphIdParam (params);
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
            record.Add ("nodeId", GraphText (event.nodeId));
            record.Add ("timestampMs", static_cast<GS::Int64> (event.timestampMs));
            record.Add ("message", GraphText (event.message));
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
        response.Add ("graphId", GraphText (graphId));
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
        const graph::GraphId graphId = ReadGraphIdParam (params);
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
            run.Add ("error", GraphText (record.error));
            run.Add ("failedNode", GraphText (record.failedNode));
            run.Add ("plannedCount", static_cast<GS::Int64> (record.plannedCount));
            run.Add ("executedCount", static_cast<GS::Int64> (record.executedCount));
            run.Add ("cacheHitCount", static_cast<GS::Int64> (record.cacheHitCount));
            run.Add ("failedCount", static_cast<GS::Int64> (record.failedCount));
            run.Add ("blockedCount", static_cast<GS::Int64> (record.blockedCount));
            GS::Array<GS::ObjectState> nodes;
            for (const graph::NodeRunRecord& node : record.nodes) {
                GS::ObjectState entry;
                entry.Add ("nodeId", GraphText (node.nodeId));
                entry.Add ("status", StatusName (node.finalState));
                entry.Add ("message", GraphText (node.message));
                entry.Add ("durationMilliseconds", node.durationMilliseconds);
                entry.Add ("itemCount", static_cast<GS::Int64> (node.itemCount));
                entry.Add ("cacheHit", node.cacheHit);
                nodes.Push (std::move (entry));
            }
            run.Add ("nodes", nodes);
            runs.Push (std::move (run));
        }

        GS::ObjectState response;
        response.Add ("graphId", GraphText (graphId));
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
        state.Add ("nodeId", GraphText (finding.nodeId));
        state.Add ("kind", GraphText (finding.kind));
        state.Add ("detail", GraphText (finding.detail));
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
        const graph::GraphId graphId = ReadGraphIdParam (params);
        const graph::GraphDependencyReport report = graph::GraphRuntimeState::Get ().Dependencies (graphId);
        GS::ObjectState response;
        response.Add ("graphId", GraphText (graphId));
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
        const graph::GraphId graphId = ReadGraphIdParam (params);
        GS::Int64 formatVersion = 0;
        params.Get ("formatVersion", formatVersion);

        const graph::CompatibilityReport report = graph::GraphRuntimeState::Get ().Compatibility (
            graphId, static_cast<uint32_t> (formatVersion < 0 ? 0 : formatVersion));

        GS::Array<GS::UniString> missing;
        for (const std::string& nodeType : report.missingNodeTypes)
            missing.Push (GraphText (nodeType));

        GS::ObjectState response;
        response.Add ("graphId", GraphText (graphId));
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
    { "GraphEraseElements", &MakeRegisteredNativeCommand<GraphEraseElementsCommand>, false, kEraseElementsInputSchema,
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
