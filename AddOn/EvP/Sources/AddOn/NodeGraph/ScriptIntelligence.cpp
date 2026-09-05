#include "NodeGraph/ScriptIntelligence.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

namespace evp::nodegraph {

using json::JsonArray;
using json::JsonObject;
using json::JsonValue;

namespace {

namespace fs = std::filesystem;

// The version this add-on was built against, pinned rather than floating.
//
// ⚠️ A LANGUAGE SERVER IS A DEPENDENCY LIKE ANY OTHER. `pip install basedpyright`
// with no version installs whatever shipped this morning, which is how a machine
// provisioned in March and one provisioned in June disagree about whether a
// script has an error in it. The reference catalog pins every other vendored
// thing; this is the same rule reaching the one dependency that installs itself.
constexpr const char kServerRequirement[] = "basedpyright==1.39.10";

// How long a completion may take before the editor gives up on it.
//
// ⚠️ A CEILING, NOT A BUDGET, AND IT IS GENEROUS ON PURPOSE. Pyright's FIRST
// request after start has to read typeshed and index site-packages, which on a
// cold cache is seconds; every one after it is milliseconds. A tight timeout
// would fail exactly the request that teaches somebody whether the feature works.
// The editor is not blocked meanwhile - the verb is gate-free and the browser
// keeps taking keystrokes - so the cost of waiting is patience, not a frozen
// palette.
constexpr int kCompletionTimeoutMs = 8000;
constexpr int kInitializeTimeoutMs = 30000;

std::string ReadEnvironment (const char* name)
{
#if defined(_WIN32)
    char* buffer = nullptr;
    size_t length = 0;
    if (_dupenv_s (&buffer, &length, name) != 0 || buffer == nullptr)
        return std::string ();
    std::string value (buffer);
    std::free (buffer);
    return value;
#else
    const char* value = std::getenv (name);
    return value == nullptr ? std::string () : std::string (value);
#endif
}

fs::path PathFromUtf8 (const std::string& path)
{
    const char8_t* bytes = reinterpret_cast<const char8_t*> (path.c_str ());
    return fs::path (std::u8string (bytes, bytes + path.size ()));
}

std::string Utf8FromPath (const fs::path& path)
{
    const std::u8string text = path.u8string ();
    return std::string (reinterpret_cast<const char*> (text.c_str ()), text.size ());
}

bool IsUnreservedUriByte (unsigned char byte)
{
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
           byte == '-' || byte == '.' || byte == '_' || byte == '~' || byte == '/' || byte == ':';
}

} // namespace

// ---------------------------------------------------------------------------
// The wire format.

std::string FrameLspMessage (const std::string& body)
{
    return "Content-Length: " + std::to_string (body.size ()) + "\r\n\r\n" + body;
}

bool TakeLspMessage (std::string& buffer, std::string& body)
{
    const size_t headerEnd = buffer.find ("\r\n\r\n");
    if (headerEnd == std::string::npos)
        return false;

    // Only Content-Length is read. Content-Type is allowed by the protocol and
    // says nothing this client acts on; a header it does not recognise is
    // skipped rather than refused, because a future server adding one must not
    // stop the stream.
    size_t length = std::string::npos;
    size_t lineStart = 0;
    while (lineStart < headerEnd) {
        size_t lineEnd = buffer.find ("\r\n", lineStart);
        if (lineEnd == std::string::npos || lineEnd > headerEnd)
            lineEnd = headerEnd;
        const std::string line = buffer.substr (lineStart, lineEnd - lineStart);
        const size_t colon = line.find (':');
        if (colon != std::string::npos) {
            std::string name = line.substr (0, colon);
            std::transform (name.begin (), name.end (), name.begin (),
                            [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
            if (name == "content-length") {
                try {
                    length = static_cast<size_t> (std::stoull (line.substr (colon + 1)));
                }
                catch (...) {
                    length = std::string::npos;
                }
            }
        }
        lineStart = lineEnd + 2;
    }
    if (length == std::string::npos) {
        // A header block with no length is unrecoverable: there is no way to know
        // where the body ends, so the stream is desynchronised from here on.
        // Dropping the header and continuing at least lets the next well-formed
        // message be found instead of spinning on this one forever.
        buffer.erase (0, headerEnd + 4);
        return false;
    }

    const size_t bodyStart = headerEnd + 4;
    if (buffer.size () < bodyStart + length)
        return false; // Partial. The normal case; see the header's note.

    body = buffer.substr (bodyStart, length);
    buffer.erase (0, bodyStart + length);
    return true;
}

// ---------------------------------------------------------------------------
// Pure helpers.

std::string PathToFileUri (const std::string& path)
{
    // Separators first, so the percent-encoder below never sees a backslash and
    // has no opinion about it.
    std::string normalised = path;
    std::replace (normalised.begin (), normalised.end (), '\\', '/');
    // `C:/x` -> `/C:/x`. A drive-letter path has no leading slash and the URI
    // authority is empty, so `file://` + `C:/x` would parse `C:` as a HOST.
    if (normalised.size () >= 2 && normalised[1] == ':')
        normalised.insert (normalised.begin (), '/');

    std::string encoded;
    encoded.reserve (normalised.size () + 8);
    for (const char character : normalised) {
        const unsigned char byte = static_cast<unsigned char> (character);
        if (IsUnreservedUriByte (byte)) {
            encoded.push_back (character);
            continue;
        }
        // Byte-wise, which is what makes a non-ASCII folder name work: the input
        // is UTF-8 and each byte becomes its own escape, which is exactly what
        // the URI spec asks for.
        static const char kHex[] = "0123456789ABCDEF";
        encoded.push_back ('%');
        encoded.push_back (kHex[byte >> 4]);
        encoded.push_back (kHex[byte & 0x0F]);
    }
    return "file://" + encoded;
}

std::string CompletionKindWord (int kind)
{
    // The LSP CompletionItemKind table, 1-25. Words rather than the numbers, so
    // the browser never carries the protocol's enum - and an unknown number is an
    // empty string rather than a guess, because a wrong icon is worse than none.
    static const char* const kKinds[] = {
        "",         "text",      "method", "function", "constructor",   "field",  "variable",
        "class",    "interface", "module", "property", "unit",          "value",  "enum",
        "keyword",  "snippet",   "color",  "file",     "reference",     "folder", "enumMember",
        "constant", "struct",    "event",  "operator", "typeParameter",
    };
    constexpr int kCount = static_cast<int> (sizeof (kKinds) / sizeof (kKinds[0]));
    if (kind <= 0 || kind >= kCount)
        return std::string ();
    return kKinds[kind];
}

std::vector<ScriptCompletion> ParseCompletionResult (const JsonValue& result)
{
    std::vector<ScriptCompletion> completions;

    // The protocol allows either a bare array or a CompletionList. Both are
    // returned in practice by different servers and by the same server in
    // different modes, so both are read rather than one being assumed.
    const JsonArray* items = result.AsArray ();
    if (items == nullptr) {
        const JsonValue* nested = result.Find ("items");
        if (nested != nullptr)
            items = nested->AsArray ();
    }
    if (items == nullptr)
        return completions;

    for (const JsonValue& item : *items) {
        ScriptCompletion completion;
        const JsonValue* label = item.Find ("label");
        if (label == nullptr || !label->AsString (completion.label) || completion.label.empty ())
            continue;

        // What is INSERTED is not always the label - a server may label a method
        // with its signature - so insertText wins when it is there and the label
        // is only the fallback.
        const JsonValue* insert = item.Find ("insertText");
        if (insert == nullptr || !insert->AsString (completion.insertText))
            completion.insertText = completion.label;

        int64_t kind = 0;
        const JsonValue* kindValue = item.Find ("kind");
        if (kindValue != nullptr && kindValue->AsInteger (kind))
            completion.kind = CompletionKindWord (static_cast<int> (kind));

        const JsonValue* detail = item.Find ("detail");
        if (detail != nullptr)
            detail->AsString (completion.detail);

        // `documentation` is a string or a MarkupContent. Only the first line is
        // kept either way: a completion list is fifty items and numpy's
        // docstrings are essays.
        const JsonValue* documentation = item.Find ("documentation");
        if (documentation != nullptr) {
            std::string text;
            if (!documentation->AsString (text)) {
                const JsonValue* markup = documentation->Find ("value");
                if (markup != nullptr)
                    markup->AsString (text);
            }
            const size_t newline = text.find ('\n');
            completion.documentation = newline == std::string::npos ? text : text.substr (0, newline);
        }

        completions.push_back (std::move (completion));
    }
    return completions;
}

// ---------------------------------------------------------------------------
// The process hook.

namespace {
LanguageServerProcessFactory& ProcessFactory ()
{
    static LanguageServerProcessFactory factory;
    return factory;
}
} // namespace

void SetLanguageServerProcessFactory (LanguageServerProcessFactory factory)
{
    ProcessFactory () = std::move (factory);
}

std::unique_ptr<ILanguageServerProcess> StartLanguageServerProcess (const std::string& executable,
                                                                    const std::vector<std::string>& arguments)
{
    // No factory means no server, which is what the offline suite and any
    // non-Windows build see. An ordinary answer, never an error: every caller
    // already has to handle a server that will not start.
    if (!ProcessFactory ())
        return nullptr;
    return ProcessFactory () (executable, arguments);
}

// ---------------------------------------------------------------------------
// Where the server lives.

std::string TapiocaRuntimeRoot ()
{
    const std::string localAppData = ReadEnvironment ("LOCALAPPDATA");
    if (localAppData.empty ())
        return std::string ();
    return Utf8FromPath (PathFromUtf8 (localAppData) / "Tapioca" / "runtime");
}

std::string LanguageServerExecutable ()
{
    const std::string runtime = TapiocaRuntimeRoot ();
    if (runtime.empty ())
        return std::string ();
    // A pip console script: it carries the interpreter's path inside it, so it
    // runs with nothing on PATH and cannot pick up a different Python.
    return Utf8FromPath (PathFromUtf8 (runtime) / "Scripts" / "basedpyright-langserver.exe");
}

bool LanguageServerInstalled ()
{
    const std::string executable = LanguageServerExecutable ();
    if (executable.empty ())
        return false;
    std::error_code code;
    return fs::is_regular_file (PathFromUtf8 (executable), code);
}

// ---------------------------------------------------------------------------
// The server itself.

class ScriptIntelligence::Impl {
  public:
    std::mutex mutex;
    std::unique_ptr<ILanguageServerProcess> process;
    std::string inbox;
    int64_t nextId = 1;
    bool initialised = false;
    std::string failure;

    // The documents the server has been told about, and the version each is at.
    // ⚠️ LSP VERSIONS MUST INCREASE PER DOCUMENT AND NEVER REPEAT. A server that
    // sees a version it has already processed is entitled to ignore the change,
    // which presents as completion answering about the text as it was several
    // keystrokes ago.
    std::map<std::string, int64_t> openDocuments;

    std::atomic<bool> installing { false };
    std::string installMessage;

    ~Impl ()
    {
        if (process)
            process->Stop ();
    }

    void Send (const JsonObject& message)
    {
        if (!process)
            return;
        JsonObject full = message;
        full.emplace ("jsonrpc", JsonValue::String ("2.0"));
        process->Write (FrameLspMessage (json::Write (JsonValue::Object (std::move (full)), 0)));
    }

    /**
     * Waits for the reply to one request id.
     *
     * ⚠️ NOTIFICATIONS AND OTHER REPLIES ARRIVE IN THE MEANTIME AND ARE DROPPED,
     * NOT QUEUED. Pyright sends progress, diagnostics and log messages
     * continuously; this client asks questions and does not subscribe to
     * anything, so anything that is not the answer being waited for is read off
     * the stream and discarded. It has to be READ - leaving it in the pipe would
     * eventually fill the buffer and block the server mid-write.
     */
    bool Await (int64_t id, int timeoutMs, JsonValue& result, std::string& error)
    {
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (timeoutMs);
        while (std::chrono::steady_clock::now () < deadline) {
            if (!process || !process->Running ()) {
                error = "the language server stopped";
                return false;
            }
            process->Read (inbox);

            std::string body;
            bool sawAny = false;
            while (TakeLspMessage (inbox, body)) {
                sawAny = true;
                const json::ParseResult parsed = json::Parse (body);
                if (!parsed.ok)
                    continue;
                const JsonValue* replyId = parsed.value.Find ("id");
                int64_t number = 0;
                if (replyId == nullptr || !replyId->AsInteger (number) || number != id)
                    continue;
                const JsonValue* failed = parsed.value.Find ("error");
                if (failed != nullptr) {
                    const JsonValue* message = failed->Find ("message");
                    if (message == nullptr || !message->AsString (error))
                        error = "the language server refused the request";
                    return false;
                }
                const JsonValue* value = parsed.value.Find ("result");
                result = value == nullptr ? JsonValue {} : *value;
                return true;
            }
            if (!sawAny)
                std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }
        error = "the language server did not answer in time";
        return false;
    }

    bool EnsureStarted (std::string& error)
    {
        if (process && process->Running () && initialised)
            return true;
        if (!LanguageServerInstalled ()) {
            error = "code intelligence is not installed";
            return false;
        }

        process = StartLanguageServerProcess (LanguageServerExecutable (), { "--stdio" });
        if (!process) {
            error = "the language server would not start";
            return false;
        }
        inbox.clear ();
        openDocuments.clear ();
        initialised = false;

        // ⚠️ NO rootUri, AND THAT IS NOT AN OMISSION. One server serves every
        // script node, and their folders are siblings in the library rather than
        // one tree; a root would make pyright index the whole library at start.
        // Each request carries its own document URI and its own extraPaths, which
        // is what makes the single server correct for all of them.
        JsonObject capabilities;
        JsonObject completionItem;
        completionItem.emplace ("snippetSupport", JsonValue::Bool (false));
        completionItem.emplace ("documentationFormat", JsonValue::Array ({ JsonValue::String ("plaintext") }));
        JsonObject completion;
        completion.emplace ("completionItem", JsonValue::Object (std::move (completionItem)));
        JsonObject textDocument;
        textDocument.emplace ("completion", JsonValue::Object (std::move (completion)));
        capabilities.emplace ("textDocument", JsonValue::Object (std::move (textDocument)));

        JsonObject params;
        params.emplace ("processId", JsonValue {});
        params.emplace ("rootUri", JsonValue {});
        params.emplace ("capabilities", JsonValue::Object (std::move (capabilities)));

        const int64_t id = nextId++;
        JsonObject request;
        request.emplace ("id", JsonValue::Integer (id));
        request.emplace ("method", JsonValue::String ("initialize"));
        request.emplace ("params", JsonValue::Object (std::move (params)));
        Send (request);

        JsonValue result;
        if (!Await (id, kInitializeTimeoutMs, result, error)) {
            process->Stop ();
            process.reset ();
            return false;
        }

        JsonObject initialized;
        initialized.emplace ("method", JsonValue::String ("initialized"));
        initialized.emplace ("params", JsonValue::Object ({}));
        Send (initialized);

        initialised = true;
        return true;
    }

    /**
     * Points the analyser at ONE node's import roots.
     *
     * ⚠️ SENT BEFORE EVERY REQUEST, NOT ONCE AT START. One server serves every
     * script node and each node has its own folder on its own path; configuring
     * at initialize would mean the second node asked about resolved its imports
     * against the first node's folder. That is the failure this whole
     * configuration exists to avoid, and it is silent - the completion list is
     * simply missing the node's own helpers.
     */
    void Configure (const ScriptWorkspace& workspace)
    {
        JsonArray extraPaths;
        for (const std::string& root : workspace.importRoots)
            extraPaths.push_back (JsonValue::String (root));

        JsonObject analysis;
        analysis.emplace ("extraPaths", JsonValue::Array (std::move (extraPaths)));
        // Completion only. Type checking a script node's file would report the
        // header's injected names - `x`, `y` - as undefined on every line that
        // uses them, which is every line worth writing.
        analysis.emplace ("diagnosticMode", JsonValue::String ("openFilesOnly"));
        analysis.emplace ("typeCheckingMode", JsonValue::String ("off"));

        JsonObject python;
        // The interpreter a script node actually runs in, so what completes is
        // what will import: numpy, pillow and the rest are found here and nowhere
        // else on the machine.
        const std::string runtime = TapiocaRuntimeRoot ();
        if (!runtime.empty ())
            python.emplace ("pythonPath", JsonValue::String (Utf8FromPath (PathFromUtf8 (runtime) / "python.exe")));
        python.emplace ("analysis", JsonValue::Object (std::move (analysis)));

        JsonObject settings;
        settings.emplace ("python", JsonValue::Object (std::move (python)));
        JsonObject params;
        params.emplace ("settings", JsonValue::Object (std::move (settings)));
        JsonObject notification;
        notification.emplace ("method", JsonValue::String ("workspace/didChangeConfiguration"));
        notification.emplace ("params", JsonValue::Object (std::move (params)));
        Send (notification);
    }

    // Tells the server what the EDITOR has, which is not what is on disk - see
    // Complete's note. didOpen the first time, didChange after.
    void SyncDocument (const std::string& uri, const std::string& source)
    {
        const auto existing = openDocuments.find (uri);
        if (existing == openDocuments.end ()) {
            JsonObject document;
            document.emplace ("uri", JsonValue::String (uri));
            document.emplace ("languageId", JsonValue::String ("python"));
            document.emplace ("version", JsonValue::Integer (1));
            document.emplace ("text", JsonValue::String (source));
            JsonObject params;
            params.emplace ("textDocument", JsonValue::Object (std::move (document)));
            JsonObject notification;
            notification.emplace ("method", JsonValue::String ("textDocument/didOpen"));
            notification.emplace ("params", JsonValue::Object (std::move (params)));
            Send (notification);
            openDocuments.emplace (uri, 1);
            return;
        }

        const int64_t version = ++existing->second;
        JsonObject document;
        document.emplace ("uri", JsonValue::String (uri));
        document.emplace ("version", JsonValue::Integer (version));
        // Whole-document sync. Incremental changes would need the editor and this
        // client to agree about every edit ever applied, and a single missed one
        // desynchronises them permanently; a script file is a few kilobytes, so
        // sending all of it is cheaper than being wrong.
        JsonObject change;
        change.emplace ("text", JsonValue::String (source));
        JsonObject params;
        params.emplace ("textDocument", JsonValue::Object (std::move (document)));
        params.emplace ("contentChanges", JsonValue::Array ({ JsonValue::Object (std::move (change)) }));
        JsonObject notification;
        notification.emplace ("method", JsonValue::String ("textDocument/didChange"));
        notification.emplace ("params", JsonValue::Object (std::move (params)));
        Send (notification);
    }
};

ScriptIntelligence& ScriptIntelligence::Get ()
{
    static ScriptIntelligence instance;
    if (!instance.impl_)
        instance.impl_ = std::make_shared<Impl> ();
    return instance;
}

IntelligenceStatus ScriptIntelligence::Status () const
{
    IntelligenceStatus status;
    status.executable = LanguageServerExecutable ();

    if (impl_ && impl_->installing.load ()) {
        status.state = IntelligenceState::Installing;
        status.message = "Installing code intelligence…";
        return status;
    }
    if (!LanguageServerInstalled ()) {
        status.state = IntelligenceState::NotInstalled;
        status.message = "Code intelligence is not installed yet.";
        return status;
    }
    if (impl_ && !impl_->failure.empty ()) {
        status.state = IntelligenceState::Failed;
        status.message = impl_->failure;
        return status;
    }
    status.state = IntelligenceState::Ready;
    status.message = "Ready.";
    return status;
}

std::vector<ScriptCompletion> ScriptIntelligence::Complete (const ScriptWorkspace& workspace, const std::string& file,
                                                            const std::string& source, int line, int character,
                                                            std::string& error)
{
    std::vector<ScriptCompletion> completions;
    if (!workspace.ok) {
        error = workspace.error;
        return completions;
    }

    Impl& impl = *impl_;
    std::lock_guard lock (impl.mutex);

    if (!impl.EnsureStarted (error)) {
        impl.failure = error;
        return completions;
    }
    impl.failure.clear ();

    // The file as the server will name it. It need not exist on disk - a brand
    // new node's buffer has never been saved - and pyright is perfectly happy
    // analysing a document it was handed rather than one it read.
    const std::string path = Utf8FromPath (PathFromUtf8 (workspace.root) / PathFromUtf8 (file));
    const std::string uri = PathToFileUri (path);

    impl.Configure (workspace);
    impl.SyncDocument (uri, source);

    JsonObject position;
    position.emplace ("line", JsonValue::Integer (line));
    position.emplace ("character", JsonValue::Integer (character));
    JsonObject document;
    document.emplace ("uri", JsonValue::String (uri));
    JsonObject params;
    params.emplace ("textDocument", JsonValue::Object (std::move (document)));
    params.emplace ("position", JsonValue::Object (std::move (position)));

    const int64_t id = impl.nextId++;
    JsonObject request;
    request.emplace ("id", JsonValue::Integer (id));
    request.emplace ("method", JsonValue::String ("textDocument/completion"));
    request.emplace ("params", JsonValue::Object (std::move (params)));
    impl.Send (request);

    JsonValue result;
    if (!impl.Await (id, kCompletionTimeoutMs, result, error))
        return completions;
    return ParseCompletionResult (result);
}

void ScriptIntelligence::BeginInstall ()
{
    Impl& impl = *impl_;
    bool expected = false;
    // One at a time. Two pips writing the same site-packages is the corruption
    // _env.py's file lock exists to prevent, and the palette can produce a second
    // press easily.
    if (!impl.installing.compare_exchange_strong (expected, true))
        return;

    const std::string runtime = TapiocaRuntimeRoot ();
    if (runtime.empty ()) {
        impl.installing.store (false);
        return;
    }
    const std::string python = Utf8FromPath (PathFromUtf8 (runtime) / "python.exe");

    std::shared_ptr<Impl> owner = impl_;
    // Detached, and it owns a reference to the state it reports into: the install
    // is a minute of network and the verb that started it returned immediately.
    std::thread ([owner, python] () {
        // `-s` keeps the user's own site-packages out of it, exactly as _env.py's
        // installer does - the runtime is provisioned, not shared.
        std::unique_ptr<ILanguageServerProcess> pip = StartLanguageServerProcess (
            python, { "-s", "-m", "pip", "install", "--no-warn-script-location", kServerRequirement });
        if (pip) {
            std::string output;
            while (pip->Running ()) {
                pip->Read (output);
                std::this_thread::sleep_for (std::chrono::milliseconds (200));
            }
            pip->Read (output);
        }
        owner->installing.store (false);
    }).detach ();
}

void ScriptIntelligence::Shutdown ()
{
    if (!impl_)
        return;
    std::lock_guard lock (impl_->mutex);
    if (impl_->process) {
        impl_->process->Stop ();
        impl_->process.reset ();
    }
    impl_->initialised = false;
    impl_->openDocuments.clear ();
}

} // namespace evp::nodegraph
