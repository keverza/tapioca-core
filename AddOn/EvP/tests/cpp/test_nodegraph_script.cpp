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
#include "NodeGraph/ScriptIntelligence.hpp"
#include "NodeGraph/ScriptManifest.hpp"
#include "NodeGraph/ScriptNodes.hpp"
#include "NodeGraph/ScriptRuntime.hpp"
#include "NodeGraph/ScriptReload.hpp"
#include "NodeGraph/ScriptSource.hpp"
#include "NodeGraph/ScriptValueJson.hpp"
#include "NodeGraph/ScriptWorkspace.hpp"

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

// A temporary node FOLDER that removes itself. The folder model's equivalent of
// TempScript, and the fixture most of the workspace tests are built on.
class TempWorkspace {
  public:
    explicit TempWorkspace (const char* name)
    {
        root_ = std::filesystem::temp_directory_path () / name;
        std::error_code code;
        std::filesystem::remove_all (root_, code);
        std::filesystem::create_directories (root_, code);
    }
    ~TempWorkspace ()
    {
        std::error_code code;
        std::filesystem::remove_all (root_, code);
    }

    void Write (const char* name, const std::string& contents) const
    {
        std::ofstream stream (root_ / name, std::ios::binary | std::ios::trunc);
        stream << contents;
    }

    std::string Path () const
    {
        return root_.string ();
    }

  private:
    std::filesystem::path root_;
};

} // namespace

// ---------------------------------------------------------------------------
// Code intelligence: the LSP client, without a language server.
//
// ⚠️ THE POINT OF THE FACTORY HOOK IS THIS SECTION. basedpyright is a 150 MB
// on-demand download with a bundled Node runtime; a suite that needed it would
// be a suite nobody could run. What is actually easy to get wrong here is not
// pyright, it is the CLIENT - a Content-Length counted in the wrong unit, a
// header split across two pipe reads, a document version that repeats, a
// Windows path pasted into a URI - and every one of those is exercised below
// against a fake server that answers from a script.

namespace {

// A stand-in language server: it records what was written to it and replies to
// whatever it is told to reply to.
class FakeLanguageServer : public ILanguageServerProcess {
  public:
    bool Running () const override
    {
        return running;
    }

    bool Write (const std::string& bytes) override
    {
        written += bytes;
        // Answer every request the moment it arrives, the way a real server
        // eventually would. Notifications carry no id and get no reply.
        std::string body;
        std::string pending = written;
        written.clear ();
        std::string leftover = pending;
        while (TakeLspMessage (leftover, body)) {
            requests.push_back (body);
            const json::ParseResult parsed = json::Parse (body);
            if (!parsed.ok)
                continue;
            const json::JsonValue* id = parsed.value.Find ("id");
            int64_t number = 0;
            if (id == nullptr || !id->AsInteger (number))
                continue;
            const json::JsonValue* method = parsed.value.Find ("method");
            std::string name;
            if (method != nullptr)
                method->AsString (name);
            Reply (number, name);
        }
        return true;
    }

    void Read (std::string& into) override
    {
        into += outbox;
        outbox.clear ();
    }

    void Stop () override
    {
        running = false;
    }

    // What the client sent, in order, as raw JSON bodies.
    std::vector<std::string> requests;
    // The completion result the next completion request will be answered with.
    std::string completionJson = R"({"isIncomplete":false,"items":[]})";
    bool running = true;
    // When set, replies are split across two reads, so the client has to cope
    // with a message arriving in pieces.
    bool dribble = false;

  private:
    void Reply (int64_t id, const std::string& method)
    {
        std::string result = "null";
        if (method == "initialize")
            result = R"({"capabilities":{"completionProvider":{"triggerCharacters":["."]}}})";
        else if (method == "textDocument/completion")
            result = completionJson;
        const std::string body = R"({"jsonrpc":"2.0","id":)" + std::to_string (id) + R"(,"result":)" + result + "}";
        outbox += FrameLspMessage (body);
    }

    std::string written;
    std::string outbox;
};

// Finds the one request the client sent for `method`, as parsed JSON.
json::JsonValue RequestFor (const FakeLanguageServer& server, const std::string& method)
{
    for (const std::string& body : server.requests) {
        const json::ParseResult parsed = json::Parse (body);
        if (!parsed.ok)
            continue;
        const json::JsonValue* name = parsed.value.Find ("method");
        std::string text;
        if (name != nullptr && name->AsString (text) && text == method)
            return parsed.value;
    }
    return json::JsonValue {};
}

// Installs `server` as the one process the client may start, and takes it away
// again - a factory left behind would leak into every later test in the binary.
class WithFakeServer {
  public:
    explicit WithFakeServer (FakeLanguageServer* server)
    {
        SetLanguageServerProcessFactory (
            [server] (const std::string&, const std::vector<std::string>&) -> std::unique_ptr<ILanguageServerProcess> {
                return std::unique_ptr<ILanguageServerProcess> (new Borrowed (server));
            });
    }
    ~WithFakeServer ()
    {
        ScriptIntelligence::Get ().Shutdown ();
        SetLanguageServerProcessFactory (nullptr);
    }

  private:
    // The client OWNS the process it starts, and the test wants to keep looking
    // at the fake afterwards - so what it is handed is a non-owning shim.
    class Borrowed : public ILanguageServerProcess {
      public:
        explicit Borrowed (FakeLanguageServer* target) : target_ (target)
        {
        }
        bool Running () const override
        {
            return target_->Running ();
        }
        bool Write (const std::string& bytes) override
        {
            return target_->Write (bytes);
        }
        void Read (std::string& into) override
        {
            target_->Read (into);
        }
        void Stop () override
        {
            target_->Stop ();
        }

      private:
        FakeLanguageServer* target_;
    };
};

} // namespace

TEST (ScriptIntelligenceFraming, ALengthIsCountedInBytesAndNotInCharacters)
{
    // ⚠️ THE FAILURE THIS PINS IS PERMANENT, NOT INTERMITTENT. A length counted
    // in anything but bytes desynchronises the stream forever - every later
    // message is read at the wrong offset - so it presents as a language server
    // that worked until somebody typed an accented character in a comment.
    // The bytes spelled out rather than a u8 literal: in C++20 that is a
    // char8_t array and will not construct a std::string, and the point here is
    // the BYTES anyway - this is a two-byte character, one UTF-16 unit.
    const std::string body = "{\"text\":\"caf"
                             "\xC3\xA9"
                             "\"}";
    const std::string framed = FrameLspMessage (body);
    EXPECT_NE (framed.find ("Content-Length: " + std::to_string (body.size ())), std::string::npos);

    std::string buffer = framed;
    std::string taken;
    ASSERT_TRUE (TakeLspMessage (buffer, taken));
    EXPECT_EQ (taken, body);
    EXPECT_TRUE (buffer.empty ());
}

