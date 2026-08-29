#include "NodeGraph/GraphReports.hpp"

#include "NodeGraph/ArchicadHost.hpp"

#include <algorithm>
#include <set>

namespace evp::nodegraph {
namespace {

// Which parameters of a node name an Archicad resource. Driven by the declared
// parameter type rather than by a name convention, so a new node that takes an
// element reference is covered without touching this file.
ReferenceKind KindOf (ValueType valueType, bool& isReference)
{
    isReference = valueType == ValueType::ArchicadElementRef;
    return ReferenceKind::Element;
}

void CollectReferences (const Node& node, const NodeType& nodeType, std::vector<ReferenceSite>& sites)
{
    for (const ParameterSchema& parameter : nodeType.parameters) {
        bool isReference = false;
        const ReferenceKind kind = KindOf (parameter.valueType, isReference);
        if (!isReference)
            continue;

        const auto found = node.parameters.find (parameter.id);
        const Value* value = found != node.parameters.end ()       ? &found->second
                             : parameter.defaultValue.has_value () ? &*parameter.defaultValue
                                                                   : nullptr;
        if (value == nullptr || value->Type () != ValueType::ArchicadElementRef)
            continue;

        ReferenceSite site;
        site.nodeId = node.id;
        site.parameterId = parameter.id;
        site.reference = Reference { kind, std::get<ArchicadElementRef> (value->DataValue ()).guid, {} };
        sites.push_back (std::move (site));
    }
}

} // namespace

const char* FindingSeverityName (FindingSeverity severity)
{
    return severity == FindingSeverity::Error ? "error" : "warning";
}

const char* CompatibilityStatusName (CompatibilityStatus status)
{
    switch (status) {
        case CompatibilityStatus::Compatible:
            return "compatible";
        case CompatibilityStatus::NeedsMigration:
            return "needsMigration";
        case CompatibilityStatus::UnsupportedFormat:
            return "unsupportedFormat";
        case CompatibilityStatus::MissingNodeType:
            return "missingNodeType";
        case CompatibilityStatus::MissingCapability:
            return "missingCapability";
    }
    return "compatible";
}

bool GraphResolution::Usable () const
{
    return std::none_of (findings.begin (), findings.end (),
                         [] (const GraphFinding& finding) { return finding.severity == FindingSeverity::Error; });
}

GraphResolution ResolveGraph (const GraphDocument& document, const NodeRegistry& registry, IArchicadHost* host)
{
    GraphResolution resolution;

    const bool hostAvailable = host != nullptr && host->IsAvailable ();
    const UnavailableReferenceResolver offline;
    const IReferenceResolver& resolver =
        hostAvailable ? host->References () : static_cast<const IReferenceResolver&> (offline);

    for (const auto& [nodeId, node] : document.Nodes ()) {
        const NodeType* nodeType = registry.Find (node.nodeType);
        if (nodeType == nullptr) {
            resolution.findings.push_back ({ FindingSeverity::Error, nodeId, "missingNodeType",
                                             "this runtime has no node type called '" + node.nodeType + "'" });
            continue;
        }

        if (nodeType->executionDomain == ExecutionDomain::ArchicadMainThread && !hostAvailable) {
            resolution.findings.push_back ({ FindingSeverity::Error, nodeId, "hostUnavailable",
                                             std::string (nodeType->label) + " needs an open Archicad project" });
        }

        for (const GenerationDomain domain : nodeType->generations.Domains ()) {
            uint64_t value = 0;
            std::string error;
            if (!hostAvailable || !host->Generations ().Sample (domain, value, error)) {
                resolution.findings.push_back ({ FindingSeverity::Error, nodeId, "unsampleableGeneration",
                                                 std::string ("the '") + GenerationDomainName (domain) +
                                                     "' state this node reads is unavailable" +
                                                     (error.empty () ? "" : ": " + error) });
            }
        }

        if (nodeType->effect == EffectKind::HostUiWrite) {
            resolution.findings.push_back (
                { FindingSeverity::Warning, nodeId, "effectRequiresExplicitRun",
                  std::string (nodeType->label) + " changes Archicad and runs only on an explicit run" });
        }

        CollectReferences (node, *nodeType, resolution.sites);
    }

    // One batched resolve for the whole document, after the walk: the same
    // reason ArchicadNodes batches, and it also means the report describes one
    // consistent moment rather than a project drifting under a slow loop.
    {
        std::vector<Reference> references;
        references.reserve (resolution.sites.size ());
        for (const ReferenceSite& site : resolution.sites)
            references.push_back (site.reference);
        const std::vector<ReferenceResolution> resolved = resolver.ResolveAll (references);
        for (size_t i = 0; i < resolution.sites.size () && i < resolved.size (); ++i)
            resolution.sites[i].resolution = resolved[i];
    }

    for (const ReferenceSite& site : resolution.sites) {
        if (site.resolution.Usable ())
            continue;
        resolution.findings.push_back (
            { FindingSeverity::Error, site.nodeId, "unresolvedReference",
              std::string (ResolutionStatusName (site.resolution.status)) + ": " + site.resolution.detail });
    }

    return resolution;
}

GraphDependencyReport MakeDependencyReport (const GraphDocument& document, const NodeRegistry& registry,
                                            const GraphResolution& resolution)
{
    GraphDependencyReport report;
    report.findings = resolution.findings;
    report.canEvaluate = resolution.Usable ();

    for (const ReferenceSite& site : resolution.sites) {
        if (site.resolution.Usable ())
            ++report.resolvedReferences;
        else
            ++report.unresolvedReferences;
    }

    for (const auto& [nodeId, node] : document.Nodes ()) {
        (void) nodeId;
        const NodeType* nodeType = registry.Find (node.nodeType);
        if (nodeType == nullptr)
            continue;
        if (nodeType->executionDomain == ExecutionDomain::ArchicadMainThread)
            ++report.nodesNeedingArchicad;
        if (nodeType->effect == EffectKind::HostUiWrite)
            ++report.effectNodes;
    }

    return report;
}

CompatibilityReport MakeCompatibilityReport (const GraphDocument& document, const NodeRegistry& registry,
                                             const GraphResolution& resolution, uint32_t formatVersion)
{
    CompatibilityReport report;

    // Format first: a document this runtime cannot parse makes every other
    // finding meaningless, so it short-circuits rather than being one finding
    // among many.
    if (formatVersion > kGraphFormatVersion) {
        report.status = CompatibilityStatus::UnsupportedFormat;
        report.findings.push_back ({ FindingSeverity::Error,
                                     {},
                                     "unsupportedFormat",
                                     "this graph was written by a newer version of Tapioca" });
        return report;
    }
    if (formatVersion != 0 && formatVersion < kGraphFormatVersion) {
        report.status = CompatibilityStatus::NeedsMigration;
        report.findings.push_back ({ FindingSeverity::Warning,
                                     {},
                                     "needsMigration",
                                     "this graph was written by an older version and will be migrated" });
    }

    // Loading cares about what the document NAMES, not about whether Archicad
    // happens to be reachable right now - a graph is perfectly loadable with no
    // project open, and saying otherwise would refuse to open a file for editing
    // because it cannot run. That is the substantive difference between the two
    // projections over the same pass.
    std::set<std::string> missing;
    for (const GraphFinding& finding : resolution.findings) {
        if (finding.kind == "missingNodeType") {
            const Node* node = document.FindNode (finding.nodeId);
            if (node != nullptr && missing.insert (node->nodeType).second)
                report.missingNodeTypes.push_back (node->nodeType);
            report.findings.push_back (finding);
        }
    }
    if (!report.missingNodeTypes.empty ()) {
        report.status = CompatibilityStatus::MissingNodeType;
        return report;
    }

    // A capability the runtime does not have at all - as opposed to one that is
    // merely not connected - is a load-time problem. Today every declared domain
    // is one this build knows, so this only fires for a graph from a future
    // build whose format version did not change.
    for (const auto& [nodeId, node] : document.Nodes ()) {
        (void) nodeId;
        if (registry.Find (node.nodeType) == nullptr) {
            report.status = CompatibilityStatus::MissingCapability;
            return report;
        }
    }

    return report;
}

} // namespace evp::nodegraph
