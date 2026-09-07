// RhinoCompute/RhinoComputeProtocol.cpp — the Resthopper wire.
//
// This is the second Grasshopper backend's equivalent of test_ghprotocol.cpp,
// and it exists for the same reason: the transport is the part that fails
// QUIETLY. compute answers HTTP 200 to a definition it could not solve, states
// the reason in an array nobody is required to read, and returns empty trees.
// Every refusal below was written against a measured response from
// compute.geometry 8.0.0.0 on Rhino 8.34, not against the documentation.
//
// The fixtures marked MEASURED are literal captures, trimmed only of length.

#include "RhinoCompute/RequestSequence.hpp"
#include "RhinoCompute/RhinoComputeProtocol.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

using namespace evp::rhinocompute;

namespace {

InputValue Value (const char* id, const char* type, const char* value)
{
    InputValue v;
    v.id = id;
    v.type = type;
    v.value = value;
    return v;
}

} // namespace

// ── request shaping ──────────────────────────────────────────────────────────

TEST (RhinoComputeRequest, IoRequestCarriesTheDefinitionAndNoPointer)
{
    const std::string body = BuildIoRequest ("QUJD");

    EXPECT_NE (body.find ("\"algo\""), std::string::npos);
    EXPECT_NE (body.find ("QUJD"), std::string::npos);
    // Discovery must never answer from a cache that could predate an edit.
    EXPECT_NE (body.find ("\"pointer\":null"), std::string::npos);
}

TEST (RhinoComputeRequest, PointerWinsOverDefinition)
{
    // The measured reason this matters: the same solve cost 52 ms carrying the
    // definition and 4.5 ms carrying the pointer. A 100 ms debounce that
    // re-uploads spends its whole budget on transport.
    SolveRequest request;
    request.pointer = "md5_28D8733B2DDE1E2A595F92A4C687255E";
    request.definitionBase64 = "QUJD";

    const std::string body = BuildSolveRequest (request);

    EXPECT_NE (body.find ("md5_28D8733B2DDE1E2A595F92A4C687255E"), std::string::npos);
    EXPECT_EQ (body.find ("QUJD"), std::string::npos);
}

TEST (RhinoComputeRequest, NumberIsWrittenWithoutALocale)
{
    SolveRequest request;
    request.pointer = "md5_x";
    request.inputs.push_back (Value ("Width", "number", "4.5"));

    const std::string body = BuildSolveRequest (request);

    EXPECT_NE (body.find ("System.Double"), std::string::npos);
    EXPECT_NE (body.find ("4.5"), std::string::npos);
    // A machine set to a comma decimal separator would otherwise write "4,5",
    // which compute rejects.
    EXPECT_EQ (body.find ("4,5"), std::string::npos);
}

TEST (RhinoComputeRequest, InputIdBecomesParamName)
{
    SolveRequest request;
    request.pointer = "md5_x";
    request.inputs.push_back (Value ("Width", "number", "1"));

    const std::string body = BuildSolveRequest (request);

    EXPECT_NE (body.find ("\"ParamName\":\"Width\""), std::string::npos);
    EXPECT_NE (body.find ("\"{0}\""), std::string::npos);
}

TEST (RhinoComputeRequest, StringValueIsJsonEncodedInsideData)
{
    // Resthopper carries a JSON-encoded value inside a JSON string, so a quote
    // in the user's text has to survive two levels of escaping.
    SolveRequest request;
    request.pointer = "md5_x";
    request.inputs.push_back (Value ("Label", "string", "a\"b"));

    const std::string body = BuildSolveRequest (request);

    EXPECT_NE (body.find ("System.String"), std::string::npos);
    EXPECT_NE (body.find ("\\\\\\\""), std::string::npos);
}

TEST (RhinoComputeRequest, UnencodableInputIsDroppedRatherThanGuessed)
{
    SolveRequest request;
    request.pointer = "md5_x";
    request.inputs.push_back (Value ("Width", "number", "not a number"));
    request.inputs.push_back (Value ("Depth", "number", "2"));

    const std::string body = BuildSolveRequest (request);

    EXPECT_EQ (body.find ("Width"), std::string::npos);
    EXPECT_NE (body.find ("Depth"), std::string::npos);
}