TEST (ScriptIntelligenceFraming, APartialMessageIsLeftAloneUntilTheRestArrives)
{
    // The normal case, not an edge case: a pipe read returns whatever bytes have
    // arrived, which is routinely half a header or most of a body.
    const std::string framed = FrameLspMessage (R"({"id":1})");
    std::string buffer = framed.substr (0, 12);
    std::string body;
    EXPECT_FALSE (TakeLspMessage (buffer, body));
    EXPECT_EQ (buffer, framed.substr (0, 12)) << "a partial read must not consume the buffer";

    buffer = framed.substr (0, framed.size () - 3);
    EXPECT_FALSE (TakeLspMessage (buffer, body)) << "a complete header with a partial body is still partial";

    buffer = framed;
    EXPECT_TRUE (TakeLspMessage (buffer, body));
    EXPECT_EQ (body, R"({"id":1})");
}

TEST (ScriptIntelligenceFraming, TwoMessagesInOneReadAreTakenOneAtATime)
{
    std::string buffer = FrameLspMessage (R"({"id":1})") + FrameLspMessage (R"({"id":2})");
    std::string body;
    ASSERT_TRUE (TakeLspMessage (buffer, body));
    EXPECT_EQ (body, R"({"id":1})");
    ASSERT_TRUE (TakeLspMessage (buffer, body));
    EXPECT_EQ (body, R"({"id":2})");
    EXPECT_FALSE (TakeLspMessage (buffer, body));
}

TEST (ScriptIntelligenceFraming, AnUnknownHeaderIsSkippedAndTheContentTypeIsIgnored)
{
    // A server adding a header must not stop the stream.
    std::string buffer = "Content-Type: application/vscode-jsonrpc\r\nContent-Length: 8\r\n\r\n{\"id\":1}";
    std::string body;
    ASSERT_TRUE (TakeLspMessage (buffer, body));
    EXPECT_EQ (body, R"({"id":1})");
}

TEST (ScriptIntelligenceUri, AWindowsPathBecomesAFileUriTheServerWillRecognise)
{
    // ⚠️ THE LEADING SLASH IS NOT COSMETIC. Without it `file://C:/x` parses `C:`
    // as the URI's HOST, and the server answers about a document it does not
    // have - an empty completion list, and no clue why.
    EXPECT_EQ (PathToFileUri ("C:\\Tapioca\\Workflows\\apartment\\main.py"),
               "file:///C:/Tapioca/Workflows/apartment/main.py");

    // A space is the common case, on a folder under a user's own name.
    EXPECT_EQ (PathToFileUri ("C:\\My Scripts\\main.py"), "file:///C:/My%20Scripts/main.py");

    // And a non-ASCII folder is percent-encoded BYTE BY BYTE, which is what makes
    // the UTF-8 path survive.
    EXPECT_EQ (PathToFileUri ("C:\\caf"
                              "\xC3\xA9"
                              "\\main.py"),
               "file:///C:/caf%C3%A9/main.py");
}

TEST (ScriptIntelligenceCompletion, ReadsBothAListAndABareArray)
{
    // The protocol allows either, and servers use both. Assuming one is how a
    // client works against one server and returns nothing against the next.
    const json::ParseResult list =
        json::Parse (R"({"isIncomplete":false,"items":[{"label":"hypot","kind":3,"detail":"(x, y) -> float"}]})");
    ASSERT_TRUE (list.ok);
    const std::vector<ScriptCompletion> fromList = ParseCompletionResult (list.value);
    ASSERT_EQ (fromList.size (), 1u);
    EXPECT_EQ (fromList[0].label, "hypot");
    EXPECT_EQ (fromList[0].kind, "function");
    EXPECT_EQ (fromList[0].detail, "(x, y) -> float");
    // No insertText, so the label is what gets inserted.
    EXPECT_EQ (fromList[0].insertText, "hypot");

    const json::ParseResult array = json::Parse (R"([{"label":"pi","kind":21}])");
    ASSERT_TRUE (array.ok);
    const std::vector<ScriptCompletion> fromArray = ParseCompletionResult (array.value);
    ASSERT_EQ (fromArray.size (), 1u);
    EXPECT_EQ (fromArray[0].kind, "constant");
}

