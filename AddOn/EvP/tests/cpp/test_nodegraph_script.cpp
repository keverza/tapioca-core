// The script node family, offline.
//
// ⚠️ THIS FILE IS THE ARGUMENT FOR EMBEDDING A JAVASCRIPT ENGINE. The WebView
// could have run this JavaScript for nothing; what it could not have done is run
// it HERE, with no Archicad, no palette and no APX rebuild. A script node runs
// code the user wrote, which makes it the least predictable body in the catalog
// and the one whose containment - the time budget, the marshalling, the failure
// paths - most needs a test that runs on every commit.

#include "NodeGraph/GraphEdit.hpp"
#include "NodeGraph/GraphRuntimeState.hpp"
#include "NodeGraph/GraphSerializer.hpp"
#include "NodeGraph/NodeExecution.hpp"
#include "NodeGraph/NodeRegistry.hpp"
#include "NodeGraph/ScriptManifest.hpp"
#include "NodeGraph/ScriptNodes.hpp"
#include "NodeGraph/ScriptRuntime.hpp"
#include "NodeGraph/ScriptReload.hpp"
#include "NodeGraph/ScriptSource.hpp"
#include "NodeGraph/ScriptValueJson.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <fstream>
#include <string>

using namespace evp::nodegraph;

namespace {

ScriptManifest ParsePython (const std::string& source)
{
    return ParseScriptManifest (source, ScriptLanguage::Python);
}

ScriptManifest ParseJs (const std::string& source)
{
    return ParseScriptManifest (source, ScriptLanguage::JavaScript);
}

ScriptRunResult RunJs (const std::string& source, ValueMap inputs, std::vector<PortSchema> outputs,
                       double budgetMs = 2000.0)
{
    InstallJavaScriptRuntime ();
    ScriptRunRequest request;
    request.language = ScriptLanguage::JavaScript;
    request.path = "test.js";
    request.source = source;
    request.inputs = std::move (inputs);
    request.outputs = std::move (outputs);
    request.timeBudgetMs = budgetMs;
    return ActiveScriptRuntime (ScriptLanguage::JavaScript)->Run (request);
}

PortSchema Out (const char* id, ValueType type)
{
    return PortSchema { id, id, type, true, false };
}

// A temporary file that removes itself, so a failing assertion cannot leave one
// behind for the next run to trip over.
class TempScript {
  public:
    explicit TempScript (const char* name, const std::string& contents)
    {
        path_ = std::filesystem::temp_directory_path () / name;
        Write (contents);
    }
    ~TempScript ()
    {
        std::error_code code;
        std::filesystem::remove (path_, code);
    }

    void Write (const std::string& contents) const
    {
        std::ofstream stream (path_, std::ios::binary | std::ios::trunc);
        stream << contents;
    }

    std::string Path () const
    {
        return path_.string ();
    }

  private:
    std::filesystem::path path_;
};

} // namespace

// ---------------------------------------------------------------------------
// The header. This is where a node's ports come from, so a parse that is subtly
// wrong is a node whose interface is subtly wrong.

TEST (ScriptManifest, ReadsPortsFromTheLeadingCommentBlock)
{
    const ScriptManifest manifest = ParsePython ("# @name Area\n"
                                                 "# @in radius : number = 1.5  \"Outer radius\"\n"
                                                 "# @out area : number\n"
                                                 "area = radius * radius\n");
    ASSERT_TRUE (manifest.Ok ()) << manifest.diagnostics.front ().message;
    EXPECT_EQ (manifest.name, "Area");
    ASSERT_EQ (manifest.inputs.size (), 1u);
    EXPECT_EQ (manifest.inputs[0].id, "radius");
    EXPECT_EQ (manifest.inputs[0].label, "Outer radius");
    EXPECT_EQ (manifest.inputs[0].valueType, ValueType::Double);
    // A default makes an input optional, exactly as it does for every other type
    // in the catalog: the node works the moment it is placed, and the port is
    // there so something upstream can take over.
    EXPECT_FALSE (manifest.inputs[0].required);
    ASSERT_EQ (manifest.outputs.size (), 1u);
    EXPECT_EQ (manifest.outputs[0].id, "area");
    ASSERT_TRUE (manifest.defaults.contains ("radius"));
    EXPECT_EQ (std::get<double> (manifest.defaults.at ("radius").DataValue ()), 1.5);
}

TEST (ScriptManifest, JavaScriptUsesTheOtherCommentPrefix)
{
    const ScriptManifest manifest = ParseJs ("// @in a : number\n// @out b : number\nb = a;\n");
    ASSERT_TRUE (manifest.Ok ());
    EXPECT_EQ (manifest.inputs[0].id, "a");
}

TEST (ScriptManifest, StopsAtTheFirstLineThatIsNotAComment)
{
    // ⚠️ THE RULE THAT KEEPS A DOCSTRING FROM RESHAPING A NODE. A `@out` further
    // down the file - in a commented-out experiment, in vendored code - must not
    // silently add a port and drop the user's wires.
    const ScriptManifest manifest = ParsePython ("# @in a : number\n"
                                                 "# @out b : number\n"
                                                 "b = a\n"
                                                 "# @out sneaky : number\n");
    ASSERT_TRUE (manifest.Ok ());
    EXPECT_EQ (manifest.outputs.size (), 1u);
}

TEST (ScriptManifest, AShebangDoesNotEndTheBlock)
{
    const ScriptManifest manifest = ParsePython ("#!/usr/bin/env python3\n# @out b : number\nb = 1\n");
    EXPECT_TRUE (manifest.Ok ());
}

TEST (ScriptManifest, AnUntypedOutputIsRefusedAndAnUntypedInputIsNot)
{
    // An output's value is checked against its port's type, so an untyped output
    // could only ever accept anything - which turns every wiring mistake into a
    // surprise somewhere downstream.
    const ScriptManifest bad = ParsePython ("# @in a\n# @out b\nb = a\n");
    ASSERT_FALSE (bad.Ok ());
    EXPECT_NE (bad.diagnostics.front ().message.find ("needs a type"), std::string::npos);
    EXPECT_EQ (bad.diagnostics.front ().line, 2u);

    const ScriptManifest good = ParsePython ("# @in a\n# @out b : number\nb = a\n");
    ASSERT_TRUE (good.Ok ());
    EXPECT_EQ (good.inputs[0].valueType, ValueType::Absent);
}

