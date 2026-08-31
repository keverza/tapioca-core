using System;
using System.Windows;
using System.Windows.Controls;
using Dynamo.Controls;
using Dynamo.Wpf;
using Tapioca.Nodes;

namespace Tapioca.Nodes.UI;

/// <summary>
/// The five buttons on the canvas: Update, Add, Remove, Reselect, Clear — the same
/// vocabulary, in the same order, as the Tapioca palette's selection rows
/// (Palette/SelectionSetPanel.hpp) and the node-graph editor's Get Selection node
/// (GraphUI/src/SchemaNode.svelte). A user who has captured a selection for a command
/// should not have to learn a second set of words to capture one for a graph.
///
/// ⚠️ THIS ASSEMBLY IS WPF AND IS EXPECTED TO FAIL TO LOAD HEADLESSLY. The runner is
/// net10.0 with no Windows Desktop framework, so PresentationFramework is not there.
/// Dynamo's PackageLoader catches LibraryLoadFailedException per assembly and carries
/// on with the rest of the package (PackageLoader.cs), so the runner still gets
/// Tapioca.Dynamo's ZeroTouch nodes and Tapioca.DynamoNodes' model — it simply has no
/// buttons, which is correct, because it has no canvas to press them on.
///
/// ⚠️ THE ACTIONS ARE THE MODEL'S, NOT THE VIEW'S. Every button forwards to a method
/// on SelectionSetNode and displays the sentence it returns. The view never talks to
/// the bridge and never decides what an action means, so the headless model and the
/// canvas can never drift apart on what "Add" does.
/// </summary>
public class SelectionSetNodeView : INodeViewCustomization<SelectionSetNode>
{
    private SelectionSetNode? node;
    private TextBlock? summary;
    private TextBlock? lastResult;

    public void CustomizeView(SelectionSetNode model, NodeView nodeView)
    {
        node = model;

        var panel = new StackPanel { Margin = new Thickness(6, 4, 6, 6) };

        summary = new TextBlock
        {
            Text = model.SummaryText,
            FontWeight = FontWeights.Bold,
            Margin = new Thickness(0, 0, 0, 4)
        };
        panel.Children.Add(summary);

        // WrapPanel, not a fixed grid: five buttons on one row would force the node
        // wider than anything else on the canvas, and the node's width is the user's
        // to set.
        var buttons = new WrapPanel();
        AddButton(buttons, "Update", "Replace the set with Archicad's current selection", n => n.Update());
        AddButton(buttons, "Add", "Add the current selection to the set", n => n.Add());
        AddButton(buttons, "Remove", "Take the current selection out of the set", n => n.Remove());
        AddButton(buttons, "Reselect", "Select this set's elements in Archicad", n => n.Reselect());
        AddButton(buttons, "Clear", "Empty the set", n => n.Clear());
        panel.Children.Add(buttons);

        // What the last press did. Named text rather than a colour or an icon: the
        // same accessibility rule the node-graph editor states — no status carried by
        // colour alone.
        lastResult = new TextBlock
        {
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(0, 4, 0, 0),
            Opacity = 0.8
        };
        panel.Children.Add(lastResult);

        // ContentGrid, not the deprecated inputGrid: this node has no input ports, so
        // the body is content rather than something sitting beside a port column.
        nodeView.ContentGrid.Children.Add(panel);
    }

    private void AddButton(Panel parent, string label, string tooltip, Func<SelectionSetNode, string> action)
    {
        var button = new Button
        {
            Content = label,
            ToolTip = tooltip,
            MinWidth = 62,
            Margin = new Thickness(0, 0, 4, 4),
            Padding = new Thickness(6, 2, 6, 2)
        };

        button.Click += (_, _) =>
        {
            if (node is null)
            {
                return;
            }

            // The model already turns a dead bridge into a sentence, so the only thing
            // left to guard is a genuinely unexpected failure — which must not take the
            // canvas down with it.
            string message;
            try
            {
                message = action(node);
            }
            catch (Exception exception)
            {
                message = exception.Message;
            }

            if (summary is not null)
            {
                summary.Text = node.SummaryText;
            }
            if (lastResult is not null)
            {
                lastResult.Text = message;
            }
        };

        parent.Children.Add(button);
    }

    public void Dispose()
    {
        node = null;
        summary = null;
        lastResult = null;
    }
}