TEST (ScriptIntelligenceCompletion, InsertTextWinsOverTheLabelAndDocumentationIsOneLine)
{
    // ⚠️ A `json(` DELIMITER, NOT A BARE `R"(`. The label below ends in
    // `(points)"`, and the sequence `)"` closes a bare raw string right there -
    // which does not fail as a string error but as an unrelated syntax error
    // several tokens later.
    const json::ParseResult parsed = json::Parse (
        R"json({"items":[{"label":"polygon_area(points)","insertText":"polygon_area",)json"
        R"json("documentation":{"kind":"markdown","value":"The shoelace area.\nWinding order is not the caller's problem."}}]})json");
    ASSERT_TRUE (parsed.ok);
    const std::vector<ScriptCompletion> completions = ParseCompletionResult (parsed.value);
    ASSERT_EQ (completions.size (), 1u);
    // What is DRAWN and what is INSERTED are different, and a menu that inserted
    // the label would paste the signature into the user's script.
    EXPECT_EQ (completions[0].label, "polygon_area(points)");
    EXPECT_EQ (completions[0].insertText, "polygon_area");
    // A completion list is fifty items and numpy's docstrings are essays.
    EXPECT_EQ (completions[0].documentation, "The shoelace area.");
}

TEST (ScriptIntelligenceCompletion, AnItemWithNoLabelIsDroppedRatherThanDrawnBlank)
{
    const json::ParseResult parsed = json::Parse (R"({"items":[{"kind":3},{"label":""},{"label":"ok"}]})");
    ASSERT_TRUE (parsed.ok);
    const std::vector<ScriptCompletion> completions = ParseCompletionResult (parsed.value);
    ASSERT_EQ (completions.size (), 1u);
    EXPECT_EQ (completions[0].label, "ok");
}

TEST (ScriptIntelligenceCompletion, AnUnknownKindDrawsNoIconRatherThanTheWrongOne)
{
    const json::ParseResult parsed = json::Parse (R"({"items":[{"label":"x","kind":99}]})");
    ASSERT_TRUE (parsed.ok);
    EXPECT_TRUE (ParseCompletionResult (parsed.value)[0].kind.empty ());
    EXPECT_TRUE (CompletionKindWord (0).empty ());
    EXPECT_EQ (CompletionKindWord (9), "module");
}

TEST (ScriptIntelligenceClient, ConfiguresTheNodesOwnImportRootsOnEveryRequest)
{
    // ⚠️ THE FAILURE THIS PINS IS SILENT. One server serves every script node and
    // each has its own folder on its own path; configuring once at initialize
    // would mean the second node asked about resolved its imports against the
    // FIRST node's folder - and the only symptom is a completion list quietly
    // missing that node's helpers.
    const TempWorkspace node ("tapioca_lsp_roots");
    node.Write ("main.py", "import math\nout = math.hy\n");
    const ScriptWorkspace workspace = ResolveScriptWorkspace (node.Path (), ScriptLanguage::Python);

    FakeLanguageServer server;
    server.completionJson = R"({"items":[{"label":"hypot","kind":3}]})";
    const WithFakeServer installed (&server);

    std::string error;
    const std::vector<ScriptCompletion> completions =
        ScriptIntelligence::Get ().Complete (workspace, "main.py", "import math\nout = math.hy\n", 1, 13, error);
    ASSERT_TRUE (error.empty ()) << error;
    ASSERT_EQ (completions.size (), 1u);
    EXPECT_EQ (completions[0].label, "hypot");

    const json::JsonValue configuration = RequestFor (server, "workspace/didChangeConfiguration");
    const json::JsonValue* paths =
        configuration.Find ("params") == nullptr
            ? nullptr
            : configuration.Find ("params")->Find ("settings")->Find ("python")->Find ("analysis")->Find ("extraPaths");
    ASSERT_NE (paths, nullptr);
    const json::JsonArray* entries = paths->AsArray ();
    ASSERT_NE (entries, nullptr);
    ASSERT_FALSE (entries->empty ());
    std::string first;
    ASSERT_TRUE ((*entries)[0].AsString (first));
    // The node's OWN folder first, which is what lets it shadow a shared helper -
    // and it must be the same list the runtime puts on sys.path.
    EXPECT_EQ (first, workspace.importRoots.front ());
    EXPECT_EQ (entries->size (), workspace.importRoots.size ());
}

TEST (ScriptIntelligenceClient, TheSecondRequestChangesTheDocumentRatherThanOpeningItAgain)
{
    // ⚠️ VERSIONS MUST INCREASE AND NEVER REPEAT. A server that sees a version it
    // has already processed may ignore the change, which presents as completion
    // answering about the text as it was several keystrokes ago - the single most
    // confusing way this feature can fail.
    const TempWorkspace node ("tapioca_lsp_versions");
    const ScriptWorkspace workspace = ResolveScriptWorkspace (node.Path (), ScriptLanguage::Python);

    FakeLanguageServer server;
    const WithFakeServer installed (&server);

    std::string error;
    ScriptIntelligence::Get ().Complete (workspace, "main.py", "a = 1\n", 0, 5, error);
    ScriptIntelligence::Get ().Complete (workspace, "main.py", "a = 12\n", 0, 6, error);

    int opens = 0;
    std::vector<int64_t> versions;
    for (const std::string& body : server.requests) {
        const json::ParseResult parsed = json::Parse (body);
        if (!parsed.ok)
            continue;
        const json::JsonValue* method = parsed.value.Find ("method");
        std::string name;
        if (method == nullptr || !method->AsString (name))
            continue;
        if (name == "textDocument/didOpen")
            ++opens;
        if (name == "textDocument/didChange") {
            int64_t version = 0;
            parsed.value.Find ("params")->Find ("textDocument")->Find ("version")->AsInteger (version);
            versions.push_back (version);
        }
    }
    EXPECT_EQ (opens, 1) << "the same document must be opened once, then changed";
    ASSERT_EQ (versions.size (), 1u);
    EXPECT_EQ (versions[0], 2);
}

TEST (ScriptIntelligenceClient, TheEditorsBufferIsSentRatherThanTheFileOnDisk)
{
    // The whole reason completion is useful: it is asked for on text the user is
    // midway through typing and has NOT saved. A client that analysed the file on
    // disk would complete against the version before the edit being made.
    const TempWorkspace node ("tapioca_lsp_buffer");
    node.Write ("main.py", "# saved version\n");
    const ScriptWorkspace workspace = ResolveScriptWorkspace (node.Path (), ScriptLanguage::Python);

    FakeLanguageServer server;
    const WithFakeServer installed (&server);

    std::string error;
    ScriptIntelligence::Get ().Complete (workspace, "main.py", "# unsaved edit\n", 0, 14, error);

    const json::JsonValue opened = RequestFor (server, "textDocument/didOpen");
    std::string text;
    ASSERT_NE (opened.Find ("params"), nullptr);
    opened.Find ("params")->Find ("textDocument")->Find ("text")->AsString (text);
    EXPECT_EQ (text, "# unsaved edit\n");
}

TEST (ScriptIntelligenceClient, AServerThatWillNotStartIsAnEmptyListAndAReasonRatherThanAThrow)
{
    // ⚠️ A COMPLETION MENU THAT DOES NOT APPEAR IS A MUCH SMALLER FAILURE THAN AN
    // EDITOR THAT STOPS ACCEPTING KEYS. Nothing about code intelligence may take
    // a script node, a graph or Archicad down.
    const TempWorkspace node ("tapioca_lsp_absent");
    const ScriptWorkspace workspace = ResolveScriptWorkspace (node.Path (), ScriptLanguage::Python);

    SetLanguageServerProcessFactory (nullptr);
    ScriptIntelligence::Get ().Shutdown ();

    std::string error;
    const std::vector<ScriptCompletion> completions =
        ScriptIntelligence::Get ().Complete (workspace, "main.py", "x = 1\n", 0, 5, error);
    EXPECT_TRUE (completions.empty ());
    EXPECT_FALSE (error.empty ());
}

// ---------------------------------------------------------------------------
// The workspace: a node is a folder.
//
// ⚠️ THE REFUSALS MATTER MORE THAN THE RESOLUTIONS. ResolveWorkspaceFile is
// handed names that came from a browser inside Archicad, and it is the only
// thing between those names and the user's filesystem - there is no layer below
// it that knows the request came from a browser at all.

TEST (ScriptWorkspace, TheEntryFileIsMainInTheNodesOwnFolder)
{
    const TempWorkspace node ("tapioca_ws_entry");
    const ScriptWorkspace workspace = ResolveScriptWorkspace (node.Path (), ScriptLanguage::Python);
    ASSERT_TRUE (workspace.ok) << workspace.error;
    EXPECT_EQ (workspace.root, node.Path ());
    EXPECT_EQ (workspace.entryFile, (std::filesystem::path (node.Path ()) / "main.py").string ());
    // The node's own folder is FIRST, which is what lets a node deliberately
    // shadow a shared helper of the same name.
    ASSERT_FALSE (workspace.importRoots.empty ());
    EXPECT_EQ (workspace.importRoots.front (), node.Path ());
}

TEST (ScriptWorkspace, TheLanguageDecidesTheEntryFilesExtension)
{
    const TempWorkspace node ("tapioca_ws_lang");
    EXPECT_EQ (std::filesystem::path (ResolveScriptWorkspace (node.Path (), ScriptLanguage::JavaScript).entryFile)
                   .filename ()
                   .string (),
               "main.js");
    EXPECT_EQ (std::filesystem::path (ResolveScriptWorkspace (node.Path (), ScriptLanguage::Python).entryFile)
                   .filename ()
                   .string (),
               "main.py");
}

TEST (ScriptWorkspace, AFolderThatDoesNotExistYetStillResolves)
{
    // The state a node is in between being named and being scaffolded. Failing
    // here would make Create unable to use the path the user just typed.
    const std::string path = (std::filesystem::temp_directory_path () / "tapioca_ws_absent").string ();
    const ScriptWorkspace workspace = ResolveScriptWorkspace (path, ScriptLanguage::Python);
    EXPECT_TRUE (workspace.ok) << workspace.error;
    EXPECT_FALSE (StampWorkspace (workspace).exists);
}

TEST (ScriptWorkspace, AnEmptyPathIsRefusedRatherThanResolvedToTheLibraryRoot)
{
    // A node with no folder must not silently become a node pointing at the
    // whole workflow library, which is what an empty relative path would do.
    const ScriptWorkspace workspace = ResolveScriptWorkspace ("", ScriptLanguage::Python);
    EXPECT_FALSE (workspace.ok);
    EXPECT_NE (workspace.error.find ("no folder"), std::string::npos);
}

TEST (ScriptWorkspace, TheStampCoversHelpersAndNotOnlyTheEntryFile)
{
    // ⚠️ THE POINT OF THE FOLDER STAMP. A node's behaviour can change entirely
    // without main.py being touched - the edit was in calculations.py - and a
    // node that only watched its entry file would go on running the previous
    // helper while the editor showed the new one.
    const TempWorkspace node ("tapioca_ws_stamp");
    node.Write ("main.py", "# @out b : number\nb = 1\n");
    const ScriptWorkspace workspace = ResolveScriptWorkspace (node.Path (), ScriptLanguage::Python);

    const ScriptStamp before = StampWorkspace (workspace);
    ASSERT_TRUE (before.exists);
    node.Write ("calculations.py", "VALUE = 1\n");
    EXPECT_NE (StampWorkspace (workspace), before);
}

TEST (ScriptWorkspace, ADeletedHelperChangesTheStampToo)
{
    // A max-mtime-only stamp misses this: nothing that remains was touched.
    // Summing the sizes is what catches it.
    const TempWorkspace node ("tapioca_ws_deleted");
    node.Write ("main.py", "# @out b : number\nb = 1\n");
    node.Write ("helper.py", "VALUE = 1\n");
    const ScriptWorkspace workspace = ResolveScriptWorkspace (node.Path (), ScriptLanguage::Python);

    const ScriptStamp before = StampWorkspace (workspace);
    std::error_code code;
    std::filesystem::remove (std::filesystem::path (node.Path ()) / "helper.py", code);
    EXPECT_NE (StampWorkspace (workspace), before);
}

TEST (ScriptWorkspace, AFolderWithHelpersButNoEntryFileDoesNotExist)
{
    // Reporting it as present would send the user hunting for a syntax error in
    // a file that is not there.
    const TempWorkspace node ("tapioca_ws_noentry");
    node.Write ("calculations.py", "VALUE = 1\n");
    const ScriptWorkspace workspace = ResolveScriptWorkspace (node.Path (), ScriptLanguage::Python);
    EXPECT_FALSE (StampWorkspace (workspace).exists);
}

TEST (ScriptWorkspace, TheEntryFileIsTheFirstTabAndTheRestAreSorted)
{
    // Tab order that changed between two listings of the same folder would move
    // under the user's cursor.
    const TempWorkspace node ("tapioca_ws_tabs");
    node.Write ("zebra.py", "");
    node.Write ("main.py", "");
    node.Write ("alpha.py", "");
    const std::vector<WorkspaceFile> files =
        ListWorkspaceFiles (ResolveScriptWorkspace (node.Path (), ScriptLanguage::Python));
    ASSERT_GE (files.size (), 3u);
    EXPECT_EQ (files[0].name, "main.py");
    EXPECT_TRUE (files[0].entry);
    EXPECT_EQ (files[1].name, "alpha.py");
    EXPECT_EQ (files[2].name, "zebra.py");
}

TEST (ScriptWorkspace, ListingIgnoresFilesOfTheOtherLanguageAndNonScripts)
{
    const TempWorkspace node ("tapioca_ws_mixed");
    node.Write ("main.py", "");
    node.Write ("notes.md", "");
    node.Write ("data.json", "");
    node.Write ("other.js", "");
    const std::vector<WorkspaceFile> files =
        ListWorkspaceFiles (ResolveScriptWorkspace (node.Path (), ScriptLanguage::Python));
    for (const WorkspaceFile& file : files)
        EXPECT_NE (file.name.find (".py"), std::string::npos) << file.name;
}

TEST (ScriptWorkspace, AnEmptyNameMeansTheEntryFile)
{
    // The editor opens a node without knowing what its entry is called; making
    // it guess main.py versus main.js would put the language rule in the browser.
    const TempWorkspace node ("tapioca_ws_emptyname");
    const ScriptWorkspace workspace = ResolveScriptWorkspace (node.Path (), ScriptLanguage::Python);
    std::string absolute;
    std::string error;
    ASSERT_TRUE (ResolveWorkspaceFile (workspace, "", absolute, error)) << error;
    EXPECT_EQ (absolute, workspace.entryFile);
}

TEST (ScriptWorkspace, APlainHelperNameResolvesInsideTheFolder)
{
    const TempWorkspace node ("tapioca_ws_helper");
    const ScriptWorkspace workspace = ResolveScriptWorkspace (node.Path (), ScriptLanguage::Python);
    std::string absolute;
    std::string error;
    ASSERT_TRUE (ResolveWorkspaceFile (workspace, "calculations.py", absolute, error)) << error;
    EXPECT_EQ (absolute, (std::filesystem::path (node.Path ()) / "calculations.py").string ());
}

TEST (ScriptWorkspace, EveryWayOutOfTheFolderIsRefused)
{
    // ⚠️ THE SECURITY TEST. These names arrive from a browser. Each one below is
    // a way somebody would try to leave the node's folder, and every one of them
    // must be refused BY NAME rather than normalised into something plausible.
    const TempWorkspace node ("tapioca_ws_escape");
    const ScriptWorkspace workspace = ResolveScriptWorkspace (node.Path (), ScriptLanguage::Python);

    const char* const refused[] = {
        "..",
        "../secrets.py",
        "..\\secrets.py",
        "sub/nested.py",
        "sub\\nested.py",
        "C:\\Windows\\System32\\drivers\\etc\\hosts",
        "C:/evil.py",
        "\\\\server\\share\\evil.py",
        "notes.md",
        "main.js",
        ".",
        "....//evil.py",
    };
    for (const char* name : refused) {
        std::string absolute;
        std::string error;
        EXPECT_FALSE (ResolveWorkspaceFile (workspace, name, absolute, error)) << "accepted: " << name;
        EXPECT_TRUE (absolute.empty ()) << name;
        EXPECT_FALSE (error.empty ()) << name;
    }
}

TEST (ScriptWorkspace, ANewFileIsCreatedEmptyAndNeverOverwritesOne)
{
    const TempWorkspace node ("tapioca_ws_create");
    const ScriptWorkspace workspace = ResolveScriptWorkspace (node.Path (), ScriptLanguage::Python);
    std::string absolute;
    std::string error;
    ASSERT_TRUE (CreateWorkspaceFile (workspace, "helper.py", absolute, error)) << error;
    EXPECT_TRUE (std::filesystem::exists (absolute));

    // A new file is EMPTY: it is a blank helper, not a second copy of the
    // template, which would arrive carrying ports the user did not ask for.
    EXPECT_EQ (ReadScript (absolute).source, "");

    // Second time: refused, and what is already there is untouched.
    node.Write ("helper.py", "mine\n");
    std::string second;
    EXPECT_FALSE (CreateWorkspaceFile (workspace, "helper.py", second, error));
    EXPECT_NE (error.find ("already"), std::string::npos);
    EXPECT_EQ (ReadScript (absolute).source, "mine\n");
}

TEST (ScriptWorkspace, ANewFileCannotBeCreatedInTheSharedLibrary)
{
    // A helper created from inside one node's editor and silently landing on
    // every other node's import path is a surprise nobody wants twice.
    const TempWorkspace node ("tapioca_ws_sharedcreate");
    const ScriptWorkspace workspace = ResolveScriptWorkspace (node.Path (), ScriptLanguage::Python);
    std::string absolute;
    std::string error;
    EXPECT_FALSE (CreateWorkspaceFile (workspace, "libs/geometry.py", absolute, error));
    EXPECT_TRUE (absolute.empty ());
}

TEST (ScriptWorkspace, TheTemplateScaffoldsAFolderThatParsesAndDeclaresPorts)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path () / "tapioca_ws_template";
    std::error_code code;
    std::filesystem::remove_all (root, code);

    const ScriptWorkspace workspace = ResolveScriptWorkspace (root.string (), ScriptLanguage::Python);
    std::string error;
    ASSERT_TRUE (WriteWorkspaceTemplate (workspace, error)) << error;

    const ScriptRead read = ReadScript (workspace.entryFile);
    ASSERT_TRUE (read.ok) << read.error;
    EXPECT_TRUE (ParsePython (read.source).Ok ());
    // And it refuses a second time rather than overwriting what is now there.
    EXPECT_FALSE (WriteWorkspaceTemplate (workspace, error));
    std::filesystem::remove_all (root, code);
}

