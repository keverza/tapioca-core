#ifndef EVP_NODEGRAPH_SCRIPTWORKSPACE_HPP
#define EVP_NODEGRAPH_SCRIPTWORKSPACE_HPP

// A script node is a FOLDER, not a file.
//
// ⚠️ THIS IS THE MODEL THE WHOLE FEATURE IS BUILT ON, AND IT REPLACED A FILE ONE.
// A node used to be one `.py`, which is fine until the second function wants a
// home: everything after that is either a thousand-line file or a `sys.path`
// hack in user code. A folder gives the node somewhere to put helpers, gives the
// interpreter a root it can import from, and gives the editor something to draw
// tabs for - and it costs one rule, which is that the folder's `main.py` is the
// entry point and nothing else is.
//
//   <workflows>/                     the library root, see DefaultWorkflowRoot
//   ├── libs/                        shared by every node, read-only in the palette
//   │   └── geometry.py
//   └── apartment_metrics/           ONE NODE
//       ├── main.py                  the entry point, always
//       ├── calculations.py          helpers, importable as `calculations`
//       └── main.js                  ← never: one language per folder
//
// ⚠️ THE IMPORT ROOTS ARE COMPUTED HERE AND NOWHERE ELSE. The runtime puts them
// on `sys.path` and a future language server will be configured from the same
// list; the moment those two are derived separately they drift, and the symptom
// is an editor that reports an import error for a line that runs perfectly well
// (or worse, the reverse). One function, both consumers.
//
// ⚠️ AND EVERY PATH THAT CROSSES THE BRIDGE IS VALIDATED AGAINST THE FOLDER. The
// editor names a file to read, write or create; those names come from a browser
// and must not be able to name `..\..\..\Windows\System32\drivers\etc\hosts`.
// ResolveWorkspaceFile is the only way to turn a name into a path, and it
// refuses anything that is not a plain file directly inside the workspace.
//
// DevKit-free and Win32-free: std::filesystem and getenv only, so the offline
// suite covers the resolution, the migration and - most importantly - the
// refusals.

#include "NodeGraph/ScriptManifest.hpp"
#include "NodeGraph/ScriptSource.hpp"

#include <string>
#include <vector>

namespace evp::nodegraph {

// %LOCALAPPDATA%\Tapioca\Commands\Workflows, the library every script node folder
// lives in by default. Empty when the environment does not say, in which case a
// node needs an absolute path and says so rather than inventing a location.
//
// ⚠️ INSIDE Commands\, NOT BESIDE IT. That is what puts workflow node folders in
// the tree `scripts\Sync-All.ps1` already deploys, so a node folder is shared,
// versioned and installed exactly the way a command is. It is deliberately NOT
// %LOCALAPPDATA%\Tapioca\Workflows, which is the saved-GRAPH library
// (FileGraphStore) and holds documents rather than code.
std::string DefaultWorkflowRoot ();

// The shared library folder: <workflows>\libs. On the import path of every node,
// which is what makes `from libs.geometry import polygon_area` resolve the same
// way for every workflow instead of each node carrying its own copy.
std::string SharedLibraryRoot ();

// The entry file's name for a language: main.py or main.js. One name, hard-coded
// rather than configurable - a per-node entry point setting is a second thing to
// get wrong and buys nothing a folder name does not already say.
const char* EntryFileName (ScriptLanguage language);

struct ScriptWorkspace {
    bool ok = false;

    // The node's own folder, absolute. Empty when resolution failed.
    std::string root;

    // <root>\main.py or <root>\main.js, absolute.
    std::string entryFile;

    ScriptLanguage language = ScriptLanguage::Python;

    // What the runtime puts on sys.path, in order: the node's folder first, then
    // the shared library root and the workflows root above it. First wins, so a
    // node may shadow a shared helper on purpose - and cannot be shadowed BY one
    // by accident.
    std::vector<std::string> importRoots;

