using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using Dynamo.Graph;
using Dynamo.Graph.Nodes;
using Newtonsoft.Json;
using ProtoCore.AST.AssociativeAST;

namespace Tapioca.Nodes;

/// <summary>
/// A set of Archicad elements the user CAPTURES, operated on with the same five
/// actions as the Tapioca palette and the node-graph editor: Update, Add, Remove,
/// Reselect, Clear (Palette/SelectionSetPanel.hpp, NodeGraph/ArchicadNodes.cpp).
///
/// ⚠️ WHY THIS IS NOT <c>Tapioca.Selection.Current()</c>. That ZeroTouch node reads
/// the LIVE selection on every evaluation, so the graph's answer changed every time
/// the user clicked in the model and every downstream node went dirty with it. A
/// captured set changes only when a button is pressed. ArchicadNodes.cpp records that
/// same reasoning for the node-graph editor's Get Selection, and the two must not
/// disagree — a user who learned one should not have to learn the other.
///
/// ⚠️ WHY THE MODEL IS IN ITS OWN ASSEMBLY, with no WPF and no reference to
/// DynamoCoreWpf. Two Dynamo rules meet here:
///
///   1. An assembly containing a NodeModel is NOT imported as ZeroTouch
///      (DynamoModel.LoadNodeLibrary). Putting this class in Tapioca.Dynamo.dll would
///      silently delete every existing Tapioca ZeroTouch node from the library.
///   2. The headless runner is net10.0 with no Windows Desktop framework, exactly as
///      Dynamo's own DynamoCLI is. An assembly referencing WPF cannot load there.
///
/// So the buttons live in Tapioca.DynamoNodesUI.dll and this file holds only state.
/// The runner evaluates a captured set perfectly well and simply has no buttons —
/// which is right, because it has no canvas to press them on.
///
/// The set persists with the graph because <see cref="Elements"/> is an ordinary
/// serialized property, the same mechanism CoreNodeModels.Remember uses for its cache;
/// a reopened graph holds what it held. GUIDs are stored as REFERENCES and re-resolved
/// by whoever consumes them, never cached as geometry.
/// </summary>
[NodeName("Archicad Selection")]
[NodeCategory("Tapioca.Archicad")]
[NodeDescription(
    "A set of Archicad elements you capture. Update replaces it with the current selection, " +
    "Add and Remove change it, Reselect selects it in Archicad, Clear empties it.")]
[OutPortNames("elements", "count")]
[OutPortTypes("string[]", "int")]
[OutPortDescriptions("The captured element GUIDs", "How many elements the set holds")]
[IsDesignScriptCompatible]
public class SelectionSetNode : NodeModel
{
    private List<string> elements = new();

    /// <summary>
    /// The captured element GUIDs. Serialized with the graph, so this is also the
    /// property that makes the set survive save and reopen.
    ///
    /// Assigning normalizes and marks the node modified, which is what makes pressing
    /// a button enough on its own: consumers re-evaluate without the user ALSO having
    /// to press Run, exactly as NodeGraphSelectionCommands.cpp requires of the palette.
    /// </summary>
    [JsonProperty("tapiocaElements")]
    public List<string> Elements
    {
        get => elements;
        set
        {
            elements = Normalize(value);
            MarkNodeAsModified(forceExecute: true);
            RaisePropertyChanged(nameof(Elements));
            RaisePropertyChanged(nameof(Count));
            RaisePropertyChanged(nameof(SummaryText));
        }
    }

    [JsonIgnore]
    public int Count => elements.Count;

    /// <summary>What the node body reads when nothing has customized it.</summary>
    [JsonIgnore]
    public string SummaryText => string.Format(
        CultureInfo.InvariantCulture, "{0} element{1} captured", elements.Count, elements.Count == 1 ? "" : "s");

    /// <summary>Parameterless constructor — Dynamo needs one to discover the type.</summary>
    public SelectionSetNode()
    {
        RegisterAllPorts();
        ArgumentLacing = LacingStrategy.Disabled;
    }