// ---------------------------------------------------------------------------
// Migration off the single-file model.
//
// ⚠️ THIS MOVES THE USER'S FILE, WHICH MAKES IT THE MOST DANGEROUS THING IN THE
// SCRIPT FAMILY. The tests below are almost all about the cases where it must
// REFUSE, because a refusal is recoverable and a wrong move is not.

TEST (ScriptMigrationTest, AFileBecomesAFolderNamedAfterIt)
{
    const TempScript file ("tapioca_migrate_one.py", "# @out b : number\nb = 1\n");
    const ScriptMigration migration = MigrateScriptFileToFolder (file.Path ());
    ASSERT_TRUE (migration.ok) << migration.error;
    EXPECT_TRUE (migration.migrated);

    const std::filesystem::path folder = migration.folder;
    EXPECT_EQ (folder.filename ().string (), "tapioca_migrate_one");
    EXPECT_TRUE (std::filesystem::exists (folder / "main.py"));
    // Moved, not copied: two files that can diverge is the failure this avoids.
    EXPECT_FALSE (std::filesystem::exists (file.Path ()));
    EXPECT_EQ (ReadScript ((folder / "main.py").string ()).source, "# @out b : number\nb = 1\n");

    std::error_code code;
    std::filesystem::remove_all (folder, code);
}

