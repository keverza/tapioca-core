#ifndef EVP_NODEGRAPH_SCRIPTINTELLIGENCE_HPP
#define EVP_NODEGRAPH_SCRIPTINTELLIGENCE_HPP

// Code intelligence for script nodes: completion, from a real type checker.
//
// ⚠️ basedpyright, NOT AN INTROSPECTION TRICK, AND THE DIFFERENCE IS WHAT THIS
// COSTS. The cheap way to answer `math.hy` is to import `math` in Tapioca's own
// interpreter and `dir()` it - twenty lines, no process, accurate for anything
// installed. It cannot answer `points[0].` , or anything about a local, because
// it has no types; every completion after the first dot would be a dead end. So
// the plan (§10) says use a language server and do not implement Python semantic
// analysis, and that is what this is.
//
// ⚠️ AND THE SERVER IS NOT SHIPPED. basedpyright brings its own Node runtime -
// about 150 MB installed - and the large majority of this add-on's users never
// open a script node. It is installed ON DEMAND, into the runtime's own
// site-packages, the first time somebody asks for it; until then the editor works
// exactly as it did and says that code intelligence is available. That is why
// `State` has an installing arm at all: the wait is a real part of the feature
// rather than an error.
//
// ⚠️ THE IMPORT ROOTS COME FROM ScriptWorkspace AND ARE NOT COMPUTED AGAIN HERE.
// Pyright's `extraPaths` and the runtime's `sys.path` must be the same list or
// the editor reports an import error for a line that runs perfectly well - which
// is worse than no editor intelligence at all, because it is confidently wrong.
// ResolveScriptWorkspace owns that list; this file passes it on.
//
// ---------------------------------------------------------------------------
// What is portable and what is not
//
// Everything here is Win32-free and DevKit-free: the framing, the request
// building, the reply routing, the position mapping and the completion mapping
// are all plain C++ over strings, so the offline suite covers the parts that are
// easy to get wrong. The one thing that cannot be portable - starting a process
// and owning its pipes - is behind ILanguageServerProcess, with its Win32
// implementation in ScriptIntelligenceWin32.cpp, exactly as IScriptWatcher is.

#include "NodeGraph/Json.hpp"
#include "NodeGraph/ScriptWorkspace.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace evp::nodegraph {

// ---------------------------------------------------------------------------
// The wire format.

// One LSP message, framed. `Content-Length: <n>\r\n\r\n<body>`.
//
// Trivial, and worth its own function for one reason: the length is in BYTES and
// a script's completion request carries the whole buffer, which is UTF-8 and
// routinely not ASCII. A length counted in anything else desynchronises the
// stream permanently - every later message is read at the wrong offset - which
// presents as a language server that worked until somebody typed an accent.
std::string FrameLspMessage (const std::string& body);

// Pulls one framed message off the front of `buffer`, removing it.
//
// ⚠️ IT MUST TOLERATE A PARTIAL MESSAGE, BECAUSE THAT IS THE NORMAL CASE. A pipe
// read returns whatever bytes have arrived, which is routinely half a header or
// most of a body; returning false and leaving the buffer untouched is how the
// caller says "read more". A parser that assumed message boundaries would work on
// every small reply and fail on exactly the large ones - a completion list - which
// are the ones this feature exists to deliver.
bool TakeLspMessage (std::string& buffer, std::string& body);

// ---------------------------------------------------------------------------
// The process behind it.

// Starting a program and owning its pipes: the one thing here that cannot be
// portable. Implemented in ScriptIntelligenceWin32.cpp.
class ILanguageServerProcess {
  public:
    virtual ~ILanguageServerProcess () = default;

    // False when the process has exited or was never started. Everything else
    // checks this rather than assuming: a server that crashed must degrade the
    // editor to no completion, never take a node down.
    virtual bool Running () const = 0;

    virtual bool Write (const std::string& bytes) = 0;

    // Whatever has arrived since the last call, appended. Never blocks for long:
    // it is called from a request's own wait loop, and a read that blocked
    // forever would hang the verb rather than time it out.
    virtual void Read (std::string& into) = 0;

    virtual void Stop () = 0;
};

// Starts `executable` with `arguments`, pipes attached. Null when it would not
// start, which is an ordinary answer - the server may not be installed.
using LanguageServerProcessFactory =
    std::function<std::unique_ptr<ILanguageServerProcess> (const std::string&, const std::vector<std::string>&)>;

/**
 * Chooses how a process gets started.
 *
 * ⚠️ A HOOK RATHER THAN A DIRECT CALL, FOR TWO REASONS, AND THE SECOND IS THE
 * IMPORTANT ONE. It keeps the Win32 file out of the offline suite's link, the
 * way IScriptWatcher does. But it also lets a TEST install a scripted fake
 * server - so the framing, the initialize handshake, the per-request
 * configuration, the document versioning and the reply routing are all covered
 * with no basedpyright, no Node and no network. Those are the parts that are
 * easy to get subtly wrong and impossible to debug from a screenshot of an
 * empty completion menu.
 *
 * Unset by default: no factory means no server, which is what the offline suite
 * and a non-Windows build both see.
 */
void SetLanguageServerProcessFactory (LanguageServerProcessFactory factory);

std::unique_ptr<ILanguageServerProcess> StartLanguageServerProcess (const std::string& executable,
                                                                    const std::vector<std::string>& arguments);

// Installs the Win32 factory. Called once at add-on startup, exactly as
// InstallJavaScriptRuntime is - explicit rather than a static initialiser,
// because a translation unit nothing references may be dropped by the linker.
void InstallWin32LanguageServerProcess ();

// ---------------------------------------------------------------------------
// Where the server lives.

