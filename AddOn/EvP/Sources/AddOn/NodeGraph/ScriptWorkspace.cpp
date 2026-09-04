#include "NodeGraph/ScriptWorkspace.hpp"

#include "NodeGraph/ScriptSource.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace evp::nodegraph {
namespace {

namespace fs = std::filesystem;

// Same reason as ScriptSource's: std::filesystem::path built from UTF-8 rather
// than from the narrow locale, so a workflow under a path with non-ASCII
// characters in it opens instead of being silently mangled by the ANSI code page.
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

std::string ReadEnvironment (const char* name)
{
#ifdef _MSC_VER
    // getenv is deprecated under MSVC's secure-CRT warnings and _dupenv_s is the
    // sanctioned form. The buffer is ours to free.
    char* buffer = nullptr;
    size_t size = 0;
    if (_dupenv_s (&buffer, &size, name) != 0 || buffer == nullptr)
        return std::string ();
    std::string value (buffer);
    std::free (buffer);
    return value;
#else
    const char* value = std::getenv (name);
    return value == nullptr ? std::string () : std::string (value);
#endif
}

bool IsScriptFileName (const std::string& name, ScriptLanguage language)
{
    ScriptLanguage implied = ScriptLanguage::Python;
    return ScriptLanguageFromPath (name, implied) && implied == language;
}

// The one segment a shared file's name is allowed to be prefixed with. A literal
// rather than derived from SharedLibraryRoot's last component, because it is a
// name the EDITOR sends and the two would have to agree even if the root moved.
constexpr const char kSharedPrefix[] = "libs/";

} // namespace

std::string DefaultWorkflowRoot ()
{
    const std::string localAppData = ReadEnvironment ("LOCALAPPDATA");
    if (localAppData.empty ())
        return std::string ();
    return Utf8FromPath (PathFromUtf8 (localAppData) / "Tapioca" / "Commands" / "Workflows");
}

std::string SharedLibraryRoot ()
{
    const std::string root = DefaultWorkflowRoot ();
    return root.empty () ? std::string () : Utf8FromPath (PathFromUtf8 (root) / "libs");
}

const char* EntryFileName (ScriptLanguage language)
{
    return language == ScriptLanguage::JavaScript ? "main.js" : "main.py";
}

ScriptWorkspace ResolveScriptWorkspace (const std::string& path, ScriptLanguage language)
{
    ScriptWorkspace workspace;
    workspace.language = language;
    if (path.empty ()) {
        workspace.error = "this script node has no folder yet; choose or create one";
        return workspace;
    }

    fs::path folder = PathFromUtf8 (path);
    if (folder.is_relative ()) {
        // ⚠️ A RELATIVE PATH IS THE NORMAL CASE, NOT A FALLBACK. It is what makes
        // the library location a preset: a node called `apartment_metrics` is a
        // folder of that name in the workflow library, and the graph that holds
        // it can be opened on another machine whose LOCALAPPDATA is elsewhere.
        // An absolute path still works and is what a node outside the library
        // uses; it is simply not what the palette writes.
        const std::string library = DefaultWorkflowRoot ();
        if (library.empty ()) {
            workspace.error = "no workflow library location: %LOCALAPPDATA% is not set, so '" + path +
                              "' cannot be resolved. Use an absolute folder path.";
            return workspace;
        }
        folder = PathFromUtf8 (library) / folder;
    }

    // Lexically, not with fs::canonical: the folder is allowed not to exist yet,
    // and canonical throws on one that does not. This still collapses `.` and
    // `..`, which is what the file-name validation below relies on.
    folder = folder.lexically_normal ();
    if (folder.has_filename () == false)
        folder = folder.parent_path (); // a trailing separator is not a nameless folder

    workspace.root = Utf8FromPath (folder);
    workspace.entryFile = Utf8FromPath (folder / EntryFileName (language));

    workspace.importRoots.push_back (workspace.root);
    const std::string shared = SharedLibraryRoot ();
    if (!shared.empty ()) {
        workspace.importRoots.push_back (shared);
        // The library root itself, so `from libs.geometry import ...` works as a
        // package import as well as `from geometry import ...` - §5 of the plan
        // prefers the package spelling and both must resolve, or a script that
        // reads correctly fails at run time.
        workspace.importRoots.push_back (DefaultWorkflowRoot ());
    }

    workspace.ok = true;
    return workspace;
}