TEST (ScriptMigrationTest, AJavaScriptFileBecomesMainJs)
{
    const TempScript file ("tapioca_migrate_js.js", "// @out b : number\nb = 1;\n");
    const ScriptMigration migration = MigrateScriptFileToFolder (file.Path ());
    ASSERT_TRUE (migration.ok) << migration.error;
    EXPECT_TRUE (std::filesystem::exists (std::filesystem::path (migration.folder) / "main.js"));
    std::error_code code;
    std::filesystem::remove_all (migration.folder, code);
}

TEST (ScriptMigrationTest, AFolderIsLeftCompletelyAlone)
{
    // The common case after the first run, and it must cost one stat and change
    // nothing - a migration that ran every reload would be a rename loop.
    const TempWorkspace node ("tapioca_migrate_folder");
    node.Write ("main.py", "b = 1\n");
    const ScriptMigration migration = MigrateScriptFileToFolder (node.Path ());
    EXPECT_TRUE (migration.ok);
    EXPECT_FALSE (migration.migrated);
    EXPECT_EQ (migration.folder, node.Path ());
    EXPECT_TRUE (std::filesystem::exists (std::filesystem::path (node.Path ()) / "main.py"));
}

TEST (ScriptMigrationTest, APathThatIsNotThereIsNotAFailure)
{
    const ScriptMigration migration = MigrateScriptFileToFolder ("Z:\\definitely\\not\\here");
    EXPECT_TRUE (migration.ok);
    EXPECT_FALSE (migration.migrated);
}