TEST (ScriptManifest, AQuotedDefaultIsNotMistakenForTheLabel)
{
    // â ï¸ A LINE CAN CARRY TWO QUOTED GROUPS, and reading the first one made
    // the DEFAULT into the label - silently. The port still appeared and was
    // still correctly typed; it was merely mislabelled and had lost its value,
    // which is exactly the kind of wrong that no error message ever reports. The
    // label is the last quoted group, so it is scanned from the end.
    const ScriptManifest manifest = ParsePython ("# @in label : text = \"wall\"   \"Some text\"\n# @out out : text\n");
    ASSERT_TRUE (manifest.Ok ()) << manifest.diagnostics.front ().message;
    ASSERT_EQ (manifest.inputs.size (), 1u);
    EXPECT_EQ (manifest.inputs[0].id, "label");
    EXPECT_EQ (manifest.inputs[0].label, "Some text");
    ASSERT_TRUE (manifest.defaults.contains ("label"));
    EXPECT_EQ (std::get<std::string> (manifest.defaults.at ("label").DataValue ()), "wall");
}

TEST (ScriptManifest, ALabelAloneAndADefaultAloneBothStillWork)
{
    const ScriptManifest labelled = ParsePython ("# @in a : number   \"Just a label\"\n# @out b : number\n");
    ASSERT_TRUE (labelled.Ok ());
    EXPECT_EQ (labelled.inputs[0].label, "Just a label");
    EXPECT_FALSE (labelled.defaults.contains ("a"));

    // One quoted group directly after `=` is the VALUE, not a label - see
    // SplitTrailingLabel. The port falls back to its own id for a label.
    const ScriptManifest defaulted = ParsePython ("# @in a : text = \"only\"\n# @out b : number\n");
    ASSERT_TRUE (defaulted.Ok ());
    EXPECT_EQ (defaulted.inputs[0].label, "a");
    ASSERT_TRUE (defaulted.defaults.contains ("a"));
    EXPECT_EQ (std::get<std::string> (defaulted.defaults.at ("a").DataValue ()), "only");
}

TEST (ScriptManifest, ReportsTheLineOfEveryProblem)
{
    const ScriptManifest manifest = ParsePython ("# @in a : number\n"
                                                 "# @in a : number\n"
                                                 "# @in 9bad : number\n"
                                                 "# @in c : banana\n"
                                                 "# @out d : number\n");
    ASSERT_EQ (manifest.diagnostics.size (), 3u);
    EXPECT_EQ (manifest.diagnostics[0].line, 2u);
    EXPECT_EQ (manifest.diagnostics[1].line, 3u);
    EXPECT_EQ (manifest.diagnostics[2].line, 4u);
}

TEST (ScriptManifest, LeavesForeignDirectivesAlone)
{
    // Comment blocks are full of @param and @returns from whatever other tool the
    // author uses. Warning about those would train the user to ignore warnings.
    const ScriptManifest manifest = ParsePython ("# @param x the thing\n# @returns nothing\n# @out b : number\n");
    EXPECT_TRUE (manifest.Ok ());
}

TEST (ScriptManifest, ScriptWithNoDirectivesSaysSoRatherThanBeingSilentlyPortless)
{
    const ScriptManifest manifest = ParsePython ("# just a script\nprint('hi')\n");
    ASSERT_FALSE (manifest.Ok ());
    EXPECT_EQ (manifest.diagnostics.front ().line, 0u);
}

TEST (ScriptManifest, LanguageFollowsTheExtension)
{
    ScriptLanguage language = ScriptLanguage::Python;
    ASSERT_TRUE (ScriptLanguageFromPath ("C:\\a\\b.JS", language));
    EXPECT_EQ (language, ScriptLanguage::JavaScript);
    ASSERT_TRUE (ScriptLanguageFromPath ("/home/u/x.py", language));
    EXPECT_EQ (language, ScriptLanguage::Python);
    EXPECT_FALSE (ScriptLanguageFromPath ("notes.txt", language));
}

// ---------------------------------------------------------------------------
// Reading the file.

TEST (ScriptSource, ReadsContentAndStamp)
{
    const TempScript file ("tapioca_script_read.py", "# @out b : number\nb = 1\n");
    const ScriptRead read = ReadScript (file.Path ());
    ASSERT_TRUE (read.ok) << read.error;
    EXPECT_NE (read.source.find ("@out"), std::string::npos);
    EXPECT_TRUE (read.stamp.exists);
    EXPECT_GT (read.stamp.sizeBytes, 0u);
}

TEST (ScriptSource, StripsAUtf8ByteOrderMark)
{
    // Notepad and several editors write one by default. Left in place it is three
    // bytes in front of the first line: the header parser misses a first-line
    // directive and both engines reject the file at character one - the least
    // informative failure available for the most ordinary of causes.
    const TempScript file ("tapioca_script_bom.py", "\xEF\xBB\xBF# @out b : number\nb = 1\n");
    const ScriptRead read = ReadScript (file.Path ());
    ASSERT_TRUE (read.ok);
    EXPECT_EQ (read.source.rfind ("#", 0), 0u);
    EXPECT_TRUE (ParsePython (read.source).Ok ());
}

TEST (ScriptSource, AMissingFileIsAnAnswerRatherThanAThrow)
{
    const ScriptRead read = ReadScript ("Z:\\definitely\\not\\here.py");
    EXPECT_FALSE (read.ok);
    EXPECT_FALSE (read.stamp.exists);
    EXPECT_NE (read.error.find ("here.py"), std::string::npos);
}

TEST (ScriptSource, TheTemplateRefusesToOverwriteAnExistingFile)
{
    // Scaffolding must never be able to destroy work: "create" and "overwrite"
    // are not the same request.
    const TempScript file ("tapioca_script_template.py", "mine\n");
    std::string error;
    EXPECT_FALSE (WriteScriptTemplate (file.Path (), error));
    EXPECT_NE (error.find ("already"), std::string::npos);
    EXPECT_EQ (ReadScript (file.Path ()).source, "mine\n");
}

TEST (ScriptSource, TheTemplateItWritesParsesAndDeclaresPorts)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path () / "tapioca_script_new.py";
    std::error_code code;
    std::filesystem::remove (path, code);

    std::string error;
    ASSERT_TRUE (WriteScriptTemplate (path.string (), error)) << error;
    const ScriptRead read = ReadScript (path.string ());
    ASSERT_TRUE (read.ok);
    const ScriptManifest manifest = ParsePython (read.source);
    EXPECT_TRUE (manifest.Ok ()) << manifest.diagnostics.front ().message;
    EXPECT_FALSE (manifest.outputs.empty ());
    std::filesystem::remove (path, code);
}

// ---------------------------------------------------------------------------
// Saving an editor buffer back.
//
// ⚠️ THESE ARE THE TESTS THAT LET THE PALETTE HAVE AN EDITOR AT ALL. A script
// node's file is normally open in VSCode at the same time; the only thing
// standing between that and a silent loss of someone's work is the base-hash
// guard, so what it refuses matters more here than what it accepts.