TEST (RhinoComputeRequest, BooleanAcceptsOnlyRecognisedSpellings)
{
    SolveRequest request;
    request.pointer = "md5_x";
    request.inputs.push_back (Value ("A", "boolean", "true"));
    request.inputs.push_back (Value ("B", "boolean", "yes"));

    const std::string body = BuildSolveRequest (request);

    EXPECT_NE (body.find ("\"ParamName\":\"A\""), std::string::npos);
    EXPECT_EQ (body.find ("\"ParamName\":\"B\""), std::string::npos);
}

// ── /io parsing ──────────────────────────────────────────────────────────────

TEST (RhinoComputeIo, ReadsPointerAndOneNumberInput)
{
    // MEASURED shape, one input, no errors.
    const std::string body = R"({"Description":"","CacheKey":"md5_ABC","InputNames":["Width"],)"
                             R"("Inputs":[{"Description":"","AtLeast":1,"AtMost":1,"TreeAccess":false,)"
                             R"("Minimum":0.1,"Maximum":20.0,"Name":"Width","Nickname":"Width","ParamType":"Number"}],)"
                             R"("Outputs":[],"Warnings":[],"Errors":[]})";

    WorkflowSchema schema;
    ASSERT_TRUE (ParseIoResponse (body, schema));

    EXPECT_EQ (schema.pointer, "md5_ABC");
    ASSERT_EQ (schema.inputs.size (), 1u);
    EXPECT_EQ (schema.inputs[0].id, "Width");
    EXPECT_EQ (schema.inputs[0].type, "number");
    EXPECT_TRUE (schema.inputs[0].hasMinimum);
    EXPECT_DOUBLE_EQ (schema.inputs[0].minimum, 0.1);
    EXPECT_TRUE (schema.inputs[0].hasMaximum);
    EXPECT_DOUBLE_EQ (schema.inputs[0].maximum, 20.0);
    EXPECT_TRUE (schema.inputs[0].required);
    EXPECT_TRUE (schema.errors.empty ());
}

TEST (RhinoComputeIo, CarriesTheDuplicateNameErrorComputeReports)
{
    // MEASURED, verbatim, from private/Grasshopper/BoxToMesh.gh: three stock
    // Hops "Get Number" components with unset nicknames. compute reported ONE
    // input, listed the collision twice, and answered 200. The solve that
    // followed returned empty trees and said nothing more.
    const std::string body =
        R"({"CacheKey":"md5_28D8733B2DDE1E2A595F92A4C687255E","InputNames":["Get Number"],)"
        R"("Inputs":[{"AtLeast":1,"AtMost":10,"Name":"Get Number","Nickname":null,"ParamType":"Number"}],)"
        R"("Errors":["Multiple input parameters with the same name were detected. Parameter names must be unique.",)"
        R"("Multiple input parameters with the same name were detected. Parameter names must be unique."]})";

    WorkflowSchema schema;
    ASSERT_TRUE (ParseIoResponse (body, schema));

    // A well-formed answer is a successful parse even when the DEFINITION is
    // wrong. The errors must survive to the panel.
    ASSERT_EQ (schema.errors.size (), 2u);
    EXPECT_EQ (schema.errors[0].source, ErrorSource::Grasshopper);
    EXPECT_NE (schema.errors[0].message.find ("same name"), std::string::npos);
}

TEST (RhinoComputeIo, RefusesTwoInputsSharingAnId)
{
    const std::string body = R"({"CacheKey":"md5_A","Inputs":[)"
                             R"({"AtLeast":1,"Name":"Width","Nickname":"W","ParamType":"Number"},)"
                             R"({"AtLeast":1,"Name":"Width","Nickname":"W2","ParamType":"Number"}],"Errors":[]})";

    WorkflowSchema schema;
    ASSERT_TRUE (ParseIoResponse (body, schema));

    bool named = false;
    for (const Error& e : schema.errors) {
        if (e.code == "GH_INPUT_DUPLICATE" && e.message.find ("Width") != std::string::npos)
            named = true;
    }

    EXPECT_TRUE (named);
}