TEST (ScriptMigrationTest, ItRefusesWhenTheDestinationEntryFileAlreadyExists)
{
    // Somebody already made the folder by hand. Overwriting their main.py to
    // complete an automatic conversion would destroy work nobody asked about.
    const TempScript file ("tapioca_migrate_clash.py", "mine\n");
    const std::filesystem::path folder = std::filesystem::temp_directory_path () / "tapioca_migrate_clash";
    std::error_code code;
    std::filesystem::create_directories (folder, code);
    {
        std::ofstream stream (folder / "main.py", std::ios::binary | std::ios::trunc);
        stream << "theirs\n";
    }

    const ScriptMigration migration = MigrateScriptFileToFolder (file.Path ());
    EXPECT_FALSE (migration.ok);
    // Neither file was touched.
    EXPECT_EQ (ReadScript (file.Path ()).source, "mine\n");
    EXPECT_EQ (ReadScript ((folder / "main.py").string ()).source, "theirs\n");
    std::filesystem::remove_all (folder, code);
}

TEST (ScriptMigrationTest, ItRefusesAFileThatIsNotAScript)
{
    const TempScript file ("tapioca_migrate_notes.txt", "notes\n");
    const ScriptMigration migration = MigrateScriptFileToFolder (file.Path ());
    EXPECT_FALSE (migration.ok);
    EXPECT_EQ (ReadScript (file.Path ()).source, "notes\n");
}

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

