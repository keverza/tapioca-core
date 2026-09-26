using System.Reflection;
using Eto.Forms;
using Grasshopper2.Components;
using Grasshopper2.Doc;
using Grasshopper2.Parameters.Special;
using Rhino;

namespace TapiocaGH2;

// Preset Picker reads named presets from the connected *input parameter*, not
// from the Available output. Refresh its cache after changing those presets;
// this pinned GH2 build exposes RefreshPresets only as an internal method.
internal static class ArchicadPresetBindings
{
    private static readonly object Sync = new();
    private static readonly List<WeakReference<Component>> Components = [];
    private static readonly MethodInfo? RefreshPicker = typeof(PresetPickerObject)
        .GetMethod("RefreshPresets", BindingFlags.Instance | BindingFlags.NonPublic);

    static ArchicadPresetBindings()
    {
        ArchicadProjectOptions.Updated += OnUpdated;
        Gh2ConnectionStatus.Updated += OnConnectionUpdated;
    }

    private static void OnConnectionUpdated() =>
        PostUpdate(ArchicadProjectOptions.Changed.None, ArchicadProjectOptions.ReadStatus(), connectionOnly: true);

    internal static void Register(Component component)
    {
        lock (Sync)
            Components.Add(new(component));
        // GH2 harvests a component with an optimised constructor that omits
        // Parameters entirely. That prototype must not set input presets.
        if (component.Parameters is not null && (component is ArchicadChoiceInput or ArchicadStoryInput))
            ApplyPresets(component);
    }

    private static void OnUpdated(ArchicadProjectOptions.Changed changes, ArchicadProjectOptions.Status status)
        => PostUpdate(changes, status, connectionOnly: false);

    private static void PostUpdate(ArchicadProjectOptions.Changed changes,
        ArchicadProjectOptions.Status status, bool connectionOnly)
    {
        Application? app = Application.Instance;
        if (app is null)
            return; // no canvas in the owned headless proof
        try { app.AsyncInvoke((Action)(() => ApplyUpdate(changes, status, connectionOnly))); }
        catch (ObjectDisposedException) { } // Rhino closed while a read was finishing
        catch (InvalidOperationException) { }
    }

    private static void ApplyUpdate(ArchicadProjectOptions.Changed changes,
        ArchicadProjectOptions.Status status, bool connectionOnly)
    {
        if (!connectionOnly && ArchicadProjectOptions.ReadStatus() != status)
            return; // a newer project check superseded this queued UI update
        Component[] alive;
        lock (Sync)
        {
            Components.RemoveAll(reference => !reference.TryGetTarget(out _));
            alive = Components.Select(reference => reference.TryGetTarget(out Component? target) ? target : null)
                .OfType<Component>().ToArray();
        }
        var documents = new HashSet<Document>();
        foreach (Component component in alive)
        {
            if (component.Document is not Document document)
                continue;
            if (connectionOnly && component is not ArchicadConnectionStatus)
                continue;
            if (!ShouldUpdate(component, changes))
                continue;
            // A routine check never expires selectors. Only changed lists are
            // republished; a project switch/failure explicitly clears all.
            try
            {
                if (component is not ArchicadConnectionStatus)
                    ApplyPresets(component);
                component.Expire();
                documents.Add(document);
            }
            catch (Exception error)
            {
                RhinoApp.WriteLine("Tapioca GH2: could not update project presets: " + error.Message);
            }
        }
        foreach (Document document in documents)
        {
            try { document.Solution.Start(); }
            catch (Exception error) { RhinoApp.WriteLine("Tapioca GH2: refresh solve failed: " + error.Message); }
        }
    }

    private static bool ShouldUpdate(Component component, ArchicadProjectOptions.Changed changes) => component switch
    {
        ArchicadLayerInput => changes.HasFlag(ArchicadProjectOptions.Changed.Layers),
        ArchicadLineTypeInput => changes.HasFlag(ArchicadProjectOptions.Changed.LineTypes),
        ArchicadStoryInput => changes.HasFlag(ArchicadProjectOptions.Changed.Stories),
        ArchicadConnectionStatus => true,
        _ => false
    };

    private static void ApplyPresets(Component component)
    {
        var choice = (Grasshopper2.Parameters.Standard.IntegerParameter)component.Parameters.Input(0);
        choice.Presets.Clear();
        ArchicadProjectOptions.Snapshot snapshot = ArchicadProjectOptions.Read();
        string[] names = component switch
        {
            ArchicadLayerInput => snapshot.Layers,
            ArchicadLineTypeInput => snapshot.LineTypes,
            ArchicadStoryInput => snapshot.Stories.Select(story => $"{story.Index}: {story.Name}").ToArray(),
            _ => []
        };
        for (int index = 0; index < names.Length; index++)
            choice.Presets.Add(names[index], "Archicad project choice", index);

        Document? document = component.Document;
        if (document is null)
            return;
        foreach (Guid source in choice.Inputs.ToArray())
        {
            if (document.Objects.FindParameter(source) is not PresetPickerObject picker)
                continue;
            if (RefreshPicker is null)
            {
                RhinoApp.WriteLine("Tapioca GH2: this GH2 build has no Preset Picker refresh hook.");
                continue;
            }
            RefreshPicker.Invoke(picker, null);
            picker.Expire();
        }
    }
}