    /// <summary>Used when the node is read back out of a .dyn.</summary>
    [JsonConstructor]
    private SelectionSetNode(IEnumerable<PortModel> inPorts, IEnumerable<PortModel> outPorts)
        : base(inPorts, outPorts)
    {
        ArgumentLacing = LacingStrategy.Disabled;
    }

    /// <summary>
    /// The node's value IS its stored set, so building the AST calls no host. That is
    /// what lets the set evaluate offline and stay clean while the user works in
    /// Archicad — the property ArchicadNodes.cpp relies on for the same node.
    /// </summary>
    public override IEnumerable<AssociativeNode> BuildOutputAst(List<AssociativeNode> inputAstNodes)
    {
        var items = elements
            .Select(guid => (AssociativeNode)AstFactory.BuildStringNode(guid))
            .ToList();

        return new[]
        {
            AstFactory.BuildAssignment(GetAstIdentifierForOutputIndex(0), AstFactory.BuildExprList(items)),
            // (long), not (int): DesignScript has been 64-bit since Dynamo 2.0 and the
            // int overload is deprecated for removal.
            AstFactory.BuildAssignment(GetAstIdentifierForOutputIndex(1),
                AstFactory.BuildIntNode((long)elements.Count))
        };
    }

    // ---- the five actions -------------------------------------------------
    //
    // The palette's vocabulary, in the palette's order (SelectionSetPanel.hpp). Each
    // returns the sentence the view shows, because the view must not have to know what
    // any of them means — and each is a BUTTON PRESS, never an evaluation, which is
    // what makes Reselect's write to the user's selection legitimate at all.
    //
    // Failures come back as text rather than exceptions: a bridge that is not
    // listening (Archicad closed, the add-on not loaded) is an ordinary state for a
    // canvas that outlives it, not a graph error.

    /// <summary>Replaces the set with Archicad's current selection.</summary>
    public string Update() => Capture(current => current);

    /// <summary>Adds Archicad's current selection to the set.</summary>
    public string Add() => Capture(current => elements.Concat(current));

    /// <summary>Takes Archicad's current selection out of the set.</summary>
    public string Remove() => Capture(current =>
    {
        var dropped = new HashSet<string>(current, System.StringComparer.OrdinalIgnoreCase);
        return elements.Where(guid => !dropped.Contains(guid));
    });

    /// <summary>Selects this set's elements in Archicad.</summary>
    public string Reselect()
    {
        if (elements.Count == 0)
        {
            return "Nothing captured to reselect.";
        }

        try
        {
            int selected = Selection.Select(elements);
            // Reported rather than silently tolerated: an element deleted since the
            // capture cannot be selected, and a set that quietly shrank on the user is
            // the drift GraphRuntimeState.hpp refuses for the same action.
            return selected == elements.Count
                ? $"Reselected {selected} element{(selected == 1 ? "" : "s")}."
                : $"Reselected {selected} of {elements.Count}; the rest no longer exist.";
        }
        catch (System.Exception exception)
        {
            return $"Reselect failed: {exception.Message}";
        }
    }

    /// <summary>Empties the set.</summary>
    public string Clear()
    {
        Elements = new List<string>();
        return "Set cleared.";
    }

    /// <summary>
    /// One read of Archicad's selection, combined into the set however the caller says.
    /// </summary>
    private string Capture(System.Func<IReadOnlyList<string>, IEnumerable<string>> combine)
    {
        try
        {
            var current = Selection.Current();
            Elements = combine(current).ToList();
            return SummaryText + ".";
        }
        catch (System.Exception exception)
        {
            return $"Could not read the Archicad selection: {exception.Message}";
        }
    }

    /// <summary>
    /// Duplicates dropped, order preserved: the same element captured twice is one
    /// element, and a stable order keeps a graph's output from reshuffling for no
    /// reason the user can see.
    /// </summary>
    private static List<string> Normalize(IEnumerable<string>? guids)
    {
        if (guids is null)
        {
            return new List<string>();
        }

        return guids
            .Where(guid => !string.IsNullOrWhiteSpace(guid))
            .Select(guid => guid.Trim())
            .Distinct(System.StringComparer.OrdinalIgnoreCase)
            .ToList();
    }
}