TEST (ScriptWrite, ReplacesTheFileWhenTheBaseHashStillMatches)
{
    const TempScript file ("tapioca_script_write.py", "# @out b : number\nb = 1\n");
    const std::string base = HashScriptSource (ReadScript (file.Path ()).source);

    const ScriptWrite written = WriteScriptSource (file.Path (), "# @out b : number\nb = 2\n", base,
                                                   &HashScriptSource);
    ASSERT_TRUE (written.ok) << written.error;
    EXPECT_FALSE (written.conflict);
    EXPECT_EQ (ReadScript (file.Path ()).source, "# @out b : number\nb = 2\n");
    EXPECT_TRUE (written.stamp.exists);
    // The hash comes back so the buffer can go on guarding subsequent saves
    // without a round trip through Read.
    EXPECT_EQ (written.diskHash, HashScriptSource ("# @out b : number\nb = 2\n"));
}

TEST (ScriptWrite, RefusesAndHandsBackTheOtherVersionWhenTheFileChangedMeanwhile)
{
    // The whole point of the guard: the user opened the buffer, went to VSCode,
    // edited and saved there, then pressed Save in the palette. The palette's
    // buffer is older, and applying it would destroy the VSCode edit.
    const TempScript file ("tapioca_script_conflict.py", "mine\n");
    const std::string base = HashScriptSource ("mine\n");
    file.Write ("theirs\n");

    const ScriptWrite written = WriteScriptSource (file.Path (), "buffer\n", base, &HashScriptSource);
    EXPECT_FALSE (written.ok);
    EXPECT_TRUE (written.conflict);
    EXPECT_EQ (written.diskSource, "theirs\n");
    EXPECT_EQ (written.diskHash, HashScriptSource ("theirs\n"));
    // And nothing was written: a refused save must leave disk exactly as it was.
    EXPECT_EQ (ReadScript (file.Path ()).source, "theirs\n");
}

TEST (ScriptWrite, AnEmptyBaseHashWritesAFileThatIsNotThereYet)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path () / "tapioca_script_fresh.py";
    std::error_code code;
    std::filesystem::remove (path, code);

    const ScriptWrite written = WriteScriptSource (path.string (), "b = 1\n", std::string {}, &HashScriptSource);
    ASSERT_TRUE (written.ok) << written.error;
    EXPECT_EQ (ReadScript (path.string ()).source, "b = 1\n");
    std::filesystem::remove (path, code);
}

TEST (ScriptWrite, AFileThatVanishedIsAConflictRatherThanASilentRecreate)
{
    // Deleting or renaming a script is a deliberate act. Re-creating it because
    // a buffer happened to still be open would undo that without saying so.
    const std::filesystem::path path = std::filesystem::temp_directory_path () / "tapioca_script_gone.py";
    std::error_code code;
    std::filesystem::remove (path, code);

    const ScriptWrite written =
        WriteScriptSource (path.string (), "b = 1\n", HashScriptSource ("b = 0\n"), &HashScriptSource);
    EXPECT_FALSE (written.ok);
    EXPECT_TRUE (written.conflict);
    EXPECT_TRUE (written.diskSource.empty ());
    EXPECT_FALSE (std::filesystem::exists (path, code));
}

TEST (ScriptWrite, RefusesAPathThatIsNotAScript)
{
    const ScriptWrite written =
        WriteScriptSource ("C:\\somewhere\\notes.txt", "b = 1\n", std::string {}, &HashScriptSource);
    EXPECT_FALSE (written.ok);
    EXPECT_FALSE (written.conflict);
    EXPECT_NE (written.error.find (".js"), std::string::npos);
}

TEST (ScriptWrite, LeavesNoTemporaryBesideTheFileItWrote)
{
    // The write goes through a sibling temporary and a rename, which is what
    // keeps a failure from leaving a half-written script behind. A temporary that
    // survived a SUCCESSFUL write would sit in the user's script folder forever,
    // and on a folder an external editor watches it is also a phantom file.
    const TempScript file ("tapioca_script_temp.py", "b = 1\n");
    const ScriptWrite written =
        WriteScriptSource (file.Path (), "b = 2\n", HashScriptSource ("b = 1\n"), &HashScriptSource);
    ASSERT_TRUE (written.ok) << written.error;
    std::error_code code;
    EXPECT_FALSE (std::filesystem::exists (file.Path () + ".tapioca-save", code));
}

TEST (ScriptWrite, TheTextItWroteIsWhatTheNodeThenLoads)
{
    // The round trip that matters end to end: a save reshapes the node, so the
    // header the editor wrote has to be the header the manifest parser sees.
    const TempScript file ("tapioca_script_roundtrip.py", "# @out b : number\nb = 1\n");
    const std::string base = HashScriptSource (ReadScript (file.Path ()).source);
    const std::string edited = "# @in  a : number\n# @out b : number\nb = a * 2\n";
    ASSERT_TRUE (WriteScriptSource (file.Path (), edited, base, &HashScriptSource).ok);

    const ScriptState state = LoadScriptState (file.Path (), ScriptLanguage::Python);
    EXPECT_TRUE (state.loadError.empty ()) << state.loadError;
    ASSERT_EQ (state.manifest.inputs.size (), 1u);
    EXPECT_EQ (state.manifest.inputs.front ().id, "a");
    EXPECT_EQ (state.sourceHash, HashScriptSource (edited));
}

TEST (ScriptSource, ASizeStampCatchesASaveThatKeptTheSameTimestamp)
{
    // Windows filesystem timestamps are coarse enough that a fast edit-save-edit
    // cycle can produce two different files with the same mtime. A node that
    // trusted mtime alone would go on running the previous version while the
    // editor showed the new one.
    ScriptStamp first;
    first.exists = true;
    first.modifiedUnixMs = 1000;
    first.sizeBytes = 40;
    ScriptStamp second = first;
    second.sizeBytes = 41;
    EXPECT_NE (first, second);
}

// ---------------------------------------------------------------------------
// Running it.

TEST (ScriptRuntimeJs, RunsAndReadsBackTheDeclaredOutputs)
{
    const ScriptRunResult result =
        RunJs ("area = radius * 2;", { { "radius", Value (3.0) } }, { Out ("area", ValueType::Double) });
    ASSERT_TRUE (result.ok) << result.error;
    EXPECT_EQ (std::get<double> (result.outputs.at ("area").DataValue ()), 6.0);
}

