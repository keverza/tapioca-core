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

std::string ScriptTemplateSource (ScriptLanguage language)
{
    // ⚠️ ONE FUNCTION, TWO READERS, AND THAT IS WHY IT IS NOT INLINED INTO THE
    // WRITER. The palette shows this text in an editor buffer BEFORE the folder
    // exists - a script node is scaffolded on the first save, not on placement -
    // so the starter script has to be describable without anything having been
    // written yet. A second copy of it in the browser bundle would be a starter
    // script that drifts from the one the disk actually gets.
    //
    // ⚠️ AND IT MUST RUN AS IT STANDS. The whole point of a template is that a
    // node placed a moment ago already has ports, already computes something and
    // can be wired up straight away - so `x` and `y` are inputs with defaults
    // (an unwired input still evaluates) and `out` is a typed output. A template
    // that needed editing before it did anything would just be a comment block.
    const std::string prefix = ScriptCommentPrefix (language);
    std::string body;
    body += prefix + " @name        New script\n";
    body += prefix + " @description Describe what this node does.\n";
    body += prefix + "\n";
    body += prefix + " Ports are declared here and nowhere else. Save the file and the\n";
    body += prefix + " node reshapes itself to match.\n";
    body += prefix + "\n";
    body += prefix + " @in  x : number = 0   \"X\"\n";
    body += prefix + " @in  y : number = 0   \"Y\"\n";
    // ⚠️ UNTYPED, AND THAT IS THE LESSON THE TEMPLATE IS TEACHING. An output's
    // type is decided by the line that computes it; writing it again here is a
    // restatement that can only ever be wrong. Add `: number` when you want the
    // node's interface PINNED - on a node other people wire into, that is worth
    // having - and leave it off the rest of the time.
    body += prefix + " @out out\n";
    body += "\n";
    if (language == ScriptLanguage::Python) {
        // The node's own folder and the shared library are both on sys.path for
        // the duration of a run, which is what makes these the useful lines to
        // start from: `math` comes from Tapioca's own interpreter, and a helper
        // beside this file imports by its bare name.
        body += "import math\n";
        body += "\n";
        body += "# A helper beside this file imports by name: `import calculations`.\n";
        body += "# So does a shared module in the library's libs folder: `import geometry`.\n";
        body += "\n";
        body += "out = math.hypot(x, y)\n";
    }
    else {
        // No module loader is configured for the JavaScript runtime, deliberately
        // - a loader is also a way to reach the filesystem - so there is nothing
        // honest to import here and the template does not pretend otherwise.
        body += "out = Math.hypot(x, y);\n";
    }
    return body;
}

std::string WithScriptName (const std::string& source, const std::string& name, ScriptLanguage language)
{
    // ⚠️ A REWRITE OF ONE LINE, NEVER A REGENERATION OF THE HEADER. This runs
    // when the user renames the NODE, over a file they may have spent a morning
    // on; anything that rebuilt the comment block would throw away their
    // description, their port labels and whatever else they wrote up there.
    //
    // ⚠️ AND IT IS PURE. It takes text and returns text, so the offline suite
    // covers the cases that are easy to get wrong - no header at all, a shebang
    // first, an @name that is already right - without a filesystem or a running
    // Archicad. The caller does the guarded read/write pair around it.
    const std::string prefix = ScriptCommentPrefix (language);
    const std::string directive = prefix + " @name        " + name;

    std::string result;
    size_t offset = 0;
    size_t lineNumber = 0;
    bool replaced = false;
    size_t insertAt = std::string::npos;

    while (offset < source.size ()) {
        size_t lineEnd = source.find ('\n', offset);
        const bool lastLine = lineEnd == std::string::npos;
        if (lastLine)
            lineEnd = source.size ();
        const std::string raw = source.substr (offset, lineEnd - offset);
        const size_t nextOffset = lastLine ? source.size () : lineEnd + 1;
        ++lineNumber;

        // Trimmed only for the DECISION; the line itself is copied through
        // untouched, so a header indented or padded the way its author likes it
        // stays that way.
        std::string trimmed = raw;
        while (!trimmed.empty () && (trimmed.back () == '\r' || trimmed.back () == ' ' || trimmed.back () == '\t'))
            trimmed.pop_back ();
        size_t start = 0;
        while (start < trimmed.size () && (trimmed[start] == ' ' || trimmed[start] == '\t'))
            ++start;
        trimmed = trimmed.substr (start);

        const bool shebang = lineNumber == 1 && trimmed.rfind ("#!", 0) == 0;
        const bool comment = trimmed.rfind (prefix, 0) == 0;

        // ⚠️ THE SHEBANG IS EXCLUDED BEFORE THE COMMENT TEST, NOT AFTER. In
        // Python it starts with `#` and so LOOKS like the head of the comment
        // block; inserting a name above it would leave a `#!` on line two, which
        // is no longer a shebang at all.
        if (!replaced && comment && !shebang) {
            std::string body = trimmed.substr (prefix.size ());
            size_t bodyStart = 0;
            while (bodyStart < body.size () && (body[bodyStart] == ' ' || body[bodyStart] == '\t'))
                ++bodyStart;
            body = body.substr (bodyStart);
            if (body.rfind ("@name", 0) == 0 && (body.size () == 5 || body[5] == ' ' || body[5] == '\t')) {
                result += directive;
                if (!lastLine)
                    result += '\n';
                offset = nextOffset;
                replaced = true;
                continue;
            }
            // The first comment line that is NOT the name is where a missing
            // @name goes, so an inserted one leads the block it belongs to
            // instead of being appended under the port declarations.
            if (insertAt == std::string::npos)
                insertAt = result.size ();
        }
        else if (!replaced && !comment && !shebang && trimmed.empty () == false) {
            // The leading comment block is over. If there was no @name in it,
            // this is the last moment at which one can still be part of it.
            if (insertAt == std::string::npos)
                insertAt = result.size ();
            result += raw;
            if (!lastLine)
                result += '\n';
            offset = nextOffset;
            break;
        }

        result += raw;
        if (!lastLine)
            result += '\n';
        offset = nextOffset;
    }

    result += source.substr (offset);

    if (!replaced) {
        if (insertAt == std::string::npos)
            insertAt = result.size ();
        result.insert (insertAt, directive + "\n");
    }
    return result;
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

    std::ofstream stream (target, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "could not create " + path;
        return false;
    }
    stream << ScriptTemplateSource (language);
    if (!stream) {
        error = "could not write " + path;
        return false;
    }
    return true;
}

