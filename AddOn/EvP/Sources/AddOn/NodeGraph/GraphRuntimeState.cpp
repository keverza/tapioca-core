#include "NodeGraph/GraphRuntimeState.hpp"

#include "NodeGraph/ArchicadHost.hpp"
#include "NodeGraph/ArchicadNodes.hpp"
#include "NodeGraph/ElementClassification.hpp"
#include "NodeGraph/GraphAlgorithms.hpp"
#include "NodeGraph/NodeExecution.hpp"
#include "NodeGraph/PreviewProjection.hpp"
#include "Preview/GraphPreviewStore.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <variant>

namespace evp::nodegraph {

const char* const kDefaultGraphId = "default";

GraphRuntimeState& GraphRuntimeState::Get ()
{
    static GraphRuntimeState state;
    return state;
}

GraphRuntimeState::GraphRuntimeState ()
    : registry_ (MakeRuntimeNodeRegistry ()), library_ (std::make_unique<FileGraphStore> ())
{
}

GraphRuntimeState::Slot& GraphRuntimeState::SlotFor (const GraphId& graphId) const
{
    std::lock_guard lock (mapMutex_);
    std::unique_ptr<Slot>& slot = graphs_[graphId];
    if (!slot)
        slot = std::make_unique<Slot> ();
    return *slot;
}

NodeRegistry GraphRuntimeState::Catalog () const
{
    return registry_;
}

std::vector<GraphId> GraphRuntimeState::GraphIds () const
{
    std::lock_guard lock (mapMutex_);
    std::vector<GraphId> ids;
    for (const auto& [graphId, slot] : graphs_) {
        (void) slot;
        ids.push_back (graphId);
    }
    return ids;
}

GraphDocument GraphRuntimeState::Document (const GraphId& graphId) const
{
    Slot& slot = SlotFor (graphId);
    std::lock_guard lock (slot.documentMutex);
    return slot.document;
}

GraphRuntimeState::SelectionActionResult
GraphRuntimeState::ApplySelectionAction (const GraphId& graphId, const NodeId& nodeId, SelectionAction action)
{
    SelectionActionResult result;

    Slot& slot = SlotFor (graphId);
    std::vector<ArchicadElementRef> stored;
    {
        std::lock_guard lock (slot.documentMutex);
        const Node* node = slot.document.FindNode (nodeId);
        if (node == nullptr) {
            result.error = "there is no node called '" + nodeId + "'";
            return result;
        }
        if (node->nodeType != kSelectionSetNodeType) {
            result.error = "node '" + nodeId + "' is not a selection set";
            return result;
        }
        const auto parameter = node->parameters.find (kSelectionSetParameter);
        if (parameter != node->parameters.end ())
            stored = ElementsFromValue (parameter->second);
        result.revision = slot.document.Revision ();
    }

    IArchicadHost* host = ActiveArchicadHost ();
    const bool needsHost = action != SelectionAction::Clear;
    if (needsHost && (host == nullptr || !host->IsAvailable ())) {
        result.error = "no Archicad project is open";
        return result;
    }

    if (action == SelectionAction::Reselect) {
        if (stored.empty ()) {
            result.error = "the set is empty, so there is nothing to select";
            return result;
        }
        // Resolved first and all at once: a stale reference fails the whole
        // action rather than quietly selecting the subset that still exists,
        // because a partial selection looks like a correct answer.
        std::vector<Reference> references;
        references.reserve (stored.size ());
        for (const ArchicadElementRef& element : stored)
            references.push_back (Reference { ReferenceKind::Element, element.guid, {} });
        const std::vector<ReferenceResolution> resolutions = host->References ().ResolveAll (references);
        for (size_t i = 0; i < resolutions.size (); ++i) {
            if (!resolutions[i].Usable ())
                result.missing.push_back (stored[i].guid);
        }
        if (!result.missing.empty ()) {
            result.count = stored.size ();
            result.error =
                "the set names " + std::to_string (result.missing.size ()) + " element(s) this project no longer has";
            return result;
        }
        if (!host->SetSelection (stored, result.error))
            return result;
        result.ok = true;
        result.count = stored.size ();
        return result;
    }

    std::vector<ArchicadElementRef> next;
    if (action == SelectionAction::Clear) {
        result.changed = stored.size ();
    }
    else {
        std::vector<ArchicadElementRef> current;
        if (!host->GetSelection (current, result.error))
            return result;

        const auto contains = [] (const std::vector<ArchicadElementRef>& list, const std::string& guid) {
            return std::any_of (list.begin (), list.end (),
                                [&guid] (const ArchicadElementRef& e) { return e.guid == guid; });
        };

        if (action == SelectionAction::Update) {
            next = current;
            result.changed = current.size ();
        }
        else if (action == SelectionAction::Add) {
            next = stored;
            for (const ArchicadElementRef& element : current) {
                if (contains (next, element.guid))
                    continue;
                next.push_back (element);
                ++result.changed;
            }
        }
        else {
            for (const ArchicadElementRef& element : stored) {
                if (contains (current, element.guid))
                    ++result.changed;
                else
                    next.push_back (element);
            }
        }
    }

    // WHAT each element is, captured HERE - on the press that has already
    // crossed to the host - rather than looked up later.
    //
    // ⚠️ THIS IS WHAT KEEPS THE SELECTION NODE Pure. The per-type containers need
    // every element's type; asking for it at evaluation time would make the node
    // depend on the model and go dirty every time the user clicked in it, which
    // is exactly the behaviour the captured set was designed to avoid. So the
    // type is captured with the guid and stored beside it. See
    // kSelectionTypesParameter for the cost: the types are as old as the capture.
    //
    // A failed read is NOT a failed action. The set changed correctly; only the
    // grouping is unknown, so every element falls into the "Other" container and
    // the next Update fixes it. Refusing the capture over a grouping would be a
    // worse trade.
    std::vector<std::string> capturedTypes (next.size (), kUnclassifiedElementTypeId);
    if (!next.empty () && host != nullptr && host->IsAvailable ()) {
        std::vector<ElementDescription> descriptions;
        std::string ignored;
        if (host->DescribeElements (next, descriptions, ignored) && descriptions.size () == next.size ()) {
            for (size_t i = 0; i < descriptions.size (); ++i)
                capturedTypes[i] = descriptions[i].elementType;
        }
    }

    // Through the ordinary validated edit, so the revision moves, the dirty set
    // is computed the usual way, and the change persists with the graph like any
    // other parameter.
    const EditResult edit =
        Apply (graphId, GraphEdit { SetParameterEdit { nodeId, kSelectionSetParameter, ValueFromElements (next) } });
    if (!edit.accepted) {
        result.error = edit.error;
        return result;
    }
    // Second, and unconditional once the first was accepted: the two parameters
    // are PARALLEL, and leaving the old type list beside a new guid list would
    // file every element under its predecessor's container.
    const EditResult typesEdit = Apply (
        graphId, GraphEdit { SetParameterEdit { nodeId, kSelectionTypesParameter, ValueFromTypes (capturedTypes) } });
    if (!typesEdit.accepted) {
        result.error = typesEdit.error;
        return result;
    }
    result.revision = typesEdit.revision;
    result.count = next.size ();

    // The button IS the run. Evaluating what the change can reach - and nothing
    // else - is what makes pressing Update show its result without anybody
    // pressing Evaluate afterwards.
    EvaluationRequest request;
    request.targets = TerminalNodesDownstreamOf (Document (graphId), nodeId);
    result.evaluation = Evaluate (graphId, request);
    result.ok = true;
    return result;
}

GraphMetadata GraphRuntimeState::Metadata (const GraphId& graphId) const
{
    Slot& slot = SlotFor (graphId);
    std::lock_guard lock (slot.documentMutex);
    return slot.metadata;
}

void GraphRuntimeState::SetMetadata (const GraphId& graphId, GraphMetadata metadata)
{
    Slot& slot = SlotFor (graphId);
    std::lock_guard lock (slot.documentMutex);
    slot.metadata = std::move (metadata);
}

void GraphRuntimeState::SetLibrary (std::unique_ptr<IGraphStore> library)
{
    std::lock_guard lock (libraryMutex_);
    library_ = std::move (library);
}

IGraphStore& GraphRuntimeState::Library () const
{
    std::lock_guard lock (libraryMutex_);
    return *library_;
}

StoreResult GraphRuntimeState::SaveToLibrary (const GraphId& graphId, const std::string& name)
{
    Slot& slot = SlotFor (graphId);
    GraphDocument document;
    GraphMetadata metadata;
    {
        std::lock_guard lock (slot.documentMutex);
        document = slot.document;
        metadata = slot.metadata;
    }
    // Serialised OUTSIDE the document lock, on a copy: writing a file is slow
    // enough that holding the lock across it would stall every edit the user
    // makes while a large workflow is being saved.
    return Library ().Save (name, document, metadata);
}

StoreResult GraphRuntimeState::LoadFromLibrary (const std::string& name, const GraphId& graphId)
{
    SerializedGraph loaded;
    const StoreResult result = Library ().Load (name, registry_, loaded);
    if (!result.Ok ())
        return result;

    Slot& slot = SlotFor (graphId);
    std::lock_guard lock (slot.documentMutex);
    slot.document = std::move (loaded.document);
    slot.metadata = std::move (loaded.metadata);
    // The cache belonged to the previous program.
    slot.evaluator.Reset ();
    // A saved graph carries ids this runtime generated in an earlier session.
    // Naming from 1 again would collide with its own file.
    SeedNodeOrdinalLocked (slot);
    return result;
}

// The graph's results -> the viewport, in the one place that holds the document
// and the evaluator together.
//
// ⚠️ CALLED WITH slot.documentMutex HELD, AND FROM EVERY PATH THAT CHANGES WHAT
// THE GRAPH SHOWS - a run, and an edit. The edit case is the one that is easy to
// forget and impossible to miss once seen: deleting a Preview node without
// republishing leaves its geometry on screen, attached to a node that no longer
// exists, until something else happens to run. Stale preview is worse than none,
// because nothing about it says it is stale.
void GraphRuntimeState::PublishPreviewLocked (const GraphId& graphId, Slot& slot)
{
    const PreviewProjection projection = ProjectGraphPreview (
        graphId, slot.document, registry_, [&slot] (const NodeId& nodeId) { return slot.evaluator.Result (nodeId); });
    evp::preview::GraphPreviewStore::Get ().PublishGraph (graphId, projection.primitives);
}

// The readable half of a generated id: the last segment of the node type, so
// `math.add` names `add-7` rather than `math.add-7` or an opaque `n7`. A node id
// appears in error messages, in saved files and in every wire, and one a person
// can read is worth the few lines it costs to make.
namespace {
std::string ShortTypeName (const std::string& nodeType)
{
    const size_t dot = nodeType.rfind ('.');
    const std::string tail = dot == std::string::npos ? nodeType : nodeType.substr (dot + 1);
    std::string cleaned;
    cleaned.reserve (tail.size ());
    for (const char character : tail) {
        // Anything that is not a letter, a digit or a dash would make an id that
        // is awkward to quote; there is no reason to carry it into a name.
        if (std::isalnum (static_cast<unsigned char> (character)) != 0 || character == '-')
            cleaned.push_back (static_cast<char> (std::tolower (static_cast<unsigned char> (character))));
    }
    return cleaned.empty () ? std::string ("node") : cleaned;
}

// The trailing `-<digits>` of an id, if it has one. Used to lift the ordinal
// above a loaded document's own names.
std::optional<uint64_t> TrailingOrdinal (const NodeId& nodeId)
{
    const size_t dash = nodeId.rfind ('-');
    if (dash == std::string::npos || dash + 1 >= nodeId.size ())
        return std::nullopt;
    const std::string digits = nodeId.substr (dash + 1);
    if (!std::all_of (digits.begin (), digits.end (),
                      [] (char character) { return std::isdigit (static_cast<unsigned char> (character)) != 0; }))
        return std::nullopt;
    // A very long run of digits is not an ordinal this ever wrote; refusing to
    // parse it is safer than overflowing on a name from somewhere else.
    if (digits.size () > 18)
        return std::nullopt;
    return std::stoull (digits);
}
// Rewrite every node reference in `edit` through the aliases this transaction
// has assigned so far.
//
// A PASTE ADDS NODES AND WIRES THEM IN ONE TRANSACTION, so the wires name nodes
// whose real ids did not exist when the client built the batch. Resolution is
// strictly backwards-looking: an edit can only refer to an alias an EARLIER edit
// introduced, which is the same order the client wrote them in and keeps the
// rule "an alias means one node" true no matter how the batch is arranged.
//
// A name that is not an alias is left exactly as it is - that is how an edit
// refers to a node the document already had.
void ResolveAliases (GraphEdit& edit, const std::map<std::string, NodeId>& aliases)
{
    const auto resolve = [&aliases] (NodeId& nodeId) {
        if (const auto found = aliases.find (nodeId); found != aliases.end ())
            nodeId = found->second;
    };
    const auto resolveEdge = [&resolve] (Edge& edge) {
        resolve (edge.sourceNode);
        resolve (edge.targetNode);
    };

    std::visit (
        [&] (auto& operation) {
            using T = std::decay_t<decltype (operation)>;
            if constexpr (std::is_same_v<T, AddNodeEdit>) {
                // Nothing to resolve: this edit is what DEFINES an alias.
            }
            else if constexpr (std::is_same_v<T, RemoveElementsEdit>) {
                for (NodeId& nodeId : operation.nodeIds)
                    resolve (nodeId);
                for (Edge& edge : operation.edges)
                    resolveEdge (edge);
            }
            else if constexpr (std::is_same_v<T, ConnectEdit> || std::is_same_v<T, DisconnectEdit>) {
                resolveEdge (operation.edge);
            }
            else {
                // Every remaining edit names exactly one node.
                resolve (operation.nodeId);
            }
        },
        edit.data);
}

} // namespace

NodeId GraphRuntimeState::GenerateNodeIdLocked (Slot& slot, const std::string& nodeType)
{
    const std::string stem = ShortTypeName (nodeType);
    // The loop is the belt to the counter's braces: the counter alone is right
    // unless a document arrived carrying a name that looks like one of ours,
    // and a duplicate id is refused by ApplyEdit rather than silently merged.
    for (;;) {
        NodeId candidate = stem + "-" + std::to_string (slot.nextNodeOrdinal++);
        if (slot.document.FindNode (candidate) == nullptr)
            return candidate;
    }
}

void GraphRuntimeState::SeedNodeOrdinalLocked (Slot& slot)
{
    uint64_t highest = 0;
    for (const auto& [nodeId, node] : slot.document.Nodes ()) {
        if (const std::optional<uint64_t> ordinal = TrailingOrdinal (nodeId); ordinal.has_value ())
            highest = std::max (highest, *ordinal);
    }
    slot.nextNodeOrdinal = highest + 1;
}

void GraphRuntimeState::PushHistoryLocked (Slot& slot, const GraphDocument& before, const std::string& label,
                                           const std::string& coalesceKey)
{
    // A new edit makes the redo branch unreachable. Keeping it would let Ctrl+Y
    // after an edit reapply a future that no longer follows from the present.
    slot.redoStack.clear ();

    // Coalesce: the previous entry already holds the document from BEFORE the
    // gesture started, so folding into it is simply declining to push. This is
    // what makes one slider drag one Ctrl+Z rather than two hundred.
    if (!coalesceKey.empty () && !slot.undoStack.empty () && slot.undoStack.back ().coalesceKey == coalesceKey)
        return;

    slot.undoStack.push_back (HistoryEntry { before, label, coalesceKey });
    ++slot.stepsRecorded;

    // Drop from the OLD end. A deque would avoid the shift, but the depth is
    // small and bounded and the copy dominates either way.
    while (slot.undoStack.size () > slot.undoDepth)
        slot.undoStack.erase (slot.undoStack.begin ());
}

BatchEditResult GraphRuntimeState::StepHistoryLocked (const GraphId& graphId, Slot& slot, const GraphDocument& target)
{
    // ⚠️ EVERY NODE ON BOTH SIDES IS DIRTY, WHICH IS DELIBERATELY MORE THAN THE
    // MINIMUM. Working out the true difference means comparing parameters, wires
    // and modes node by node, and an under-invalidation here does not look like
    // a bug - it looks like a node that quietly kept the result of a graph that
    // no longer exists. Over-invalidating costs a recompute; under-invalidating
    // costs the user's trust in every number on screen.
    std::vector<NodeId> dirty;
    dirty.reserve (slot.document.Nodes ().size () + target.Nodes ().size ());
    for (const auto& [nodeId, node] : slot.document.Nodes ())
        dirty.push_back (nodeId);
    for (const auto& [nodeId, node] : target.Nodes ())
        dirty.push_back (nodeId);
    std::sort (dirty.begin (), dirty.end ());
    dirty.erase (std::unique (dirty.begin (), dirty.end ()), dirty.end ());

    slot.document.RestoreContent (target);
    slot.evaluator.Invalidate (slot.document, dirty);
    PublishPreviewLocked (graphId, slot);

    BatchEditResult result;
    result.accepted = true;
    result.dirtyNodes = std::move (dirty);
    result.revision = slot.document.Revision ();
    return result;
}

BatchEditResult GraphRuntimeState::Undo (const GraphId& graphId)
{
    Slot& slot = SlotFor (graphId);
    std::lock_guard lock (slot.documentMutex);

    BatchEditResult refused;
    refused.revision = slot.document.Revision ();
    if (slot.undoStack.empty ()) {
        refused.code = batchcode::kNothingToUndo;
        refused.error = "there is nothing to undo";
        return refused;
    }

    HistoryEntry entry = std::move (slot.undoStack.back ());
    slot.undoStack.pop_back ();
    // The CURRENT document becomes the redo step, under the label of the edit
    // being undone, so "Redo Delete node" names the same act "Undo Delete node"
    // did rather than the one before it.
    slot.redoStack.push_back (HistoryEntry { slot.document, entry.label, std::string () });
    return StepHistoryLocked (graphId, slot, entry.document);
}

BatchEditResult GraphRuntimeState::Redo (const GraphId& graphId)
{
    Slot& slot = SlotFor (graphId);
    std::lock_guard lock (slot.documentMutex);

    BatchEditResult refused;
    refused.revision = slot.document.Revision ();
    if (slot.redoStack.empty ()) {
        refused.code = batchcode::kNothingToRedo;
        refused.error = "there is nothing to redo";
        return refused;
    }

    HistoryEntry entry = std::move (slot.redoStack.back ());
    slot.redoStack.pop_back ();
    // ⚠️ PUSHED DIRECTLY, NOT THROUGH PushHistoryLocked, WHICH CLEARS THE REDO
    // STACK. A redo is not a new edit; clearing here would make the second
    // consecutive redo impossible.
    slot.undoStack.push_back (HistoryEntry { slot.document, entry.label, std::string () });
    return StepHistoryLocked (graphId, slot, entry.document);
}

HistoryState GraphRuntimeState::History (const GraphId& graphId) const
{
    Slot& slot = SlotFor (graphId);
    std::lock_guard lock (slot.documentMutex);
    HistoryState state;
    state.canUndo = !slot.undoStack.empty ();
    state.canRedo = !slot.redoStack.empty ();
    if (state.canUndo)
        state.undoLabel = slot.undoStack.back ().label;
    if (state.canRedo)
        state.redoLabel = slot.redoStack.back ().label;
    state.depth = slot.undoDepth;
    state.undoCount = slot.undoStack.size ();
    state.redoCount = slot.redoStack.size ();
    state.stepsRecorded = slot.stepsRecorded;
    return state;
}

void GraphRuntimeState::SetHistoryDepth (const GraphId& graphId, size_t depth)
{
    Slot& slot = SlotFor (graphId);
    std::lock_guard lock (slot.documentMutex);
    slot.undoDepth = std::clamp (depth, kMinUndoDepth, kMaxUndoDepth);
    // ⚠️ APPLIED NOW, NOT AT THE NEXT EDIT. A user who lowers the depth to free
    // memory has asked for those copies to go; leaving them until something
    // else happens to push the stack would be a setting that has not taken
    // effect and no way to tell.
    while (slot.undoStack.size () > slot.undoDepth)
        slot.undoStack.erase (slot.undoStack.begin ());
}

BatchEditResult GraphRuntimeState::ApplyBatch (const GraphId& graphId, std::optional<uint64_t> expectedRevision,
                                               const std::vector<GraphEdit>& edits, const std::string& label,
                                               const std::string& coalesceKey)
{
    Slot& slot = SlotFor (graphId);
    std::lock_guard lock (slot.documentMutex);

    BatchEditResult result;
    result.revision = slot.document.Revision ();

    if (expectedRevision.has_value () && *expectedRevision != slot.document.Revision ()) {
        result.code = batchcode::kRevisionConflict;
        result.error = "the graph is at revision " + std::to_string (slot.document.Revision ()) +
                       ", not the expected " + std::to_string (*expectedRevision);
        return result;
    }
    if (edits.empty ()) {
        result.code = batchcode::kEmptyBatch;
        result.error = "a transaction must carry at least one edit";
        return result;
    }
    if (edits.size () > kMaxBatchEdits) {
        result.code = batchcode::kBatchTooLarge;
        result.error = "a transaction may carry at most " + std::to_string (kMaxBatchEdits) + " edits";
        return result;
    }

    // ⚠️ A RELEASE CANNOT BE ROLLED BACK, SO IT CANNOT BE BATCHED. Its effect is
    // half in the document and half in the EVALUATOR's staged value, and only
    // the document half is snapshotted here. Refusing it outright is honest;
    // accepting it and rolling back only the document would leave the graph in a
    // state neither the client nor the runtime believes in. A release is a single
    // deliberate gesture anyway - it has no business inside a paste.
    for (size_t index = 0; index < edits.size (); ++index) {
        if (std::get_if<ReleaseHoldingEdit> (&edits[index].data) != nullptr) {
            result.code = batchcode::kUnbatchableEdit;
            result.error = "releaseHolding cannot take part in a transaction; apply it on its own";
            result.failedIndex = index;
            return result;
        }
    }

    // The rollback. See GraphDocument::RestoreContent for why this is a copy and
    // not a list of inverse actions.
    const GraphDocument before = slot.document;

    std::vector<NodeId> dirty;
    std::map<std::string, NodeId> aliases;
    for (size_t index = 0; index < edits.size (); ++index) {
        GraphEdit edit = edits[index];
        ResolveAliases (edit, aliases);

        if (auto* add = std::get_if<AddNodeEdit> (&edit.data); add != nullptr) {
            if (!add->alias.empty () && slot.document.FindNode (add->alias) != nullptr) {
                slot.document.RestoreContent (before);
                result.code = batchcode::kAliasConflict;
                result.error = "alias " + add->alias + " names a node this graph already has";
                result.failedIndex = index;
                result.revision = slot.document.Revision ();
                return result;
            }
            if (add->node.id.empty ()) {
                add->node.id = GenerateNodeIdLocked (slot, add->node.nodeType);
                result.assignedNodes.emplace_back (add->alias, add->node.id);
            }
            if (!add->alias.empty ())
                aliases.emplace (add->alias, add->node.id);
        }

        const EditResult step = ApplyEdit (slot.document, registry_, edit);
        if (!step.accepted) {
            slot.document.RestoreContent (before);
            result.code = step.code;
            result.error = step.error;
            result.failedIndex = index;
            result.revision = slot.document.Revision ();
            // The ids handed out by the refused half were never applied, and a
            // client that kept them would hold layout for nodes that do not exist.
            result.assignedNodes.clear ();
            return result;
        }
        dirty.insert (dirty.end (), step.dirtyNodes.begin (), step.dirtyNodes.end ());
    }

    std::sort (dirty.begin (), dirty.end ());
    dirty.erase (std::unique (dirty.begin (), dirty.end ()), dirty.end ());

    PushHistoryLocked (slot, before, label.empty () ? "Edit" : label, coalesceKey);

    // ⚠️ ONCE, NOT PER EDIT. Invalidating inside the loop would republish the
    // preview for a document that is halfway through a transaction the caller
    // may still roll back - a viewport flash of a state that never existed.
    slot.evaluator.Invalidate (slot.document, dirty);
    PublishPreviewLocked (graphId, slot);

    result.accepted = true;
    result.dirtyNodes = std::move (dirty);
    result.revision = slot.document.Revision ();
    return result;
}

EditResult GraphRuntimeState::Apply (const GraphId& graphId, const GraphEdit& edit, const std::string& label,
                                     const std::string& coalesceKey)
{
    Slot& slot = SlotFor (graphId);
    std::lock_guard lock (slot.documentMutex);

    // ⚠️ STAGE F5: A RELEASE IS REFUSED BEFORE THE DOCUMENT MOVES, NOT AFTER.
    // Its legality is split across two owners - the mode is in the document and
    // the staged value is in the evaluator - and this is the only place that
    // holds both. Asking afterwards would mean a revision bump and a dirtied
    // downstream closure for a release that then turned out to have nothing to
    // promote, which is a half-applied transaction by any other name.
    const auto* release = std::get_if<ReleaseHoldingEdit> (&edit.data);
    if (release != nullptr) {
        std::string error;
        std::string code;
        if (!slot.evaluator.CanRelease (slot.document, release->nodeId, error, code))
            return EditResult { false, code, error, {}, slot.document.Revision () };
    }

    // Taken before the edit so it can become the undo step. A copy of two
    // containers on every accepted edit is the cost of Ctrl+Z; the alternative
    // is an inverse action per edit kind, which is the thing that goes wrong.
    const GraphDocument before = slot.document;

    // An add that arrived unnamed is named HERE, under the document lock, which
    // is the only place that can promise the id is free.
    GraphEdit named = edit;
    NodeId assigned;
    if (auto* add = std::get_if<AddNodeEdit> (&named.data); add != nullptr && add->node.id.empty ()) {
        assigned = GenerateNodeIdLocked (slot, add->node.nodeType);
        add->node.id = assigned;
    }

    EditResult result = ApplyEdit (slot.document, registry_, named);
    result.assignedNodeId = result.accepted ? assigned : NodeId ();
    if (result.accepted) {
        if (release != nullptr) {
            // Promote first, THEN invalidate. Invalidate clears the downstream
            // statuses that the promotion is the reason for; doing it the other
            // way round would leave them describing the pre-release run.
            slot.evaluator.ReleaseHolding (slot.document, release->nodeId);
        }
        // ⚠️ A RELEASE IS NOT RECORDED, for the same reason it cannot be batched:
        // undoing it would restore the document's holding mode without putting
        // the promoted value back, so Ctrl+Z would appear to work and quietly
        // lose data. See ApplyBatch.
        if (release == nullptr)
            PushHistoryLocked (slot, before, label.empty () ? "Edit" : label, coalesceKey);
        slot.evaluator.Invalidate (slot.document, result.dirtyNodes);
        PublishPreviewLocked (graphId, slot);
    }
    return result;
}

RunId GraphRuntimeState::Cancel (const GraphId& graphId)
{
    Slot& slot = SlotFor (graphId);
    std::lock_guard lock (slot.runMutex);
    if (!slot.currentCancellation.has_value ())
        return kNoRun;
    slot.currentCancellation->Cancel ();
    return slot.currentRunId;
}

EvaluationSummary GraphRuntimeState::Evaluate (const GraphId& graphId, const EvaluationRequest& request)
{
    Slot& slot = SlotFor (graphId);

    // A newer run supersedes an older one: ask the one in flight to stop before
    // queueing behind it on the document lock.
    Cancel (graphId);

    std::lock_guard lock (slot.documentMutex);

    RunContext context;
    context.graphId = graphId;
    {
        std::lock_guard runLock (slot.runMutex);
        context.runId = slot.nextRunId++;
        context.cancellation = CancellationToken {};
        slot.currentRunId = context.runId;
        slot.currentCancellation = context.cancellation;
    }
    context.graphRevision = slot.document.Revision ();
    context.events = slot.recorder.SinkFor (graphId);
    // Read once per run rather than held: the host detaches on project close,
    // and a run that started with a project must see that it has gone rather
    // than keep a pointer to it.
    context.archicad = ActiveArchicadHost ();

    const EvaluationOutcome outcome =
        slot.evaluator.Evaluate (slot.document, registry_, ExecuteRuntimeNode, request, context);

    {
        std::lock_guard runLock (slot.runMutex);
        // Only retire the token if it is still ours; a newer run may already own it.
        if (slot.currentRunId == context.runId) {
            slot.currentCancellation.reset ();
            slot.currentRunId = kNoRun;
        }
        slot.lastRunId = context.runId;
    }

    PublishPreviewLocked (graphId, slot);

    EvaluationSummary summary;
    summary.graphId = graphId;
    summary.succeeded = outcome.succeeded;
    summary.cancelled = outcome.cancelled;
    summary.error = outcome.error;
    summary.failedNode = outcome.failedNode;
    summary.cyclicNodes = outcome.cyclicNodes;
    summary.runId = outcome.runId;
    summary.revision = outcome.graphRevision;
    summary.lastEventSeq = slot.recorder.Events ().LastSeq ();
    summary.plannedCount = outcome.plannedNodes.size ();
    summary.executedCount = outcome.executedCount;
    summary.cacheHitCount = outcome.cacheHitCount;
    summary.failedCount = outcome.failedCount;
    summary.blockedCount = outcome.blockedCount;
    summary.skippedEffectNodes = outcome.skippedEffectNodes;
    summary.effectsCommitted = outcome.effectsCommitted;
    summary.parallelism = outcome.parallelism;
    return summary;
}

ResultsSnapshot GraphRuntimeState::Results (const GraphId& graphId) const
{
    Slot& slot = SlotFor (graphId);
    std::lock_guard lock (slot.documentMutex);

    ResultsSnapshot snapshot;
    snapshot.graphId = graphId;
    snapshot.revision = slot.document.Revision ();
    // Read INSIDE the document lock and before the node walk, so the sequence a
    // client stores can only be older than the state it describes. A client that
    // re-asks from a slightly early sequence replays an event; one that asks from
    // a late sequence would silently miss one.
    snapshot.lastEventSeq = slot.recorder.Events ().LastSeq ();
    {
        std::lock_guard runLock (slot.runMutex);
        snapshot.lastRunId = slot.lastRunId;
    }
    for (const auto& [nodeId, node] : slot.document.Nodes ()) {
        (void) node;
        snapshot.nodes.push_back ({ nodeId, slot.evaluator.Status (nodeId), slot.evaluator.Result (nodeId) });
    }
    return snapshot;
}

RunEventLog::Tail GraphRuntimeState::Events (const GraphId& graphId, EventSeq sinceSeq, size_t maxEvents) const
{
    // Deliberately not under documentMutex: the event tail has to remain
    // readable while a run holds that lock, or a client could not watch a run in
    // progress - which is the entire point of the stream.
    return SlotFor (graphId).recorder.Events ().Since (sinceSeq, maxEvents);
}

GraphDependencyReport GraphRuntimeState::Dependencies (const GraphId& graphId) const
{
    Slot& slot = SlotFor (graphId);
    std::lock_guard lock (slot.documentMutex);
    const GraphResolution resolution = ResolveGraph (slot.document, registry_, ActiveArchicadHost ());
    return MakeDependencyReport (slot.document, registry_, resolution);
}

CompatibilityReport GraphRuntimeState::Compatibility (const GraphId& graphId, uint32_t formatVersion) const
{
    Slot& slot = SlotFor (graphId);
    std::lock_guard lock (slot.documentMutex);
    const GraphResolution resolution = ResolveGraph (slot.document, registry_, ActiveArchicadHost ());
    return MakeCompatibilityReport (slot.document, registry_, resolution, formatVersion);
}

std::vector<RunRecord> GraphRuntimeState::RecentRuns (const GraphId& graphId, size_t maxRuns) const
{
    return SlotFor (graphId).recorder.History ().Recent (maxRuns);
}

} // namespace evp::nodegraph