TEST (ScriptManifest, NeitherDirectionNeedsATypeAndADeclaredOneStillPinsIt)
{
    // ⚠️ THE REGRESSION THIS PINS RUNS THE OTHER WAY FROM THE RULE IT REPLACED.
    // An untyped output used to be refused outright, on the grounds that a port
    // accepting anything turns a wiring mistake into a surprise downstream. That
    // cost is real; what made the rule wrong is that the type of a script output
    // is decided by the line that computes it, so the header was a restatement
    // that could only ever be wrong - and it was mandatory on every script.
    const ScriptManifest inferred = ParsePython ("# @in a\n# @out b\nb = a\n");
    ASSERT_TRUE (inferred.Ok ()) << inferred.diagnostics.front ().message;
    EXPECT_EQ (inferred.inputs[0].valueType, ValueType::Absent);
    ASSERT_EQ (inferred.outputs.size (), 1u);
    EXPECT_EQ (inferred.outputs[0].valueType, ValueType::Absent);

    // And declaring one still pins the interface, which is the whole reason to.
    const ScriptManifest pinned = ParsePython ("# @in a\n# @out b : number\nb = a\n");
    ASSERT_TRUE (pinned.Ok ());
    EXPECT_EQ (pinned.outputs[0].valueType, ValueType::Double);
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

TEST (ScriptSource, TheStarterScriptComputesSomethingFromTwoInputsWithDefaults)
{
    // ⚠️ THE POINT OF THE TEMPLATE IS THAT A NODE PLACED A MOMENT AGO ALREADY
    // WORKS. So this asserts the SHAPE the user is promised - two inputs they can
    // wire, both with defaults so an unwired node still evaluates, and one typed
    // output - and not merely that the file parses. A template that parsed and
    // declared nothing would pass the older test and be useless.
    const ScriptManifest manifest = ParsePython (ScriptTemplateSource (ScriptLanguage::Python));
    ASSERT_TRUE (manifest.Ok ());
    ASSERT_EQ (manifest.inputs.size (), 2u);
    EXPECT_EQ (manifest.inputs[0].id, "x");
    EXPECT_EQ (manifest.inputs[1].id, "y");
    ASSERT_EQ (manifest.outputs.size (), 1u);
    EXPECT_EQ (manifest.outputs[0].id, "out");
    // ⚠️ UNTYPED, DELIBERATELY, AND THE TEMPLATE IS WHERE PEOPLE LEARN THE FORM.
    // An output's type is decided by the line that computes it; writing it again
    // in the header is a restatement that can only ever be wrong. A starter
    // script that declared one would teach everybody to keep declaring them.
    EXPECT_EQ (manifest.outputs[0].valueType, ValueType::Absent);
    EXPECT_EQ (manifest.defaults.count ("x"), 1u);
    EXPECT_EQ (manifest.defaults.count ("y"), 1u);
    // And the body is not a comment block: it imports and it assigns the output.
    const std::string source = ScriptTemplateSource (ScriptLanguage::Python);
    EXPECT_NE (source.find ("import math"), std::string::npos);
    EXPECT_NE (source.find ("out = math.hypot(x, y)"), std::string::npos);
}

TEST (ScriptSource, TheJavaScriptStarterDeclaresTheSamePortsAndImportsNothing)
{
    const std::string source = ScriptTemplateSource (ScriptLanguage::JavaScript);
    const ScriptManifest manifest = ParseJs (source);
    ASSERT_TRUE (manifest.Ok ());
    EXPECT_EQ (manifest.inputs.size (), 2u);
    ASSERT_EQ (manifest.outputs.size (), 1u);
    EXPECT_EQ (manifest.outputs[0].id, "out");
    // QuickJS is configured with no module loader on purpose, so a template that
    // suggested an import would be suggesting something that cannot work.
    EXPECT_EQ (source.find ("import "), std::string::npos);
    EXPECT_EQ (source.find ("require("), std::string::npos);
}

TEST (ScriptSource, TheWrittenTemplateIsExactlyTheTemplateFunctionsText)
{
    // One source of truth: the palette shows this text before the folder exists,
    // and the file that eventually lands must not say something else.
    const std::filesystem::path path = std::filesystem::temp_directory_path () / "tapioca_script_same.py";
    std::error_code code;
    std::filesystem::remove (path, code);
    std::string error;
    ASSERT_TRUE (WriteScriptTemplate (path.string (), error)) << error;
    EXPECT_EQ (ReadScript (path.string ()).source, ScriptTemplateSource (ScriptLanguage::Python));
    std::filesystem::remove (path, code);
}

// ---------------------------------------------------------------------------
// The alias: renaming the node rewrites one line of its header.
//
// ⚠️ EVERY TEST HERE IS ABOUT WHAT SURVIVES. The file being edited is one
// somebody may have spent a morning on, and a rename that took their description
// or their port labels with it would be a far worse bug than a rename that did
// nothing at all.

TEST (ScriptAlias, ReplacesAnExistingNameAndLeavesEverythingElseWhereItWas)
{
    const std::string before = "# @name        Old name\n"
                               "# @description Something the user wrote.\n"
                               "#\n"
                               "# @in  x : number = 0   \"X\"\n"
                               "# @out out : number\n"
                               "\n"
                               "out = x\n";
    const std::string after = WithScriptName (before, "New name", ScriptLanguage::Python);
    const ScriptManifest manifest = ParsePython (after);
    EXPECT_EQ (manifest.name, "New name");
    EXPECT_EQ (manifest.description, "Something the user wrote.");
    ASSERT_EQ (manifest.inputs.size (), 1u);
    EXPECT_EQ (manifest.inputs[0].label, "X");
    EXPECT_NE (after.find ("out = x\n"), std::string::npos);
    EXPECT_EQ (after.find ("Old name"), std::string::npos);
}

TEST (ScriptAlias, AHeaderWithNoNameGetsOneAtTheTopOfItsBlock)
{
    const std::string before = "# @description Something.\n"
                               "# @out out : number\n"
                               "\n"
                               "out = 1\n";
    const std::string after = WithScriptName (before, "Named", ScriptLanguage::Python);
    EXPECT_EQ (ParsePython (after).name, "Named");
    // At the TOP of the comment block, not appended under the port declarations
    // where it would read as an afterthought in the user's own file.
    EXPECT_LT (after.find ("@name"), after.find ("@description"));
    EXPECT_EQ (ParsePython (after).outputs.size (), 1u);
}

TEST (ScriptAlias, AFileWithNoHeaderAtAllGetsOneBeforeItsFirstLine)
{
    const std::string after = WithScriptName ("out = 1\n", "Named", ScriptLanguage::Python);
    EXPECT_EQ (ParsePython (after).name, "Named");
    EXPECT_NE (after.find ("out = 1\n"), std::string::npos);
}

TEST (ScriptAlias, AShebangKeepsItsFirstLine)
{
    // A shebang that stopped being the first line stops being a shebang.
    const std::string after = WithScriptName ("#!/usr/bin/env python\nout = 1\n", "Named", ScriptLanguage::Python);
    EXPECT_EQ (after.rfind ("#!/usr/bin/env python", 0), 0u);
    EXPECT_EQ (ParsePython (after).name, "Named");
}

TEST (ScriptAlias, JavaScriptUsesItsOwnCommentPrefix)
{
    const std::string after = WithScriptName ("// @out out : number\nout = 1;\n", "Named", ScriptLanguage::JavaScript);
    EXPECT_EQ (ParseJs (after).name, "Named");
    EXPECT_EQ (after.find ("# @name"), std::string::npos);
}

TEST (ScriptAlias, RenamingToWhatItAlreadySaysChangesNothingAtAll)
{
    // Byte-identical, which is what lets the caller skip the write entirely - and
    // a skipped write is a conflict that never happens with the external editor.
    const std::string before = "# @name        Same\n# @out out : number\n\nout = 1\n";
    EXPECT_EQ (WithScriptName (before, "Same", ScriptLanguage::Python), before);
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

    const ScriptWrite written = WriteScriptSource (file.Path (), "# @out b : number\nb = 2\n", base, &HashScriptSource);
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
    // Through the FOLDER, because that is what a node is - the editor saves
    // main.py and the node loads the workspace that contains it.
    const TempWorkspace node ("tapioca_script_roundtrip");
    node.Write ("main.py", "# @out b : number\nb = 1\n");
    const std::string entry = (std::filesystem::path (node.Path ()) / "main.py").string ();
    const std::string base = HashScriptSource (ReadScript (entry).source);
    const std::string edited = "# @in  a : number\n# @out b : number\nb = a * 2\n";
    ASSERT_TRUE (WriteScriptSource (entry, edited, base, &HashScriptSource).ok);

    const ScriptState state = LoadScriptState (node.Path (), ScriptLanguage::Python);
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

// ---------------------------------------------------------------------------
// Inferred outputs: `@out result`, with no type written down.
//
// ⚠️ THESE ARE ABOUT WHAT THE VALUE STAYS, NOT ABOUT WHETHER IT ARRIVES. The
// version this replaced flattened an untyped output to TEXT, which "worked" -
// `out = 2.5` arrived downstream as the string "2.5" - and made omitting the
// type useless, so nobody did. A number has to still be a number.

TEST (ScriptInference, ANumberStaysANumberAndAWholeOneStaysAnInteger)
{
    const ScriptRunResult number = RunJs ("out = 2.5;", {}, { Out ("out", ValueType::Absent) });
    ASSERT_TRUE (number.ok) << number.error;
    EXPECT_EQ (number.outputs.at ("out").Type (), ValueType::Double);
    EXPECT_EQ (std::get<double> (number.outputs.at ("out").AsValue ().DataValue ()), 2.5);

    // ⚠️ A WHOLE NUMBER IS AN INTEGER, AND IN JAVASCRIPT THAT IS A DECISION ABOUT
    // THE VALUE. JS has one number type, so `2` and `2.0` are indistinguishable -
    // and a count from a JS node has to wire into the same integer port a Python
    // node's count does, or identical scripts produce different graph types.
    const ScriptRunResult whole = RunJs ("out = 4;", {}, { Out ("out", ValueType::Absent) });
    ASSERT_TRUE (whole.ok) << whole.error;
    EXPECT_EQ (whole.outputs.at ("out").Type (), ValueType::Integer);
}

TEST (ScriptInference, TextBoolAndAPointKeepTheirShapes)
{
    const ScriptRunResult text = RunJs ("out = 'wall';", {}, { Out ("out", ValueType::Absent) });
    ASSERT_TRUE (text.ok) << text.error;
    EXPECT_EQ (text.outputs.at ("out").Type (), ValueType::String);

    // ⚠️ BOOL IS TESTED WITH JS_IsBool AND NOT JS_ToBool. Everything in JavaScript
    // is truthy-convertible, so a decoder that asked "can this be a bool" first -
    // which is exactly what the JSON decoder can safely do - would turn every
    // output in the language into `true`.
    const ScriptRunResult flag = RunJs ("out = false;", {}, { Out ("out", ValueType::Absent) });
    ASSERT_TRUE (flag.ok) << flag.error;
    EXPECT_EQ (flag.outputs.at ("out").Type (), ValueType::Bool);
    EXPECT_EQ (std::get<bool> (flag.outputs.at ("out").AsValue ().DataValue ()), false);

    const ScriptRunResult point = RunJs ("out = { x: 1, y: 2, z: 3 };", {}, { Out ("out", ValueType::Absent) });
    ASSERT_TRUE (point.ok) << point.error;
    EXPECT_EQ (point.outputs.at ("out").Type (), ValueType::Point3);
}

TEST (ScriptInference, AnArrayIsALISTAndNeverAPolyline)
{
    // ⚠️ THE ONE PLACE INFERENCE DELIBERATELY DOES NOT GUESS. An array of points
    // is a list of points; a polyline and a polygon differ from it by whether the
    // last point joins the first, which is a fact about INTENT that no shape
    // carries. A script that means a polyline says `@out edge : polyline`, and
    // that is precisely what declaring a type is for.
    const ScriptRunResult list = RunJs ("out = [{x:0,y:0,z:0},{x:1,y:0,z:0}];", {}, { Out ("out", ValueType::Absent) });
    ASSERT_TRUE (list.ok) << list.error;
    ASSERT_EQ (list.outputs.at ("out").Type (), ValueType::List);
    ASSERT_EQ (list.outputs.at ("out").Items ().size (), 2u);
    EXPECT_EQ (list.outputs.at ("out").Items ()[0].Type (), ValueType::Point3);

    // A DECLARED polyline still reads the same array as a polyline, unchanged.
    const ScriptRunResult polyline =
        RunJs ("out = [{x:0,y:0,z:0},{x:1,y:0,z:0}];", {}, { Out ("out", ValueType::Polyline) });
    ASSERT_TRUE (polyline.ok) << polyline.error;
    EXPECT_EQ (polyline.outputs.at ("out").Type (), ValueType::Polyline);
}

TEST (ScriptInference, AHeterogeneousListIsKeptItemByItem)
{
    // A declared `list` reads every item as a number, because a header has no way
    // to state a per-item type. An inferred one had no header to disagree with,
    // so each item keeps what it is.
    const ScriptRunResult mixed = RunJs ("out = [1, 'two', true];", {}, { Out ("out", ValueType::Absent) });
    ASSERT_TRUE (mixed.ok) << mixed.error;
    ASSERT_EQ (mixed.outputs.at ("out").Items ().size (), 3u);
    EXPECT_EQ (mixed.outputs.at ("out").Items ()[0].Type (), ValueType::Integer);
    EXPECT_EQ (mixed.outputs.at ("out").Items ()[1].Type (), ValueType::String);
    EXPECT_EQ (mixed.outputs.at ("out").Items ()[2].Type (), ValueType::Bool);
}

TEST (ScriptInference, AnInferredOutputStillCannotFabricateGeometryOrAnElement)
{
    // Omitting the type relaxes what a script may RETURN, never what it may
    // INVENT. Both refusals are the ones a declared port already makes.
    const ScriptRunResult mesh =
        RunJs ("out = { isMesh: true, vertexCount: 3 };", {}, { Out ("out", ValueType::Absent) });
    EXPECT_FALSE (mesh.ok);
    EXPECT_NE (mesh.error.find ("mesh"), std::string::npos);

    const ScriptRunResult element = RunJs ("out = { elementGuid: '' };", {}, { Out ("out", ValueType::Absent) });
    EXPECT_FALSE (element.ok);
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

ScriptState LoadExample (const char* folder, ScriptLanguage language)
{
    return LoadScriptState (ExamplePath (folder), language);
}

// A COPY of a shipped example, in the temp directory, that removes itself.
//
// ⚠️ NO TEST MAY POINT A NODE AT THE REPOSITORY'S OWN FILES, AND THIS EXISTS
// BECAUSE ONE DID. ReloadScriptNode migrates a single-file node into a folder,
// which MOVES the file - so a test that handed it a path into AddOn/EvP/GraphScripts
// renamed two shipped examples out from under the working tree. A test that
// writes anywhere but a temporary directory is a bug regardless of what it was
// asserting; the copy costs a few hundred bytes and makes that impossible.
class ExampleCopy {
  public:
    ExampleCopy (const char* folder, const char* entry)
    {
        const std::string source = ExamplePath (folder);
        root_ = std::filesystem::temp_directory_path () / (std::string ("tapioca_example_") + folder);
        std::error_code code;
        std::filesystem::remove_all (root_, code);
        if (source.empty () || !std::filesystem::exists (std::filesystem::path (source) / entry, code))
            return;
        std::filesystem::create_directories (root_, code);
        std::filesystem::copy (source, root_, std::filesystem::copy_options::recursive, code);
        ok_ = !code;
    }
    ~ExampleCopy ()
    {
        std::error_code code;
        std::filesystem::remove_all (root_, code);
    }

    bool Ok () const
    {
        return ok_;
    }
    std::string Path () const
    {
        return root_.string ();
    }

  private:
    std::filesystem::path root_;
    bool ok_ = false;
};

} // namespace

TEST (ScriptExamples, EveryShippedExampleParsesAndDeclaresPorts)
{
    const struct {
        const char* folder;
        ScriptLanguage language;
    } examples[] = { { "01-hello", ScriptLanguage::Python },
                     { "02-hello", ScriptLanguage::JavaScript },
                     { "03-every-type", ScriptLanguage::Python },
                     { "04-ports-change", ScriptLanguage::Python },
                     { "05-output-and-errors", ScriptLanguage::Python },
                     { "06-geometry", ScriptLanguage::JavaScript } };
    for (const auto& [name, language] : examples) {
        const ScriptState state = LoadExample (name, language);
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
    const ScriptState state = LoadExample ("01-hello", ScriptLanguage::Python);
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
    const ScriptState python = LoadExample ("01-hello", ScriptLanguage::Python);
    const ScriptState javascript = LoadExample ("02-hello", ScriptLanguage::JavaScript);
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
    const ScriptState state = LoadExample ("03-every-type", ScriptLanguage::Python);
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
    const ScriptState state = LoadExample ("02-hello", ScriptLanguage::JavaScript);
    if (!state.loadError.empty ())
        GTEST_SKIP () << state.loadError;
    ASSERT_TRUE (state.manifest.Ok ());

    const ScriptRunResult result = RunJs (state.source, { { "value", Value (21.0) } }, state.manifest.outputs);
    ASSERT_TRUE (result.ok) << result.error;
    EXPECT_EQ (std::get<double> (result.outputs.at ("doubled").DataValue ()), 42.0);
}

TEST (ScriptExamples, TheGeometryExampleRoundTripsPointsAndAPolyline)
{
    const ScriptState state = LoadExample ("06-geometry", ScriptLanguage::JavaScript);
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
    // A COPY, never the repository's own file: this calls ReloadScriptNode, which
    // is allowed to move files on disk. See ExampleCopy.
    const ExampleCopy example ("01-hello", "main.py");
    if (!example.Ok ())
        GTEST_SKIP () << "the shipped examples are not present";
    const std::string path = example.Path ();

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
    const ExampleCopy example ("02-hello", "main.js");
    if (!example.Ok ())
        GTEST_SKIP () << "the shipped examples are not present";
    const std::string path = example.Path ();

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
