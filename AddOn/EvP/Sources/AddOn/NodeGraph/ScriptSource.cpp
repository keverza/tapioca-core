#include "NodeGraph/ScriptSource.hpp"

#include "NodeGraph/ScriptManifest.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace evp::nodegraph {
namespace {

namespace fs = std::filesystem;

// std::filesystem::path is constructed from UTF-8 rather than from the narrow
// locale, so a script under a path with non-ASCII characters in it opens. On
// Windows the narrow overload goes through the ANSI code page, which silently
// mangles exactly the paths a non-English user is most likely to have.
fs::path PathFromUtf8 (const std::string& path)
{
    const char8_t* bytes = reinterpret_cast<const char8_t*> (path.c_str ());
    return fs::path (std::u8string (bytes, bytes + path.size ()));
}

int64_t ToUnixMilliseconds (fs::file_time_type when)
{
    // clock_cast rather than arithmetic on the raw count: a filesystem clock's
    // epoch is unspecified, so the raw number is comparable with itself and with
    // nothing else - and this one crosses the bridge to a browser that will
    // render it as a time.
    const auto systemTime = std::chrono::clock_cast<std::chrono::system_clock> (when);
    return std::chrono::duration_cast<std::chrono::milliseconds> (systemTime.time_since_epoch ()).count ();
}

} // namespace

ScriptStamp StatScript (const std::string& path)
{
    ScriptStamp stamp;
    if (path.empty ())
        return stamp;

    std::error_code code;
    const fs::path target = PathFromUtf8 (path);
    if (!fs::is_regular_file (target, code) || code)
        return stamp;

    const uint64_t size = fs::file_size (target, code);
    if (code)
        return stamp;
    const fs::file_time_type modified = fs::last_write_time (target, code);
    if (code)
        return stamp;

    stamp.exists = true;
    stamp.sizeBytes = size;
    stamp.modifiedUnixMs = ToUnixMilliseconds (modified);
    return stamp;
}

ScriptRead ReadScript (const std::string& path)
{
    ScriptRead read;
    if (path.empty ()) {
        read.error = "this script node has no file yet; choose or create one";
        return read;
    }

    read.stamp = StatScript (path);
    if (!read.stamp.exists) {
        read.error = "no file at " + path;
        return read;
    }
    if (read.stamp.sizeBytes > kMaxScriptBytes) {
        read.error = "the file at " + path + " is larger than a script node will read (4 MB)";
        return read;
    }

    std::ifstream stream (PathFromUtf8 (path), std::ios::binary);
    if (!stream) {
        // The common cause is the editor holding the file mid-save, so the
        // message says the thing worth trying rather than only the thing that
        // happened.
        read.error = "could not open " + path + "; it may be locked by another program";
        return read;
    }
    read.source.assign (std::istreambuf_iterator<char> (stream), std::istreambuf_iterator<char> ());
    if (stream.bad ()) {
        read.error = "could not read " + path;
        read.source.clear ();
        return read;
    }

    // A UTF-8 BOM is what Notepad and several editors write by default. Left in
    // place it is three bytes in front of the first line, which makes the header
    // parser miss a first-line directive and both engines reject the file with a
    // syntax error at character one - about the least informative failure
    // available for the most ordinary of causes.
    if (read.source.rfind ("\xEF\xBB\xBF", 0) == 0)
        read.source.erase (0, 3);

    // ⚠️ THE STAMP IS RE-READ AFTER THE CONTENT, NOT BEFORE. A save that lands
    // between the stat and the read would otherwise leave the node holding new
    // text under an old stamp - which is the one state that never resolves,
    // because every later comparison says the file is unchanged. Re-reading
    // means the worst case is one redundant reload, not a permanently stale node.
    const ScriptStamp after = StatScript (path);
    if (after != read.stamp) {
        read.stamp = after;
        read.error = "the file changed while it was being read";
        read.source.clear ();
        return read;
    }

    read.ok = true;
    return read;
}

bool WriteScriptTemplate (const std::string& path, std::string& error)
{
    ScriptLanguage language = ScriptLanguage::JavaScript;
    if (!ScriptLanguageFromPath (path, language)) {
        error = "a script file must end in .js or .py";
        return false;
    }

    std::error_code code;
    const fs::path target = PathFromUtf8 (path);
    if (fs::exists (target, code)) {
        error = "there is already a file at " + path;
        return false;
    }
    if (target.has_parent_path ()) {
        fs::create_directories (target.parent_path (), code);
        if (code) {
            error = "could not create the folder for " + path;
            return false;
        }
    }

    const std::string prefix = ScriptCommentPrefix (language);
    std::string body;
    body += prefix + " @name        New script\n";
    body += prefix + " @description Describe what this node does.\n";
    body += prefix + "\n";
    body += prefix + " Ports are declared here and nowhere else. Save the file and the\n";
    body += prefix + " node reshapes itself to match.\n";
    body += prefix + "\n";
    body += prefix + " @in  value : number = 1   \"Value\"\n";
    body += prefix + " @out result : number\n";
    body += "\n";
    if (language == ScriptLanguage::Python)
        body += "result = value * 2\n";
    else
        body += "result = value * 2;\n";

    std::ofstream stream (target, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "could not create " + path;
        return false;
    }
    stream << body;
    if (!stream) {
        error = "could not write " + path;
        return false;
    }
    return true;
}

namespace {

// Atomic for the reason ArchicadHost's is: read whenever the watched set is
// rebuilt and written twice in the process lifetime.
std::atomic<IScriptWatcher*> gWatcher { nullptr };

} // namespace

IScriptWatcher* ActiveScriptWatcher ()
{
    return gWatcher.load (std::memory_order_acquire);
}

void SetActiveScriptWatcher (IScriptWatcher* watcher)
{
    gWatcher.store (watcher, std::memory_order_release);
}

} // namespace evp::nodegraph
