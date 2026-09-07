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

#include "RhinoCompute/RhinoComputeProtocol.hpp"

#include <gtest/gtest.h>

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