std::vector<WorkspaceFile> ListWorkspaceFiles (const ScriptWorkspace& workspace)
{
    std::vector<WorkspaceFile> files;
    if (!workspace.ok)
        return files;

    const std::string entryName = EntryFileName (workspace.language);
    std::error_code code;

    // The node's own folder. Sorted by name, with the entry file lifted to the
    // front afterwards: tabs whose order changed between two listings of the same
    // folder would move under the user's cursor.
    for (const fs::directory_entry& item : fs::directory_iterator (PathFromUtf8 (workspace.root), code)) {
        if (!item.is_regular_file (code))
            continue;
        const std::string name = Utf8FromPath (item.path ().filename ());
        if (!IsScriptFileName (name, workspace.language))
            continue;
        WorkspaceFile file;
        file.name = name;
        file.entry = name == entryName;
        file.sizeBytes = static_cast<uint64_t> (fs::file_size (item.path (), code));
        files.push_back (std::move (file));
    }
    std::sort (files.begin (), files.end (),
               [] (const WorkspaceFile& left, const WorkspaceFile& right) { return left.name < right.name; });
    const auto entry =
        std::find_if (files.begin (), files.end (), [] (const WorkspaceFile& file) { return file.entry; });
    if (entry != files.end ())
        std::rotate (files.begin (), entry, entry + 1);

    // The shared library, listed after and marked. Present on every node, because
    // that is what "shared" means - a helper you can reach from here whether or
    // not this node imports it yet.
    const std::string shared = SharedLibraryRoot ();
    if (shared.empty ())
        return files;

    std::vector<WorkspaceFile> sharedFiles;
    for (const fs::directory_entry& item : fs::directory_iterator (PathFromUtf8 (shared), code)) {
        if (!item.is_regular_file (code))
            continue;
        const std::string name = Utf8FromPath (item.path ().filename ());
        if (!IsScriptFileName (name, workspace.language))
            continue;
        WorkspaceFile file;
        file.name = std::string (kSharedPrefix) + name;
        file.shared = true;
        file.sizeBytes = static_cast<uint64_t> (fs::file_size (item.path (), code));
        sharedFiles.push_back (std::move (file));
    }
    std::sort (sharedFiles.begin (), sharedFiles.end (),
               [] (const WorkspaceFile& left, const WorkspaceFile& right) { return left.name < right.name; });
    files.insert (files.end (), sharedFiles.begin (), sharedFiles.end ());
    return files;
}

bool ResolveWorkspaceFile (const ScriptWorkspace& workspace, const std::string& name, std::string& absolute,
                           std::string& error)
{
    absolute.clear ();
    if (!workspace.ok) {
        error = workspace.error.empty () ? "this script node has no folder" : workspace.error;
        return false;
    }

    // An EMPTY name means the entry file. The editor opens a node without knowing
    // what its entry is called, and making it guess `main.py` versus `main.js`
    // would put the language rule in the browser.
    if (name.empty ()) {
        absolute = workspace.entryFile;
        return true;
    }

    std::string bare = name;
    bool shared = false;
    if (bare.rfind (kSharedPrefix, 0) == 0) {
        shared = true;
        bare = bare.substr (sizeof (kSharedPrefix) - 1);
    }

    // ⚠️ EVERY REFUSAL BELOW IS LOAD-BEARING. `bare` arrives from a browser, and
    // the only thing between it and the user's filesystem is this block. The test
    // is on the NAME rather than on the resolved path, so there is no normalising
    // step whose behaviour has to be reasoned about: a name containing a
    // separator, a colon or a dot-dot is refused outright, and what survives can
    // only be one file directly inside a folder this workspace already named.
    if (bare.empty () || bare == "." || bare == "..") {
        error = "'" + name + "' is not a file name";
        return false;
    }
    if (bare.find ('/') != std::string::npos || bare.find ('\\') != std::string::npos ||
        bare.find (':') != std::string::npos) {
        error = "'" + name + "' must be a file inside the node's folder, not a path";
        return false;
    }
    if (!IsScriptFileName (bare, workspace.language)) {
        error = "'" + bare + "' is not a " + ScriptLanguageName (workspace.language) + " file";
        return false;
    }

    if (shared) {
        const std::string root = SharedLibraryRoot ();
        if (root.empty ()) {
            error = "there is no shared library folder on this machine";
            return false;
        }
        absolute = Utf8FromPath (PathFromUtf8 (root) / PathFromUtf8 (bare));
        return true;
    }
    absolute = Utf8FromPath (PathFromUtf8 (workspace.root) / PathFromUtf8 (bare));
    return true;
}

ScriptStamp StampWorkspace (const ScriptWorkspace& workspace)
{
    ScriptStamp stamp;
    if (!workspace.ok)
        return stamp;

    // Existence is the ENTRY file's alone. A folder full of helpers and no
    // main.py is a node that cannot run, and reporting it as present would send
    // the user looking for a syntax error in a file that is not there.
    const ScriptStamp entry = StatScript (workspace.entryFile);
    if (!entry.exists)
        return stamp;

    stamp.exists = true;
    stamp.modifiedUnixMs = entry.modifiedUnixMs;
    stamp.sizeBytes = entry.sizeBytes;

    std::error_code code;
    for (const fs::directory_entry& item : fs::directory_iterator (PathFromUtf8 (workspace.root), code)) {
        if (!item.is_regular_file (code))
            continue;
        const std::string name = Utf8FromPath (item.path ().filename ());
        if (!IsScriptFileName (name, workspace.language) || name == EntryFileName (workspace.language))
            continue;
        const ScriptStamp helper = StatScript (Utf8FromPath (item.path ()));
        // Newest write wins, and every size adds. A helper DELETED changes the
        // total even when nothing left behind was touched, which is the case a
        // max-mtime-only stamp misses.
        stamp.modifiedUnixMs = std::max (stamp.modifiedUnixMs, helper.modifiedUnixMs);
        stamp.sizeBytes += helper.sizeBytes;
    }
    return stamp;
}