    // Why resolution failed. Written for someone looking at a node that will not
    // load, so it names the path and says what about it was wrong.
    std::string error;
};

// Turns a node's stored `scriptPath` into a workspace.
//
// `path` may be absolute, or relative - a bare `apartment_metrics` resolves
// inside DefaultWorkflowRoot, which is what makes the library location a preset
// rather than something every node has to spell out.
//
// The folder need not exist yet: a node pointed at a folder nobody has created
// resolves fine and reports the missing entry file when it is read. That is the
// state a node is in for the half second between being named and being
// scaffolded, and failing here would make Create unable to use the same path the
// user typed.
ScriptWorkspace ResolveScriptWorkspace (const std::string& path, ScriptLanguage language);

// One file the editor may open as a tab.
struct WorkspaceFile {
    // The name as the editor shows it: `main.py`, or `libs/geometry.py` for a
    // shared file. Never an absolute path - the browser has no business holding
    // one, and a tab labelled C:\Users\... is unreadable at any width.
    std::string name;
    bool entry = false;

    // From the shared library root rather than the node's own folder. Editable
    // in principle and marked in the UI, because a change to it reaches every
    // node that imports it - which is exactly the edit someone makes by accident.
    bool shared = false;

    uint64_t sizeBytes = 0;
};

// Every source file the editor should offer as a tab: the node's own folder
// first (entry file always first within it), then the shared library root.
//
// Not recursive, and that is a decision rather than a simplification: a node
// whose folder has grown subdirectories has outgrown a canvas editor, and the
// answer there is the external IDE that has always been the primary one.
std::vector<WorkspaceFile> ListWorkspaceFiles (const ScriptWorkspace& workspace);

// Turns a name from the editor into an absolute path inside the workspace.
//
// ⚠️ THE REFUSALS ARE THE POINT OF THIS FUNCTION. It accepts a plain file name in
// the node's folder, or `libs/<name>` in the shared root, and NOTHING else: no
// absolute path, no `..`, no nested directory, no extension the script family
// does not own. A browser inside Archicad asking to read `..\..\palette.json` has
// to fail here, because there is no layer below this one that knows the request
// came from a browser at all.
bool ResolveWorkspaceFile (const ScriptWorkspace& workspace, const std::string& name, std::string& absolute,
                           std::string& error);

// One stamp for the WHOLE folder: the entry file's existence, the newest write
// time across every source file in it, and their total size.
//
// ⚠️ THE STAMP COVERS THE HELPERS, NOT JUST main.py, AND THAT IS THE REASON THIS
// EXISTS. Under the file model "has it changed" was one stat. Under the folder
// model a node's behaviour can change entirely without main.py being touched -
// the edit was in calculations.py - and a node that only watched its entry file
// would go on running the previous helper while the editor showed the new one.
// Aggregating into the same ScriptStamp shape means every staleness comparison
// already written keeps working unchanged.
ScriptStamp StampWorkspace (const ScriptWorkspace& workspace);

// Writes a starter node folder: the entry file with its header directives, so a
// new node produces something that runs and has ports before anyone has typed.
// REFUSES a folder that already holds an entry file - "create" and "overwrite"
// are not the same request, and this one must never be able to destroy work.
bool WriteWorkspaceTemplate (const ScriptWorkspace& workspace, std::string& error);

// Creates one new EMPTY helper file inside the node's folder. Refuses an existing
// file, a shared path, and anything ResolveWorkspaceFile refuses.
bool CreateWorkspaceFile (const ScriptWorkspace& workspace, const std::string& name, std::string& absolute,
                          std::string& error);

// ---------------------------------------------------------------------------
// Migration off the file model.

struct ScriptMigration {
    bool ok = false;

    // True when a file was actually moved. False with ok means there was nothing
    // to do - the path was already a folder - which is the common case and must
    // not read as a failure.
    bool migrated = false;

    // The folder the node should now point at. The caller rewrites the node's
    // `scriptPath` to this through the ordinary parameter edit, so the change is
    // validated, revisioned and undoable like any other.
    std::string folder;

    std::string error;
};

// Converts `C:\scripts\offset.py` into `C:\scripts\offset\main.py`.
//
// ⚠️ IT MOVES THE USER'S FILE, WHICH MAKES IT THE MOST DANGEROUS FUNCTION IN THE
// SCRIPT FAMILY. So: it refuses if the destination folder already contains an
// entry file, it renames rather than copies (so there is never a moment with two
// divergent copies), and a failure leaves the original exactly where it was. The
// one thing it cannot do is put the file back after a successful move, which is
// why the refusals come first.
ScriptMigration MigrateScriptFileToFolder (const std::string& filePath);

} // namespace evp::nodegraph

#endif
