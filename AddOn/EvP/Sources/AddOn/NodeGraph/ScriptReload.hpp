#ifndef EVP_NODEGRAPH_SCRIPTRELOAD_HPP
#define EVP_NODEGRAPH_SCRIPTRELOAD_HPP

// Picking up a saved file: read it, parse its header, reshape the node, and mark
// it for re-evaluation.
//
// ⚠️ THIS IS THE ONLY PLACE THAT TURNS A FILE ON DISK INTO A CHANGE IN THE
// DOCUMENT, and there are three callers that each think they are the only one: the
// Reload button, the file watcher, and the evaluation path that checks staleness
// before running. Left to themselves they would each have an opinion about what a
// broken header means and what happens to the wires, and the user would meet
// whichever one fired first.
//
// ⚠️ A BROKEN HEADER DOES NOT DESTROY THE NODE'S PORTS. When the file parses, the
// node takes the file's interface and edges that no longer fit are dropped - that
// is the deal, and the user is told. When it does NOT parse, the ports stay
// exactly as they were and the node reports the diagnostic instead. The reason is
// that a header is broken most often halfway through being edited, and a node
// that shed every wire on each intermediate save would make the file unusable to
// work on.

#include "NodeGraph/Graph.hpp"
#include "NodeGraph/ScriptNodes.hpp"

#include <string>
#include <vector>

namespace evp::nodegraph {

struct ScriptReloadResult {
    bool ok = false;

    // Why the reload could not be attempted at all - an unknown node, a node that
    // is not a script node. NOT the same as a file that failed to load, which is
    // an ok reload of a node that now reports a problem: the first is a caller
    // mistake and the second is the ordinary state of a script being written.
    std::string error;

    ScriptState state;
    std::vector<Edge> droppedEdges;

    // Whether the node's ports actually moved. The editor animates a reshape and
    // says nothing at all about a save that only changed the body, which is most
    // of them.
    bool interfaceChanged = false;

    // Set when this reload converted a single-FILE node into a node FOLDER: the
    // path the node used to hold, and the folder it holds now.
    //
    // ⚠️ REPORTED BECAUSE IT MOVED THE USER'S FILE. A migration is silent from
    // the graph's point of view - same node, same ports, same wires - and loud
    // from the filesystem's: offset.py is not where it was, and the editor the
    // user has it open in still thinks it is. Saying so is the difference
    // between a conversion and a file that appears to have vanished.
    std::string migratedFrom;
    std::string migratedTo;
};

ScriptReloadResult ReloadScriptNode (const GraphId& graphId, const NodeId& nodeId);

// Reloads every script node whose file has changed since it was loaded. The
// return is the nodes that were actually reloaded, so a caller can report "3
// scripts reloaded" rather than re-deriving it.
std::vector<NodeId> ReloadStaleScriptNodes (const GraphId& graphId);

// Hands the watcher the current set of script paths. Cheap and idempotent; call
// it after anything that could add, remove or repoint a script node.
void SyncScriptWatchList ();

// Adopts the script nodes of a document that just arrived - one loaded from the
// library, or restored at startup - so that their state exists before anything
// tries to evaluate them. Loads each file once; a node whose file is missing ends
// up with the load error it will report, which is the correct thing to show for a
// graph whose scripts did not travel with it.
void AdoptScriptNodes (const GraphId& graphId);

} // namespace evp::nodegraph

#endif