TEST (RhinoComputeIo, UnsupportedParamTypeIsRefusedRatherThanTreatedAsText)
{
    // "Generic Data" is what BoxToMesh.gh's output reported. A control the user
    // can type into that can never produce a valid solve is worse than none.
    const std::string body = R"({"CacheKey":"md5_A","Inputs":[)"
                             R"({"AtLeast":1,"Name":"Thing","Nickname":"T","ParamType":"Generic Data"}],"Errors":[]})";

    WorkflowSchema schema;
    ASSERT_TRUE (ParseIoResponse (body, schema));

    EXPECT_TRUE (schema.inputs.empty ());
    ASSERT_FALSE (schema.errors.empty ());
    EXPECT_EQ (schema.errors[0].code, "GH_INPUT_UNSUPPORTED");
}

TEST (RhinoComputeIo, AtLeastZeroMeansOptional)
{
    const std::string body = R"({"CacheKey":"md5_A","Inputs":[)"
                             R"({"AtLeast":0,"Name":"Width","Nickname":"W","ParamType":"Number"}],"Errors":[]})";

    WorkflowSchema schema;
    ASSERT_TRUE (ParseIoResponse (body, schema));

    ASSERT_EQ (schema.inputs.size (), 1u);
    EXPECT_FALSE (schema.inputs[0].required);
}

TEST (RhinoComputeIo, MalformedJsonIsARefusalWithAReason)
{
    WorkflowSchema schema;
    EXPECT_FALSE (ParseIoResponse ("{not json", schema));
    ASSERT_FALSE (schema.errors.empty ());
    EXPECT_EQ (schema.errors[0].code, "COMPUTE_BAD_JSON");
}

// ── solve parsing ────────────────────────────────────────────────────────────

TEST (RhinoComputeSolve, ReadsPointerAndUnitsFromAnEmptySolve)
{
    // MEASURED: what BoxToMesh.gh actually returned. modelunits comes from the
    // definition and must be reconciled before a value becomes a dimension.
    const std::string body =
        R"({"modelunits":"Millimeters","dataversion":7,"algo":"","filename":null,)"
        R"("pointer":"md5_28D8733B2DDE1E2A595F92A4C687255E","cachesolve":false,)"
        R"("values":[{"ParamName":"Content","InnerTree":{}}],)"
        R"("errors":["Multiple input parameters with the same name were detected. Parameter names must be unique."]})";

    SolveResult result;
    ASSERT_TRUE (ParseSolveResponse (body, result));

    EXPECT_EQ (result.modelUnits, "Millimeters");
    EXPECT_EQ (result.pointer, "md5_28D8733B2DDE1E2A595F92A4C687255E");
    EXPECT_TRUE (result.meshes.empty ());
    // A 200 carrying errors is NOT a success. This is the case that would
    // otherwise draw an empty preview and report nothing.
    EXPECT_FALSE (result.success);
    ASSERT_EQ (result.errors.size (), 1u);
    EXPECT_EQ (result.errors[0].source, ErrorSource::Grasshopper);
}

TEST (RhinoComputeSolve, ReadsAPreviewMeshOfExplicitFloats)
{
    const std::string body =
        R"({"modelunits":"Meters","pointer":"md5_A","values":[{"ParamName":"Preview","InnerTree":{"{0}":[)"
        R"({"type":"System.String","data":"{\"tapiocaPreview\":1,\"id\":7,)"
        R"(\"vertices\":[0,0,0, 1,0,0, 0,1,0],\"normals\":[0,0,1, 0,0,1, 0,0,1],)"
        R"(\"indices\":[0,1,2]}"}]}}],"errors":[]})";

    SolveResult result;
    ASSERT_TRUE (ParseSolveResponse (body, result));

    ASSERT_EQ (result.meshes.size (), 1u);
    EXPECT_EQ (result.meshes[0].id, 7u);
    EXPECT_EQ (result.meshes[0].vertices.size (), 9u);
    EXPECT_EQ (result.meshes[0].normals.size (), 9u);
    ASSERT_EQ (result.meshes[0].indices.size (), 3u);
    EXPECT_EQ (result.meshes[0].indices[2], 2u);
    EXPECT_TRUE (result.success);
}

