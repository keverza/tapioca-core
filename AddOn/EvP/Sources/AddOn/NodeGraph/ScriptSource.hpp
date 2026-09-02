#ifndef EVP_NODEGRAPH_SCRIPTSOURCE_HPP
#define EVP_NODEGRAPH_SCRIPTSOURCE_HPP

// Getting a script node's file off disk, and knowing when it changed.
//
// ⚠️ THE FILE IS OWNED BY VSCODE OR SUBLIME, NOT BY THIS ADD-ON, AND THAT IS THE
// WHOLE POINT OF THE FEATURE. Everything here reads; nothing writes over a file
// a user has open in an editor. The one exception is scaffolding a NEW file,
// which refuses to overwrite an existing one - see WriteScriptTemplate.
//
// It is also why the read is a SNAPSHOT with a stamp rather than a stream. The
// editor saves whole files, often by writing a temporary and renaming over the
// target, so the honest model is "the file as it was at this instant, and here
// is how to tell whether that is still true".
//
// DevKit-free and Win32-free: std::filesystem only, so the offline suite covers
// the reading, the size ceiling and the staleness comparison. The half that
// cannot be portable - a directory change notification - is behind
// IScriptWatcher, with its Win32 implementation in ScriptWatcherWin32.cpp.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace evp::nodegraph {

// A file's identity at an instant. Compared, never interpreted: the numbers are
// only ever asked "are these the same two numbers as last time".
//
// ⚠️ SIZE IS PART OF THE STAMP ON PURPOSE. Filesystem timestamps on Windows have
// coarse enough resolution that a fast save-run-save cycle can produce two
// different files with the same mtime, and a node that trusted mtime alone would
// go on running the previous version while the editor showed the new one.
struct ScriptStamp {
    bool exists = false;
    int64_t modifiedUnixMs = 0;
    uint64_t sizeBytes = 0;

    bool operator== (const ScriptStamp& other) const
    {
        return exists == other.exists && modifiedUnixMs == other.modifiedUnixMs && sizeBytes == other.sizeBytes;
    }
    bool operator!= (const ScriptStamp& other) const
    {
        return !(*this == other);
    }
};

// A script is a text file a person types. The ceiling is generous for that and
// small enough that pointing a node at a bundled library or a video by mistake
// fails in a sentence rather than by consuming a gigabyte inside Archicad.
constexpr uint64_t kMaxScriptBytes = 4 * 1024 * 1024;

struct ScriptRead {
    bool ok = false;
    std::string source;
    ScriptStamp stamp;

    // Present whenever ok is false. Written for the person who has the file open
    // in another window, so it names the path and says what about it was wrong.
    std::string error;
};

// Never throws: a filesystem error becomes `ok == false` with a reason. A path
// that does not exist is an ordinary answer here, not an exception - it is the
// normal state of a script node whose file has been renamed, and the node has to
// be able to report it rather than take the run down.
ScriptStamp StatScript (const std::string& path);
ScriptRead ReadScript (const std::string& path);

// Writes a starter file for `path`, with the header directives already in it, so
// a new script node produces something that runs and has ports before anyone has
// typed anything. REFUSES an existing file: scaffolding must never be able to
// destroy work, and "create" and "overwrite" are not the same request.
bool WriteScriptTemplate (const std::string& path, std::string& error);

// ---------------------------------------------------------------------------
// Change notification.
//
// ⚠️ THE CALLBACK ARRIVES ON THE WATCHER'S OWN THREAD, AND IT IS THE ONLY THING
// IN THIS FILE THAT IS NOT A PLAIN FUNCTION CALL. It must not touch the graph
// document, the evaluator or any DG object; it exists to hand a path to whatever
// marshals onto the thread that may. See ScriptNodes for the one implementation
// and the debounce that goes with it.
//
// Debouncing is the caller's problem and a real one: a single save in VSCode
// produces several notifications - the temporary file, the rename, the attribute
// update - and reloading a script three times per save is both wasteful and
// visible, because each reload can reshape the node's ports.
class IScriptWatcher {
  public:
    virtual ~IScriptWatcher () = default;

    // Watch every directory containing one of these files, replacing whatever
    // was watched before. Directories rather than files because that is what the
    // platform offers, and because an editor's save-and-rename briefly makes the
    // file itself disappear - a per-file watch would stop watching at the exact
    // moment it was meant to fire.
    virtual void WatchPaths (const std::vector<std::string>& paths) = 0;

    // Stops watching and joins. Idempotent, and safe to call from a destructor.
    virtual void Stop () = 0;
};

using ScriptChangeCallback = std::function<void (const std::string& path)>;

// The installed watcher, or nullptr when there is none - the offline suite, or a
// platform with no implementation. Follows ActiveArchicadHost exactly, for the
// same reason: the portable half of the runtime names an interface and the
// platform half installs itself, so nothing here has to link against a
// notification API that only exists on one operating system.
//
// ⚠️ nullptr IS A SUPPORTED STATE, NOT A FAILURE. Without a watcher a script node
// still reloads when it is evaluated and when the user presses Reload; what it
// loses is only the automatic reload on save. The node reports which of those it
// is doing rather than claiming to watch a file nothing is watching.
IScriptWatcher* ActiveScriptWatcher ();
void SetActiveScriptWatcher (IScriptWatcher* watcher);

// The Win32 implementation. Defined in ScriptWatcherWin32.cpp, which is the one
// file in NodeGraph/ that is NOT in the offline C++ suite - so this declaration
// exists offline and nothing there calls it. The add-on constructs one during
// Initialize and installs it above.
std::unique_ptr<IScriptWatcher> MakeWin32ScriptWatcher (ScriptChangeCallback onChange);

} // namespace evp::nodegraph

#endif
