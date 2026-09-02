#include "NodeGraph/GraphRuntimeState.hpp"

#include "NodeGraph/ArchicadHost.hpp"
#include "NodeGraph/ArchicadNodes.hpp"
#include "NodeGraph/ElementClassification.hpp"
#include "NodeGraph/GraphAlgorithms.hpp"
#include "NodeGraph/NodeExecution.hpp"
#include "NodeGraph/PreviewProjection.hpp"
#include "Preview/GraphPreviewStore.hpp"

#include <algorithm>
#include <set>
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

EditResult GraphRuntimeState::Apply (const GraphId& graphId, const GraphEdit& edit)
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

    EditResult result = ApplyEdit (slot.document, registry_, edit);
    if (result.accepted) {
        if (release != nullptr) {
            // Promote first, THEN invalidate. Invalidate clears the downstream
            // statuses that the promotion is the reason for; doing it the other
            // way round would leave them describing the pre-release run.
            slot.evaluator.ReleaseHolding (slot.document, release->nodeId);
        }
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