TEST (ScriptRuntimeJs, NamesTheOutputTheScriptForgotToSet)
{
    // "The script did not set 'area'" is something the author fixes by looking at
    // one line. "Nothing happened" is not.
    const ScriptRunResult result = RunJs ("var other = 1;", {}, { Out ("area", ValueType::Double) });
    EXPECT_FALSE (result.ok);
    EXPECT_NE (result.error.find ("area"), std::string::npos);
}

TEST (ScriptRuntimeJs, AThrownErrorComesBackAsAMessageRatherThanAnException)
{
    const ScriptRunResult result = RunJs ("throw new Error('bad radius');", {}, { Out ("a", ValueType::Double) });
    EXPECT_FALSE (result.ok);
    EXPECT_NE (result.error.find ("bad radius"), std::string::npos);
}

TEST (ScriptRuntimeJs, ASyntaxErrorFailsTheNodeRatherThanTheProcess)
{
    const ScriptRunResult result = RunJs ("this is not javascript", {}, { Out ("a", ValueType::Double) });
    EXPECT_FALSE (result.ok);
    EXPECT_FALSE (result.error.empty ());
}

TEST (ScriptRuntimeJs, AnInfiniteLoopIsStoppedByItsTimeBudget)
{
    // ⚠️ THE CONTAINMENT TEST. A script node is the one body in the catalog
    // written by whoever is sitting at the machine, so it is the one that will
    // actually contain `while (true)`. Enforced from inside the engine, because a
    // runaway script is not blocked on anything a watchdog thread could interrupt.
    const auto started = std::chrono::steady_clock::now ();
    const ScriptRunResult result = RunJs ("while (true) {}", {}, { Out ("a", ValueType::Double) }, 250.0);
    const auto elapsed = std::chrono::steady_clock::now () - started;
    EXPECT_FALSE (result.ok);
    EXPECT_NE (result.error.find ("time budget"), std::string::npos);
    EXPECT_LT (std::chrono::duration_cast<std::chrono::seconds> (elapsed).count (), 5);
}

TEST (ScriptRuntimeJs, CapturesWhatTheScriptPrinted)
{
    // A script node runs on a worker thread inside Archicad with no console
    // attached. Without this, printf debugging silently does not work.
    const ScriptRunResult result = RunJs ("console.log('r =', 3); a = 1;", {}, { Out ("a", ValueType::Double) });
    ASSERT_TRUE (result.ok) << result.error;
    ASSERT_FALSE (result.log.empty ());
    EXPECT_NE (result.log.front ().find ("r = 3"), std::string::npos);
}

TEST (ScriptRuntimeJs, MarshalsPointsAndListsBothWays)
{
    const ScriptRunResult result = RunJs (
        "out = { x: p.x + 1, y: p.y, z: p.z }; n = items.length;",
        { { "p", Value (Point3 { 1.0, 2.0, 3.0 }) }, { "items", Argument::FromItems ({ Value (1.0), Value (2.0) }) } },
        { Out ("out", ValueType::Point3), Out ("n", ValueType::Integer) });
    ASSERT_TRUE (result.ok) << result.error;
    EXPECT_EQ (std::get<Point3> (result.outputs.at ("out").DataValue ()).x, 2.0);
    EXPECT_EQ (std::get<int64_t> (result.outputs.at ("n").DataValue ()), 2);
}

TEST (ScriptRuntimeJs, RefusesToLetAScriptFabricateGeometryOrAnElement)
{
    // Reconstructing a mesh from whatever a script left in a variable is how a
    // graph acquires meshes with three vertices and no normals, failing far
    // downstream in the renderer. And an element is a REFERENCE to something in
    // the model, which a script must not be able to invent.
    const ScriptRunResult mesh = RunJs ("m = { isMesh: true };", {}, { Out ("m", ValueType::Mesh) });
    EXPECT_FALSE (mesh.ok);
    EXPECT_NE (mesh.error.find ("geometry nodes"), std::string::npos);

    const ScriptRunResult element = RunJs ("e = {};", {}, { Out ("e", ValueType::ArchicadElementRef) });
    EXPECT_FALSE (element.ok);
}

TEST (ScriptRuntimeJs, OneScriptCannotSeeAnotherScriptsGlobals)
{
    // A shared interpreter would make a node work until someone reordered the
    // graph, which is about the worst debugging experience available.
    ASSERT_TRUE (RunJs ("leaked = 42; a = 1;", {}, { Out ("a", ValueType::Double) }).ok);
    const ScriptRunResult second =
        RunJs ("a = typeof leaked === 'undefined' ? 0 : 1;", {}, { Out ("a", ValueType::Double) });
    ASSERT_TRUE (second.ok) << second.error;
    EXPECT_EQ (std::get<double> (second.outputs.at ("a").DataValue ()), 0.0);
}

TEST (ScriptRuntimeJs, AScriptCannotReachTheFilesystemOrTheNetwork)
{
    // The engine is created bare - no module loader, no host bindings - so this
    // is a check that nothing has quietly added one.
    const ScriptRunResult result = RunJs ("a = (typeof require === 'undefined' && typeof fetch === 'undefined' && "
                                          "typeof std === 'undefined' && typeof os === 'undefined') ? 1 : 0;",
                                          {}, { Out ("a", ValueType::Double) });
    ASSERT_TRUE (result.ok) << result.error;
    EXPECT_EQ (std::get<double> (result.outputs.at ("a").DataValue ()), 1.0);
}

// ---------------------------------------------------------------------------
// The node in a graph.

TEST (ScriptNodes, ATypeWithInstancePortsDeclaresNoneOfItsOwn)
{
    NodeRegistry registry;
    RegisterScriptNodes (registry);
    const NodeType* type = registry.Find (kPythonNodeType);
    ASSERT_NE (type, nullptr);
    EXPECT_TRUE (type->instancePorts);
    EXPECT_TRUE (type->inputs.empty ());
    EXPECT_TRUE (type->outputs.empty ());
    // A script sees its inputs and nothing else, so it is Pure and runs on a
    // worker - which is what lets several script nodes on one level run at once.
    EXPECT_EQ (type->effect, EffectKind::Pure);
    EXPECT_EQ (type->executionDomain, ExecutionDomain::Worker);
}

TEST (ScriptNodes, ATypeCannotDeclareBothInstancePortsAndStaticOnes)
{
    NodeRegistry registry;
    NodeType type;
    type.id = "confused";
    type.label = "Confused";
    type.instancePorts = true;
    type.outputs.push_back (PortSchema { "value", "Value", ValueType::Double, true, false });
    std::string error;
    EXPECT_FALSE (registry.Register (std::move (type), error));
    EXPECT_NE (error.find ("instance ports"), std::string::npos);
}