// `%LOCALAPPDATA%\Tapioca\runtime`, the interpreter a script node already runs
// in. Empty when the environment does not say.
std::string TapiocaRuntimeRoot ();

// The langserver executable inside it, whether or not it exists. A pip console
// script, so it carries the interpreter path itself and needs nothing on PATH.
std::string LanguageServerExecutable ();

// Whether that file is there. The whole of "is code intelligence available".
bool LanguageServerInstalled ();

// ---------------------------------------------------------------------------
// What the palette asks for.

// One completion, flattened to what a menu draws. Deliberately NOT the LSP item:
// a browser has no business knowing about `textEdit`, `insertTextFormat` or
// resolve support, and a palette that passed LSP structures through would have to
// track the protocol's version alongside the server's.
struct ScriptCompletion {
    std::string label;

    // What is actually inserted, which is NOT always the label - `__init__` is
    // labelled with its parentheses by some servers, and a dunder's sort text
    // differs from both. Kept separate so the editor never has to guess.
    std::string insertText;

    // "function", "module", "class"... from the LSP kind, as a word. A number
    // would make the browser carry the protocol's enum.
    std::string kind;

    // The one-line signature or type, when the server offers one.
    std::string detail;

    // First line of the docstring, trimmed. The rest is not sent: a completion
    // list is fifty items and numpy's docstrings are essays.
    std::string documentation;
};

enum class IntelligenceState {
    // The server is not installed. An ordinary state, and the one every machine
    // starts in - see the note at the top of this file.
    NotInstalled,
    Installing,
    // Installed, and the process is up and has answered `initialize`.
    Ready,
    // Installed but the process would not start or has died. Distinguished from
    // NotInstalled because the answer is different: one is a download, the other
    // is a bug worth reporting.
    Failed,
};

struct IntelligenceStatus {
    IntelligenceState state = IntelligenceState::NotInstalled;

    // Written for the person reading it in the editor's footer, never a code.
    std::string message;

    // Where the server would be, so "not installed" can say what is missing.
    std::string executable;
};

/**
 * One language server, shared by every script node.
 *
 * ⚠️ ONE SERVER, NOT ONE PER NODE, AND NOT ONE PER FOLDER. Pyright's startup is
 * dominated by reading the typeshed stubs and the interpreter's site-packages -
 * seconds, and hundreds of megabytes resident. A server per node would multiply
 * both by the number of script nodes on a canvas, which is exactly the graph
 * somebody with twenty of them has. The node's own folder travels per REQUEST
 * instead, as the document's URI and the workspace configuration.
 *
 * ⚠️ AND IT IS LAZY IN BOTH DIRECTIONS. Nothing starts until the first request,
 * so a session that never opens a script node never pays; and a request that
 * arrives while the server is still initialising waits rather than failing,
 * because the alternative is that the first completion anybody ever asks for is
 * the one that silently does nothing.
 */
class ScriptIntelligence {
  public:
    static ScriptIntelligence& Get ();

    IntelligenceStatus Status () const;

    /**
     * Completion at a position in one of a node's files.
     *
     * `source` is the buffer as the EDITOR has it, not the file on disk, and that
     * is the whole point: completion is asked for on text the user is midway
     * through typing and has not saved. The server is told about that text with
     * `didOpen` / `didChange` before the request, so what it analyses is what is
     * on screen.
     *
     * `line` and `character` are zero-based, as LSP counts them.
     *
     * ⚠️ `character` IS A UTF-16 CODE-UNIT OFFSET, NOT A BYTE OFFSET, and it is
     * passed through from the browser UNCONVERTED because that is already what it
     * is: a JavaScript string index is a UTF-16 index, so CodeMirror's column and
     * the protocol's agree by construction. Converting here would be the bug. Any
     * future caller computing a position natively - from a byte offset into the
     * UTF-8 buffer - has to do the conversion itself, and the case that differs is
     * reachable: one emoji in a comment shifts every column after it on that line.
     *
     * Never throws and never blocks indefinitely: a server that has stopped
     * answering yields an empty list, because a completion menu that does not
     * appear is a much smaller failure than an editor that stops accepting keys.
     */
    std::vector<ScriptCompletion> Complete (const ScriptWorkspace& workspace, const std::string& file,
                                            const std::string& source, int line, int character, std::string& error);

    // Begin the on-demand install. Returns immediately; Status reports progress.
    // Safe to call twice - a second call while one is running does nothing.
    void BeginInstall ();

    // Stops the process. Called when Archicad is shutting down: a language server
    // left running is a node process the user has to find in Task Manager.
    void Shutdown ();

  private:
    ScriptIntelligence () = default;
    class Impl;
    std::shared_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// The pure parts, exposed so the offline suite can cover them.

// A file path as a `file:///C:/...` URI, which is how LSP names documents.
//
// ⚠️ THE DRIVE LETTER AND THE PERCENT-ENCODING BOTH MATTER. A Windows path is
// not a URI: backslashes are separators, the drive needs a leading slash, and a
// workflow folder may contain a space or a non-ASCII character. A server handed a
// malformed URI does not complain - it answers about a document it does not have,
// which is an empty completion list and no clue why.
std::string PathToFileUri (const std::string& path);

// The LSP `CompletionItemKind` number as a word. Unknown numbers become an empty
// string rather than a guess; the menu simply draws no icon for them.
std::string CompletionKindWord (int kind);

// Reads a `textDocument/completion` result - which the protocol allows to be
// either a bare array or a `CompletionList` object - into the flattened form.
std::vector<ScriptCompletion> ParseCompletionResult (const json::JsonValue& result);

} // namespace evp::nodegraph

#endif