TEST (RhinoComputeSolve, RefusesAMeshThatIndexesPastItsOwnVertices)
{
    // ⚠️ THE PRODUCER IS ANOTHER PROCESS RUNNING THIRD-PARTY COMPONENTS, and
    // these numbers are about to become GPU buffers inside Archicad.exe. An
    // out-of-range index must be a named refusal, never a read.
    const std::string body = R"({"pointer":"md5_A","values":[{"ParamName":"Preview","InnerTree":{"{0}":[)"
                             R"({"type":"System.String","data":"{\"tapiocaPreview\":1,\"id\":1,)"
                             R"(\"vertices\":[0,0,0, 1,0,0, 0,1,0],\"indices\":[0,1,9]}"}]}}],"errors":[]})";

    SolveResult result;
    ASSERT_TRUE (ParseSolveResponse (body, result));

    EXPECT_TRUE (result.meshes.empty ());
    ASSERT_FALSE (result.errors.empty ());
    EXPECT_EQ (result.errors[0].code, "PREVIEW_OUT_OF_RANGE");
}

TEST (RhinoComputeSolve, RefusesARaggedVertexArray)
{
    const std::string body =
        R"({"pointer":"md5_A","values":[{"ParamName":"Preview","InnerTree":{"{0}":[)"
        R"({"type":"System.String","data":"{\"tapiocaPreview\":1,\"vertices\":[0,0,0, 1,0],\"indices\":[]}"}]}}],)"
        R"("errors":[]})";

    SolveResult result;
    ASSERT_TRUE (ParseSolveResponse (body, result));

    EXPECT_TRUE (result.meshes.empty ());
    ASSERT_FALSE (result.errors.empty ());
    EXPECT_EQ (result.errors[0].code, "PREVIEW_MALFORMED");
}

TEST (RhinoComputeSolve, RefusesNormalsThatDoNotMatchVertices)
{
    const std::string body = R"({"pointer":"md5_A","values":[{"ParamName":"Preview","InnerTree":{"{0}":[)"
                             R"({"type":"System.String","data":"{\"tapiocaPreview\":1,\"vertices\":[0,0,0],)"
                             R"(\"normals\":[0,0,1, 0,0,1],\"indices\":[]}"}]}}],"errors":[]})";

    SolveResult result;
    ASSERT_TRUE (ParseSolveResponse (body, result));

    EXPECT_TRUE (result.meshes.empty ());
    ASSERT_FALSE (result.errors.empty ());
    EXPECT_EQ (result.errors[0].code, "PREVIEW_MALFORMED");
}

TEST (RhinoComputeSolve, IgnoresOutputsThatAreNotTapiocaPreview)
{
    // A definition is free to output anything. Only what the Tapioca preview
    // component stated is drawn; a serialized RhinoCommon object is not decoded
    // here and must not be mistaken for one.
    const std::string body =
        R"({"pointer":"md5_A","values":[{"ParamName":"Content","InnerTree":{"{0}":[)"
        R"({"type":"Rhino.Geometry.Mesh","data":"{\"archive3dm\":70,\"data\":\"AAAA\"}"}]}}],"errors":[]})";

    SolveResult result;
    ASSERT_TRUE (ParseSolveResponse (body, result));

    EXPECT_TRUE (result.meshes.empty ());
    EXPECT_TRUE (result.errors.empty ());
    EXPECT_TRUE (result.success);
}