bool WriteWorkspaceTemplate (const ScriptWorkspace& workspace, std::string& error)
{
    if (!workspace.ok) {
        error = workspace.error.empty () ? "this script node has no folder" : workspace.error;
        return false;
    }

    std::error_code code;
    const fs::path entry = PathFromUtf8 (workspace.entryFile);
    if (fs::exists (entry, code)) {
        error = "there is already a " + std::string (EntryFileName (workspace.language)) + " in " + workspace.root;
        return false;
    }
    fs::create_directories (PathFromUtf8 (workspace.root), code);
    if (code) {
        error = "could not create the folder " + workspace.root;
        return false;
    }

    // WriteScriptTemplate owns what a starter script SAYS, so the two cannot
    // drift; this function owns only where it goes. It refuses an existing file
    // itself, which is a second guard behind the one above.
    return WriteScriptTemplate (workspace.entryFile, error);
}

bool CreateWorkspaceFile (const ScriptWorkspace& workspace, const std::string& name, std::string& absolute,
                          std::string& error)
{
    if (!ResolveWorkspaceFile (workspace, name, absolute, error))
        return false;
    if (name.empty ()) {
        error = "name the new file";
        return false;
    }
    // ⚠️ NEW FILES GO IN THE NODE'S OWN FOLDER, NEVER IN THE SHARED LIBRARY. A
    // helper created from inside one node's editor and silently landing on every
    // other node's import path is a surprise nobody wants twice.
    if (name.rfind (kSharedPrefix, 0) == 0) {
        error = "new files are created in the node's own folder, not in libs";
        absolute.clear ();
        return false;
    }

    std::error_code code;
    const fs::path target = PathFromUtf8 (absolute);
    if (fs::exists (target, code)) {
        error = "there is already a file called " + name;
        absolute.clear ();
        return false;
    }
    fs::create_directories (PathFromUtf8 (workspace.root), code);

    std::ofstream stream (target, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "could not create " + name;
        absolute.clear ();
        return false;
    }
    return true;
}

ScriptMigration MigrateScriptFileToFolder (const std::string& filePath)
{
    ScriptMigration migration;
    if (filePath.empty ()) {
        migration.error = "this script node has no folder yet; choose or create one";
        return migration;
    }

    std::error_code code;
    fs::path source = PathFromUtf8 (filePath);
    if (source.is_relative ()) {
        const std::string library = DefaultWorkflowRoot ();
        if (library.empty ()) {
            migration.error = "%LOCALAPPDATA% is not set, so '" + filePath + "' cannot be resolved";
            return migration;
        }
        source = PathFromUtf8 (library) / source;
    }
    source = source.lexically_normal ();

    // Already a folder, or a path naming something that does not exist: nothing
    // to migrate. Both are ordinary and neither is a failure - the second is what
    // a node looks like between being named and being scaffolded.
    if (!fs::is_regular_file (source, code)) {
        migration.ok = true;
        migration.folder = filePath;
        return migration;
    }

    ScriptLanguage language = ScriptLanguage::Python;
    if (!ScriptLanguageFromPath (Utf8FromPath (source.filename ()), language)) {
        migration.error = "a script file must end in .js or .py";
        return migration;
    }

    // `C:\scripts\offset.py` -> `C:\scripts\offset\main.py`. The folder takes the
    // file's own stem, so the node keeps the name the user gave it and the path
    // in the graph stays recognisably the same thing.
    const fs::path folder = source.parent_path () / source.stem ();
    const fs::path entry = folder / EntryFileName (language);

    if (fs::exists (entry, code)) {
        migration.error = "cannot convert " + filePath + " to a folder: " + Utf8FromPath (entry) + " already exists";
        return migration;
    }
    if (fs::exists (folder, code) && !fs::is_directory (folder, code)) {
        migration.error =
            "cannot convert " + filePath + " to a folder: " + Utf8FromPath (folder) + " exists and is not a folder";
        return migration;
    }

    fs::create_directories (folder, code);
    if (code) {
        migration.error = "could not create " + Utf8FromPath (folder);
        return migration;
    }

    // ⚠️ RENAME, NOT COPY-THEN-DELETE. A copy leaves two files that can diverge if
    // the delete fails, and the user's editor may well have the original open. A
    // rename either happened or did not, and a failure leaves the original
    // exactly where it was.
    fs::rename (source, entry, code);
    if (code) {
        migration.error =
            "could not move " + filePath + " into " + Utf8FromPath (folder) + "; it may be locked by another program";
        return migration;
    }

    migration.ok = true;
    migration.migrated = true;
    migration.folder = Utf8FromPath (folder);
    return migration;
}

} // namespace evp::nodegraph