ScriptWrite WriteScriptSource (const std::string& path, const std::string& source, const std::string& baseHash,
                               const std::function<std::string (const std::string&)>& hashSource)
{
    ScriptWrite write;
    if (path.empty ()) {
        write.error = "this script node has no file yet; choose or create one";
        return write;
    }

    ScriptLanguage language = ScriptLanguage::JavaScript;
    if (!ScriptLanguageFromPath (path, language)) {
        write.error = "a script file must end in .js or .py";
        return write;
    }
    if (source.size () > kMaxScriptBytes) {
        write.error = "that is larger than a script node will read (4 MB)";
        return write;
    }

    // ⚠️ THE GUARD, AND IT RUNS BEFORE A SINGLE BYTE IS WRITTEN. What is on disk
    // is re-read here rather than compared by stamp: this decides whether to
    // destroy someone's work, and a coarse timestamp that missed a save would
    // decide it wrongly in the one direction that cannot be undone.
    const ScriptStamp existing = StatScript (path);
    if (existing.exists) {
        const ScriptRead current = ReadScript (path);
        if (!current.ok) {
            write.error = current.error;
            return write;
        }
        const std::string currentHash = hashSource (current.source);
        if (currentHash != baseHash) {
            write.conflict = true;
            write.diskSource = current.source;
            write.diskHash = currentHash;
            write.stamp = current.stamp;
            write.error = "the file changed on disk since it was opened here";
            return write;
        }
    }
    else if (!baseHash.empty ()) {
        // The buffer was read from a file that has since been deleted or moved.
        // Recreating it silently would resurrect a file the user removed on
        // purpose, so this is a conflict with an empty other side.
        write.conflict = true;
        write.diskSource.clear ();
        write.diskHash.clear ();
        write.error = "the file is no longer at " + path;
        return write;
    }

    std::error_code code;
    const fs::path target = PathFromUtf8 (path);
    if (target.has_parent_path ()) {
        fs::create_directories (target.parent_path (), code);
        if (code) {
            write.error = "could not create the folder for " + path;
            return write;
        }
    }

    // ⚠️ TEMPORARY-AND-RENAME, BESIDE THE TARGET RATHER THAN IN THE TEMP FOLDER.
    // Truncating the real file and streaming into it leaves a script node's file
    // empty or half-written if anything fails mid-way, and an external editor
    // watching the folder sees that intermediate state and reloads it. The
    // temporary is a sibling because a rename is only atomic within one volume,
    // and %TEMP% is routinely on another.
    fs::path temporary = target;
    temporary += ".tapioca-save";
    {
        std::ofstream stream (temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            write.error = "could not write beside " + path + "; the folder may be read-only";
            return write;
        }
        stream << source;
        stream.flush ();
        if (!stream) {
            stream.close ();
            fs::remove (temporary, code);
            write.error = "could not write " + path;
            return write;
        }
    }

    fs::rename (temporary, target, code);
    if (code) {
        fs::remove (temporary, code);
        // The usual cause is the file being held open by the editor or by a
        // virus scanner, so the message names the thing worth trying.
        write.error = "could not replace " + path + "; it may be locked by another program";
        return write;
    }

    write.ok = true;
    write.stamp = StatScript (path);
    write.diskHash = hashSource (source);
    return write;
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