TEST (RhinoComputeSolve, ParsesTheLiveBoxCapture)
{
    // MEASURED, end to end: three numbers injected into BoxToMesh.gh (4, 6, 3),
    // meshed, emitted by TapiocaPreviewOutComponent and returned by a Context
    // Print. This is the exact bytes compute answered with, and it is the only
    // fixture here that has been through a real Grasshopper solve.
    //
    // Note the path: "{0;0}", not "{0}". A Context Print downstream of a mesh
    // operation produces a two-level path, so anything that assumed a flat
    // branch key would drop the whole preview.
    const std::string body =
        R"RAW({"modelunits":"Millimeters","pointer":"md5_FFFAAB0FE520A5A5A7BBA5CD81587BC5","values":[{"ParamName":"Tx","InnerTree":{"{0;0}":[{"type":"System.String","data":"{\"tapiocaPreview\":1,\"id\":0,\"vertices\":[-4,-6,-3,-4,-6,3,4,-6,-3,4,-6,3,4,-6,-3,4,-6,3,4,6,-3,4,6,3,4,6,-3,4,6,3,-4,6,-3,-4,6,3,-4,6,-3,-4,6,3,-4,-6,-3,-4,-6,3,-4,-6,-3,4,-6,-3,-4,6,-3,4,6,-3,-4,-6,3,-4,6,3,4,-6,3,4,6,3],\"normals\":[0,-1,0,0,-1,0,0,-1,0,0,-1,0,1,0,0,1,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,1,0,0,1,0,-1,0,0,-1,0,0,-1,0,0,-1,0,0,0,0,-1,0,0,-1,0,0,-1,0,0,-1,0,0,1,0,0,1,0,0,1,0,0,1],\"indices\":[1,0,2,5,4,6,9,8,10,13,12,14,17,16,18,21,20,22,1,2,3,5,6,7,9,10,11,13,14,15,17,18,19,21,22,23]}"}]}}]})RAW";

    SolveResult result;
    ASSERT_TRUE (ParseSolveResponse (body, result));
    EXPECT_TRUE (result.errors.empty ());

    ASSERT_EQ (result.meshes.size (), 1u);
    const PreviewMesh& mesh = result.meshes[0];

    // A box: 24 vertices (four per face, unshared for per-face normals) and 12
    // triangles.
    EXPECT_EQ (mesh.vertices.size (), 72u);
    EXPECT_EQ (mesh.normals.size (), 72u);
    EXPECT_EQ (mesh.indices.size (), 36u);

    // The injected 4/6/3 as a centred box: x[-4,4] y[-6,6] z[-3,3]. If injection
    // silently stopped working again, this is the assertion that would catch it.
    float minX = mesh.vertices[0], maxX = mesh.vertices[0];
    float minY = mesh.vertices[1], maxY = mesh.vertices[1];
    float minZ = mesh.vertices[2], maxZ = mesh.vertices[2];
    for (size_t i = 0; i + 2 < mesh.vertices.size (); i += 3) {
        minX = std::min (minX, mesh.vertices[i]);
        maxX = std::max (maxX, mesh.vertices[i]);
        minY = std::min (minY, mesh.vertices[i + 1]);
        maxY = std::max (maxY, mesh.vertices[i + 1]);
        minZ = std::min (minZ, mesh.vertices[i + 2]);
        maxZ = std::max (maxZ, mesh.vertices[i + 2]);
    }

    EXPECT_FLOAT_EQ (maxX - minX, 8.0f);
    EXPECT_FLOAT_EQ (maxY - minY, 12.0f);
    EXPECT_FLOAT_EQ (maxZ - minZ, 6.0f);
}

TEST (RhinoComputeSolve, CarriesWarningsWithoutFailingTheSolve)
{
    // A driven input is the case this exists for: injection is ignored, the
    // solve succeeds on the wired value, and this array is the only trace.
    const std::string body =
        R"({"pointer":"md5_A","values":[],"warnings":["This input has something wired into it."]})";

    SolveResult result;
    ASSERT_TRUE (ParseSolveResponse (body, result));

    ASSERT_EQ (result.warnings.size (), 1u);
    EXPECT_NE (result.warnings[0].find ("wired into it"), std::string::npos);
    // A warning is not a failure. Reporting it as one would make an ordinary
    // authoring mistake look like a broken worker.
    EXPECT_TRUE (result.success);
    EXPECT_TRUE (result.errors.empty ());
}

// ── readiness ────────────────────────────────────────────────────────────────

TEST (RhinoComputeReadiness, RecognisesTheNoChildRefusal)
{
    // ⚠️ GET /healthcheck ANSWERED "Healthy" FOR A FULL MINUTE WITH ZERO WORKING
    // CHILDREN. Readiness is a request that reaches a child, and this is what
    // its failure looks like.
    EXPECT_TRUE (IsNoServerResponse (R"({"error":"Internal Server Error","message":"No compute server found"})"));
    EXPECT_FALSE (IsNoServerResponse (R"({"rhino":"8.34.26223.11001","compute":"8.0.0.0"})"));
}