TEST (ScriptNodes, PortsResolveFromTheNodeForAScriptTypeAndFromTheTypeForEverythingElse)
{
    NodeRegistry registry = MakeRuntimeNodeRegistry ();

    Node script { "s1", kJavaScriptNodeType };
    script.dynamicInputs.push_back (PortSchema { "a", "A", ValueType::Double, false, false });
    EXPECT_EQ (ResolvedInputs (script, *registry.Find (kJavaScriptNodeType)).size (), 1u);

    // ⚠️ AND THE OTHER DIRECTION: a hand-edited document must not be able to bolt
    // ports onto a type that never agreed to them.
    Node add { "a1", "add" };
    add.dynamicInputs.push_back (PortSchema { "sneaky", "Sneaky", ValueType::Double, false, false });
    EXPECT_EQ (ResolvedInputs (add, *registry.Find ("add")).size (), 2u);
}

TEST (ScriptNodes, ReshapingANodeDropsOnlyTheEdgesThatNoLongerFit)
{
    NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument document;
    std::string ignored;

    Node script { "s1", kJavaScriptNodeType };
    script.dynamicOutputs.push_back (PortSchema { "kept", "Kept", ValueType::Double, true, false });
    script.dynamicOutputs.push_back (PortSchema { "gone", "Gone", ValueType::Double, true, false });
    ASSERT_TRUE (ApplyEdit (document, registry, GraphEdit { AddNodeEdit { script } }).accepted);
    ASSERT_TRUE (ApplyEdit (document, registry, GraphEdit { AddNodeEdit { Node { "p1", "panel" } } }).accepted);
    ASSERT_TRUE (ApplyEdit (document, registry, GraphEdit { AddNodeEdit { Node { "p2", "panel" } } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (document, registry, GraphEdit { ConnectEdit { Edge { "s1", "kept", "p1", "value" } } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (document, registry, GraphEdit { ConnectEdit { Edge { "s1", "gone", "p2", "value" } } }).accepted);

    SetScriptInterfaceEdit reshape;
    reshape.nodeId = "s1";
    reshape.outputs.push_back (PortSchema { "kept", "Kept", ValueType::Double, true, false });
    reshape.sourceHash = "abc123";
    const EditResult applied = ApplyEdit (document, registry, GraphEdit { reshape });

    ASSERT_TRUE (applied.accepted) << applied.error;
    ASSERT_EQ (applied.droppedEdges.size (), 1u);
    EXPECT_EQ (applied.droppedEdges.front ().sourcePort, "gone");
    EXPECT_EQ (document.Edges ().size (), 1u);
    // The hash lands in the parameters, which is what puts it in the evaluator's
    // cache key - without it a saved file whose ports did not change would reload
    // into an identical node and be served the previous run's cached outputs.
    EXPECT_EQ (
        std::get<std::string> (document.FindNode ("s1")->parameters.at (kScriptSourceHashParameter).DataValue ()),
        "abc123");
}

TEST (ScriptNodes, AHeaderDefaultFillsAnEmptyPortAndNeverOverwritesTheUsersValue)
{
    NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument document;

    Node script { "s1", kJavaScriptNodeType };
    ASSERT_TRUE (ApplyEdit (document, registry, GraphEdit { AddNodeEdit { script } }).accepted);

    SetScriptInterfaceEdit first;
    first.nodeId = "s1";
    first.inputs.push_back (PortSchema { "radius", "Radius", ValueType::Double, false, false });
    first.outputs.push_back (PortSchema { "area", "Area", ValueType::Double, true, false });
    first.defaults.emplace ("radius", Value (1.0));
    ASSERT_TRUE (ApplyEdit (document, registry, GraphEdit { first }).accepted);
    EXPECT_EQ (std::get<double> (document.FindNode ("s1")->parameters.at ("radius").DataValue ()), 1.0);

    // The user types their own value in.
    ASSERT_TRUE (
        ApplyEdit (document, registry, GraphEdit { SetParameterEdit { "s1", "radius", Value (7.0) } }).accepted);

    // A reload must not reset it. A node that lost the user's number on every
    // save would be unusable while its script was being worked on.
    SetScriptInterfaceEdit second = first;
    second.sourceHash = "changed";
    ASSERT_TRUE (ApplyEdit (document, registry, GraphEdit { second }).accepted);
    EXPECT_EQ (std::get<double> (document.FindNode ("s1")->parameters.at ("radius").DataValue ()), 7.0);
}

TEST (ScriptNodes, AnInternalisedValueForAVanishedPortIsCleanedUp)
{
    // Left behind it would fail ValidateNode as an unknown parameter the next
    // time anything touched this node - long after the edit that orphaned it.
    NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument document;
    ASSERT_TRUE (
        ApplyEdit (document, registry, GraphEdit { AddNodeEdit { Node { "s1", kJavaScriptNodeType } } }).accepted);

    SetScriptInterfaceEdit first;
    first.nodeId = "s1";
    first.inputs.push_back (PortSchema { "old", "Old", ValueType::Double, false, false });
    first.outputs.push_back (PortSchema { "v", "V", ValueType::Double, true, false });
    first.defaults.emplace ("old", Value (2.0));
    ASSERT_TRUE (ApplyEdit (document, registry, GraphEdit { first }).accepted);

    SetScriptInterfaceEdit renamed;
    renamed.nodeId = "s1";
    renamed.inputs.push_back (PortSchema { "renamed", "Renamed", ValueType::Double, false, false });
    renamed.outputs = first.outputs;
    const EditResult applied = ApplyEdit (document, registry, GraphEdit { renamed });

    ASSERT_TRUE (applied.accepted) << applied.error;
    EXPECT_FALSE (document.FindNode ("s1")->parameters.contains ("old"));
    // The three the type itself declares are never swept away with them.
    EXPECT_TRUE (document.FindNode ("s1")->parameters.contains (kScriptSourceHashParameter));
}

TEST (ScriptNodes, ReshapingRefusesANodeThatDoesNotAuthorItsOwnPorts)
{
    NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument document;
    ASSERT_TRUE (ApplyEdit (document, registry, GraphEdit { AddNodeEdit { Node { "a1", "add" } } }).accepted);

    SetScriptInterfaceEdit edit;
    edit.nodeId = "a1";
    const EditResult applied = ApplyEdit (document, registry, GraphEdit { edit });
    EXPECT_FALSE (applied.accepted);
    EXPECT_EQ (applied.code, "script.notAScriptNode");
}

TEST (ScriptNodes, ANodeWithNoFileFailsWithTheReasonItHasNone)
{
    NodeRegistry registry = MakeRuntimeNodeRegistry ();
    Node node { "s1", kJavaScriptNodeType };
    ValueMap outputs;
    std::string error;
    NodeExecutionContext context;
    EXPECT_FALSE (ExecuteScriptNode (node, {}, context, outputs, error));
    EXPECT_NE (error.find ("no file"), std::string::npos);
}

TEST (ScriptNodes, APythonNodeSaysTheRuntimeIsAbsentRatherThanCrashing)
{
    // A supported state, not a failure: Python's runtime is absent in the offline
    // suite and in an add-on whose CPython did not resolve.
    ScriptState state;
    state.path = "x.py";
    state.language = ScriptLanguage::Python;
    state.source = "b = 1\n";
    state.manifest = ParsePython ("# @out b : number\n");
    ScriptStore::Get ().SetState ("py1", state);

    NodeRegistry registry = MakeRuntimeNodeRegistry ();
    Node node { "py1", kPythonNodeType };
    ValueMap outputs;
    std::string error;
    NodeExecutionContext context;
    if (ActiveScriptRuntime (ScriptLanguage::Python) == nullptr) {
        EXPECT_FALSE (ExecuteScriptNode (node, {}, context, outputs, error));
        EXPECT_NE (error.find ("not available"), std::string::npos);
    }
    ScriptStore::Get ().Forget ("py1");
}

TEST (ScriptNodes, InstancePortsSurviveASaveAndLoad)
{
    // ⚠️ THEY MUST PERSIST, AND NOT BECAUSE THEY ARE EXPENSIVE TO RECOVER: the
    // EDGES in the file are validated against them on load. A graph that
    // re-derived a script node's ports by reading the script would silently drop
    // every wire whenever the file was missing or had been edited since the save.
    NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument document;
    Node script { "s1", kJavaScriptNodeType };
    script.dynamicInputs.push_back (PortSchema { "a", "A", ValueType::Double, false, false });
    script.dynamicOutputs.push_back (PortSchema { "b", "B", ValueType::Polygon, true, false });
    ASSERT_TRUE (ApplyEdit (document, registry, GraphEdit { AddNodeEdit { script } }).accepted);

    const SerializeResult written = SerializeGraph (document, GraphMetadata {}, 2);
    ASSERT_TRUE (written.error.empty ()) << written.error;
    const DeserializeResult read = DeserializeGraph (written.text, registry);
    ASSERT_TRUE (read.error.empty ()) << read.error;

    const Node* restored = read.graph.document.FindNode ("s1");
    ASSERT_NE (restored, nullptr);
    ASSERT_EQ (restored->dynamicInputs.size (), 1u);
    EXPECT_EQ (restored->dynamicOutputs.at (0).valueType, ValueType::Polygon);
}

// ---------------------------------------------------------------------------
// The two engines agree about what a value looks like.

TEST (ScriptValueShape, TheJavaScriptEngineAndTheSharedProjectionAgree)
{
    // ⚠️ THE DRIFT TEST, AND THE REASON ScriptValueJson.hpp CAN SAY "one shape,
    // two implementations" HONESTLY. The Python bridge builds the script-facing
    // shape through ScriptValueToJson; the JavaScript engine builds it directly
    // against QuickJS, because serialising to text only to parse it again would
    // be pure cost. If those two ever disagree, a script that works in one
    // language stops working when translated to the other - which is the one
    // thing this node family promises will not happen.
    //
    // The comparison runs the value through the JS engine and asks JavaScript
    // itself to stringify what it received, then compares against the shared
    // projection's own text.
    struct Sample {
        const char* name;
        Argument value;
    };
    const std::vector<Sample> samples = {
        { "bool", Value (true) },
        { "integer", Value (static_cast<int64_t> (7)) },
        { "number", Value (2.5) },
        { "text", Value (std::string ("wall")) },
        { "point", Value (Point3 { 1.0, 2.0, 3.0 }) },
        { "polyline", Value (Polyline { { Point3 { 0.0, 0.0, 0.0 }, Point3 { 1.0, 0.0, 0.0 } } }) },
        { "element", Value (ArchicadElementRef { "ABC-123" }) },
        { "list", Argument::FromItems ({ Value (1.0), Value (2.0) }) },
    };

    for (const Sample& sample : samples) {
        // JSON.stringify orders an object's keys by insertion; the projection's
        // writer orders them by key. Sorting both sides' text makes the
        // comparison about CONTENT rather than about key order, which is the part
        // that would actually break a script.
        const ScriptRunResult produced =
            RunJs ("seen = JSON.stringify(v);", { { "v", sample.value } }, { Out ("seen", ValueType::String) });
        ASSERT_TRUE (produced.ok) << sample.name << ": " << produced.error;

        std::string fromJs = std::get<std::string> (produced.outputs.at ("seen").DataValue ());
        std::string shared = json::Write (ScriptValueToJson (sample.value), 0);
        std::sort (fromJs.begin (), fromJs.end ());
        std::sort (shared.begin (), shared.end ());
        EXPECT_EQ (fromJs, shared) << "the two engines disagree about a " << sample.name;
    }
}

TEST (ScriptValueShape, APointMayOmitZ)
{
    // A script working in plan naturally writes { x, y } and means z = 0.
    // Refusing that would make every 2D script carry a component it does not
    // care about.
    const json::ParseResult parsed = json::Parse ("{\"x\":1,\"y\":2}");
    ASSERT_TRUE (parsed.ok);
    Value value;
    std::string error;
    ASSERT_TRUE (ScriptValueFromJson (parsed.value, ValueType::Point3, value, error)) << error;
    EXPECT_EQ (std::get<Point3> (value.DataValue ()).z, 0.0);
}

TEST (ScriptValueShape, AnIntegerPortTakesAWholeNumberAndRefusesAFraction)
{
    // Python is dynamically typed and will happily leave 4.0 - or 4.5 - in a
    // variable a header called an integer. The first is what the author meant;
    // truncating the second silently would be the wrong kind of helpful.
    Value value;
    std::string error;
    const json::ParseResult whole = json::Parse ("4.0");
    ASSERT_TRUE (ScriptValueFromJson (whole.value, ValueType::Integer, value, error)) << error;
    EXPECT_EQ (std::get<int64_t> (value.DataValue ()), 4);

    const json::ParseResult fraction = json::Parse ("4.5");
    EXPECT_FALSE (ScriptValueFromJson (fraction.value, ValueType::Integer, value, error));
}

TEST (ScriptValueShape, NeitherDirectionWillCarryAMesh)
{
    const json::ParseResult parsed = json::Parse ("{\"isMesh\":true}");
    Value value;
    std::string error;
    EXPECT_FALSE (ScriptValueFromJson (parsed.value, ValueType::Mesh, value, error));
    EXPECT_NE (error.find ("geometry nodes"), std::string::npos);
}

// ---------------------------------------------------------------------------
// The SHIPPED EXAMPLES, end to end.
//
// ⚠️ THESE RUN AGAINST AddOn/EvP/GraphScripts, NOT A PRIVATE COPY. Those files
// are what a user points a node at first, so their headers are the contract this
// feature makes - and an example whose header stopped parsing has stopped being
// an example. Everything above tests the machinery on synthetic input; this tests
// that the thing we actually hand people works.
//
// The JavaScript examples are EXECUTED here. The Python ones can only be parsed
// offline - CPython is loaded by the add-on, not by this binary - so their
// headers are checked and their bodies are covered by running them in Archicad.

namespace {

std::string ExamplePath (const char* name)
{
#ifdef EVP_TEST_SCRIPT_DIR
    return std::string (EVP_TEST_SCRIPT_DIR) + "/" + name;
#else
    (void) name;
    return {};
#endif
}

ScriptState LoadExample (const char* name)
{
    ScriptLanguage language = ScriptLanguage::JavaScript;
    ScriptLanguageFromPath (name, language);
    return LoadScriptState (ExamplePath (name), language);
}

} // namespace

TEST (ScriptExamples, EveryShippedExampleParsesAndDeclaresPorts)
{
    for (const char* name : { "01-hello.py", "02-hello.js", "03-every-type.py", "04-ports-change.py",
                              "05-output-and-errors.py", "06-geometry.js" }) {
        const ScriptState state = LoadExample (name);
        if (!state.loadError.empty ())
            GTEST_SKIP () << "the shipped examples are not present: " << state.loadError;
        EXPECT_TRUE (state.manifest.Ok ())
            << name << ": " << (state.manifest.diagnostics.empty () ? "" : state.manifest.diagnostics.front ().message);
        EXPECT_FALSE (state.manifest.outputs.empty ()) << name << " declares no outputs";
        EXPECT_FALSE (state.manifest.name.empty ()) << name << " declares no @name";
    }
}

TEST (ScriptExamples, TheHelloExampleDeclaresTheInputAndOutputItAdvertises)
{
    const ScriptState state = LoadExample ("01-hello.py");
    if (!state.loadError.empty ())
        GTEST_SKIP () << state.loadError;

    ASSERT_EQ (state.manifest.inputs.size (), 1u);
    EXPECT_EQ (state.manifest.inputs[0].id, "value");
    EXPECT_EQ (state.manifest.inputs[0].label, "Value");
    EXPECT_EQ (state.manifest.inputs[0].valueType, ValueType::Double);
    // A default makes it optional: the node works the moment it is placed.
    EXPECT_FALSE (state.manifest.inputs[0].required);
    ASSERT_TRUE (state.manifest.defaults.contains ("value"));
    EXPECT_EQ (std::get<double> (state.manifest.defaults.at ("value").DataValue ()), 2.0);

    ASSERT_EQ (state.manifest.outputs.size (), 1u);
    EXPECT_EQ (state.manifest.outputs[0].id, "doubled");
    EXPECT_EQ (state.manifest.outputs[0].label, "Doubled");
}

TEST (ScriptExamples, TheTwoHelloExamplesDeclareIdenticalInterfaces)
{
    // ⚠️ THE PROMISE THE WHOLE FAMILY RESTS ON: the two languages differ in the
    // syntax of the file and in nothing else. The pair exists in the examples
    // folder precisely so someone can wire the same number into both and see the
    // same answer; this asserts they at least agree about their ports.
    const ScriptState python = LoadExample ("01-hello.py");
    const ScriptState javascript = LoadExample ("02-hello.js");
    if (!python.loadError.empty () || !javascript.loadError.empty ())
        GTEST_SKIP () << "the shipped examples are not present";

    ASSERT_EQ (python.manifest.inputs.size (), javascript.manifest.inputs.size ());
    for (size_t index = 0; index < python.manifest.inputs.size (); ++index) {
        EXPECT_EQ (python.manifest.inputs[index].id, javascript.manifest.inputs[index].id);
        EXPECT_EQ (python.manifest.inputs[index].valueType, javascript.manifest.inputs[index].valueType);
    }
    ASSERT_EQ (python.manifest.outputs.size (), javascript.manifest.outputs.size ());
    EXPECT_EQ (python.manifest.outputs[0].id, javascript.manifest.outputs[0].id);
    EXPECT_EQ (python.manifest.outputs[0].valueType, javascript.manifest.outputs[0].valueType);
}

TEST (ScriptExamples, TheEveryTypeExampleCoversTheWholeVocabulary)
{
    // Its whole job is to show every type at once, so a type quietly dropped from
    // the header grammar should fail HERE, on the file people read to learn what
    // is available.
    const ScriptState state = LoadExample ("03-every-type.py");
    if (!state.loadError.empty ())
        GTEST_SKIP () << state.loadError;
    ASSERT_TRUE (state.manifest.Ok ()) << state.manifest.diagnostics.front ().message;

    std::map<std::string, ValueType> inputs;
    for (const PortSchema& port : state.manifest.inputs)
        inputs.emplace (port.id, port.valueType);

    EXPECT_EQ (inputs.at ("flag"), ValueType::Bool);
    EXPECT_EQ (inputs.at ("count"), ValueType::Integer);
    EXPECT_EQ (inputs.at ("size"), ValueType::Double);
    EXPECT_EQ (inputs.at ("label"), ValueType::String);
    EXPECT_EQ (inputs.at ("origin"), ValueType::Point3);
    EXPECT_EQ (inputs.at ("numbers"), ValueType::List);

    // The defaults, including the two whose literal form is easy to get wrong.
    EXPECT_EQ (std::get<bool> (state.manifest.defaults.at ("flag").DataValue ()), true);
    EXPECT_EQ (std::get<int64_t> (state.manifest.defaults.at ("count").DataValue ()), 3);
    EXPECT_EQ (std::get<std::string> (state.manifest.defaults.at ("label").DataValue ()), "wall");
}

TEST (ScriptExamples, TheJavaScriptHelloExampleRunsAndProducesItsOutput)
{
    const ScriptState state = LoadExample ("02-hello.js");
    if (!state.loadError.empty ())
        GTEST_SKIP () << state.loadError;
    ASSERT_TRUE (state.manifest.Ok ());

    const ScriptRunResult result = RunJs (state.source, { { "value", Value (21.0) } }, state.manifest.outputs);
    ASSERT_TRUE (result.ok) << result.error;
    EXPECT_EQ (std::get<double> (result.outputs.at ("doubled").DataValue ()), 42.0);
}

TEST (ScriptExamples, TheGeometryExampleRoundTripsPointsAndAPolyline)
{
    const ScriptState state = LoadExample ("06-geometry.js");
    if (!state.loadError.empty ())
        GTEST_SKIP () << state.loadError;
    ASSERT_TRUE (state.manifest.Ok ()) << state.manifest.diagnostics.front ().message;

    ValueMap inputs;
    inputs.emplace ("center", Value (Point3 { 10.0, 0.0, 2.0 }));
    inputs.emplace ("radius", Value (5.0));
    inputs.emplace ("segments", Value (static_cast<int64_t> (12)));

    const ScriptRunResult result = RunJs (state.source, inputs, state.manifest.outputs);
    ASSERT_TRUE (result.ok) << result.error;

    // A polyline came back as a polyline, with the count the script was asked for.
    const Polyline& ring = std::get<Polyline> (result.outputs.at ("ring").DataValue ());
    EXPECT_EQ (ring.points.size (), 12u);
    // Angle zero, so the first point sits at centre + radius on x, and carries the
    // centre's z - which is the half a 2D-only marshaller would quietly drop.
    const Point3& first = std::get<Point3> (result.outputs.at ("first").DataValue ());
    EXPECT_NEAR (first.x, 15.0, 1e-9);
    EXPECT_NEAR (first.y, 0.0, 1e-9);
    EXPECT_NEAR (first.z, 2.0, 1e-9);
    EXPECT_NEAR (std::get<double> (result.outputs.at ("span").DataValue ()), 2.0 * 3.14159265358979323846 * 5.0, 1e-9);
}

TEST (ScriptExamples, APlacedNodeTakesItsPortsAndDefaultsFromTheFile)
{
    // ⚠️ THE END-TO-END PATH, and the one a user actually performs: place a node,
    // point it at a file, press Reload, and watch it grow the ports the file
    // declares. It goes through ReloadScriptNode - the same call the button and
    // the file watcher make - so what is covered here is the whole chain rather
    // than the parser alone.
    const std::string path = ExamplePath ("01-hello.py");
    if (!StatScript (path).exists)
        GTEST_SKIP () << "the shipped examples are not present";

    const GraphId graphId = "scriptExampleGraph";
    GraphRuntimeState& runtime = GraphRuntimeState::Get ();

    Node node { "s1", kPythonNodeType };
    ASSERT_TRUE (runtime.Apply (graphId, GraphEdit { AddNodeEdit { node } }).accepted);
    // The node starts portless: nothing has read a file yet.
    EXPECT_TRUE (runtime.Document (graphId).Nodes ().at ("s1").dynamicOutputs.empty ());

    ASSERT_TRUE (
        runtime.Apply (graphId, GraphEdit { SetParameterEdit { "s1", kScriptPathParameter, Value (path) } }).accepted);

    const ScriptReloadResult reloaded = ReloadScriptNode (graphId, "s1");
    ASSERT_TRUE (reloaded.ok) << reloaded.error;
    EXPECT_TRUE (reloaded.interfaceChanged);

    // â ï¸ Document() RETURNS BY VALUE, so the document must be held in a named
    // local. `runtime.Document (graphId).FindNode (...)` hands back a pointer into
    // a temporary that dies at the end of the statement - which does not crash,
    // it quietly reads freed memory and reports a node with no ports.
    const GraphDocument document = runtime.Document (graphId);
    const Node* loaded = document.FindNode ("s1");
    ASSERT_NE (loaded, nullptr);
    ASSERT_EQ (loaded->dynamicInputs.size (), 1u);
    EXPECT_EQ (loaded->dynamicInputs[0].id, "value");
    ASSERT_EQ (loaded->dynamicOutputs.size (), 1u);
    EXPECT_EQ (loaded->dynamicOutputs[0].id, "doubled");

    // The header's default landed as the input's internalised value, so the node
    // has something to compute with before anything is wired to it.
    ASSERT_TRUE (loaded->parameters.contains ("value"));
    EXPECT_EQ (std::get<double> (loaded->parameters.at ("value").DataValue ()), 2.0);
    // And the source hash reached the parameters, which is what puts the file's
    // content into the evaluator's cache key.
    EXPECT_FALSE (std::get<std::string> (loaded->parameters.at (kScriptSourceHashParameter).DataValue ()).empty ());

    // Reloading an unchanged file is not a reshape. The editor animates a reshape
    // and says nothing about an ordinary save, so this distinction is visible.
    const ScriptReloadResult again = ReloadScriptNode (graphId, "s1");
    ASSERT_TRUE (again.ok) << again.error;
    EXPECT_FALSE (again.interfaceChanged);
    EXPECT_TRUE (again.droppedEdges.empty ());

    ASSERT_TRUE (runtime.Apply (graphId, GraphEdit { RemoveNodeEdit { "s1" } }).accepted);
    ScriptStore::Get ().Forget ("s1");
}

TEST (ScriptExamples, AJavaScriptNodeEvaluatesThroughTheRuntimeExecutor)
{
    // The same path, but taken all the way to a RESULT: the node executor reads
    // the store, runs the engine, and hands back a value the evaluator would
    // publish. This is "does the node actually run a script", rather than "does
    // the engine work".
    const std::string path = ExamplePath ("02-hello.js");
    if (!StatScript (path).exists)
        GTEST_SKIP () << "the shipped examples are not present";

    const GraphId graphId = "scriptExampleGraph2";
    GraphRuntimeState& runtime = GraphRuntimeState::Get ();
    ASSERT_TRUE (runtime.Apply (graphId, GraphEdit { AddNodeEdit { Node { "js1", kJavaScriptNodeType } } }).accepted);
    ASSERT_TRUE (
        runtime.Apply (graphId, GraphEdit { SetParameterEdit { "js1", kScriptPathParameter, Value (path) } }).accepted);
    ASSERT_TRUE (ReloadScriptNode (graphId, "js1").ok);

    const GraphDocument document = runtime.Document (graphId);
    const Node* node = document.FindNode ("js1");
    ASSERT_NE (node, nullptr);

    ValueMap inputs;
    inputs.emplace ("value", Value (4.0));
    ValueMap outputs;
    std::string error;
    NodeExecutionContext context;
    ASSERT_TRUE (ExecuteScriptNode (*node, inputs, context, outputs, error)) << error;
    EXPECT_EQ (std::get<double> (outputs.at ("doubled").DataValue ()), 8.0);

    ASSERT_TRUE (runtime.Apply (graphId, GraphEdit { RemoveNodeEdit { "js1" } }).accepted);
    ScriptStore::Get ().Forget ("js1");
}