TEST (RhinoComputeTypes, UnknownParamTypesMapToNothing)
{
    EXPECT_EQ (SchemaTypeForParamType ("Number"), "number");
    EXPECT_EQ (SchemaTypeForParamType ("Integer"), "integer");
    EXPECT_EQ (SchemaTypeForParamType ("Boolean"), "boolean");
    EXPECT_EQ (SchemaTypeForParamType ("Text"), "string");
    EXPECT_TRUE (SchemaTypeForParamType ("Generic Data").empty ());
    EXPECT_TRUE (SchemaTypeForParamType ("Curve").empty ());
}

TEST (RhinoComputeBase64, MatchesTheThreePaddingCases)
{
    EXPECT_EQ (ToBase64 ({ 'A', 'B', 'C' }), "QUJD");
    EXPECT_EQ (ToBase64 ({ 'A' }), "QQ==");
    EXPECT_EQ (ToBase64 ({ 'A', 'B' }), "QUI=");
    EXPECT_EQ (ToBase64 ({}), "");
}

// ── request ordering (RC-005) ────────────────────────────────────────────────

TEST (RhinoComputeSequence, IdsAreMonotonicAndNeverZero)
{
    RequestSequence sequence;
    EXPECT_EQ (sequence.Latest (), 0u);
    EXPECT_EQ (sequence.Next (), 1u);
    EXPECT_EQ (sequence.Next (), 2u);
    EXPECT_EQ (sequence.Latest (), 2u);
}

TEST (RhinoComputeSequence, AStaleResultCanNeverOverwriteANewerOne)
{
    // The case this exists for: a slider drag issues 1 then 2; the cached solve
    // for 2 returns in 4 ms and the cold one for 1 returns in 120 ms. Without
    // ordering the user ends up looking at the value they already dragged past.
    RequestSequence sequence;
    const uint64_t first = sequence.Next ();
    const uint64_t second = sequence.Next ();

    EXPECT_TRUE (sequence.Accept (second));
    EXPECT_FALSE (sequence.Accept (first));
    EXPECT_EQ (sequence.Accepted (), second);
}

TEST (RhinoComputeSequence, AcceptanceIsOrderedAgainstWhatWasAcceptedNotWhatWasIssued)
{
    // Issue three, and let the newest FAIL so it is never accepted. The second
    // must still be drawable: ordering against `issued` would reject it and
    // leave the viewport empty rather than one step behind.
    RequestSequence sequence;
    sequence.Next ();
    const uint64_t second = sequence.Next ();
    sequence.Next ();

    EXPECT_TRUE (sequence.Accept (second));
    EXPECT_EQ (sequence.Accepted (), second);
}

TEST (RhinoComputeSequence, TheSameResultIsNotAcceptedTwice)
{
    // The preview cache treats a repeat as a change, so a duplicated response
    // would rebuild GPU buffers for geometry that did not move.
    RequestSequence sequence;
    const uint64_t id = sequence.Next ();

    EXPECT_TRUE (sequence.Accept (id));
    EXPECT_FALSE (sequence.Accept (id));
}

TEST (RhinoComputeSequence, ZeroIsNeverAccepted)
{
    // A default-constructed SolveResult carries requestId 0. It must not count
    // as a legitimate answer to anything.
    RequestSequence sequence;
    EXPECT_FALSE (sequence.Accept (0));
}

TEST (RhinoComputeSequence, IsCurrentOnlyForTheNewestIssuedId)
{
    RequestSequence sequence;
    const uint64_t first = sequence.Next ();
    EXPECT_TRUE (sequence.IsCurrent (first));

    const uint64_t second = sequence.Next ();
    EXPECT_FALSE (sequence.IsCurrent (first));
    EXPECT_TRUE (sequence.IsCurrent (second));
}

TEST (RhinoComputeSequence, ResetForgetsAcceptanceWithoutRewindingIssuedIds)
{
    // Loading a new definition: the previous preview is GONE rather than
    // superseded, so the next result must be accepted whatever its id. But a
    // solve still in flight from the OLD definition must not pass the gate, so
    // ids may not rewind.
    RequestSequence sequence;
    const uint64_t stale = sequence.Next ();
    EXPECT_TRUE (sequence.Accept (stale));

    sequence.ResetAccepted ();
    EXPECT_EQ (sequence.Accepted (), 0u);

    // The in-flight id from before the reset is not newer than what was issued,
    // and the next id keeps climbing past it.
    EXPECT_GT (sequence.Next (), stale);
}
