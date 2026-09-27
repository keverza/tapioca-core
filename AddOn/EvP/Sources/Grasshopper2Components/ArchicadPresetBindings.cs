using System.Reflection;
using Eto.Forms;
using Grasshopper2.Components;
using Grasshopper2.Doc;
using Grasshopper2.Parameters.Special;
using Grasshopper2.Parameters.Standard;
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
        if (component.Parameters is not null &&
            component is ArchicadChoiceInput or ArchicadStoryInput or ArchicadBakeTerrainMesh)
            ApplyPresets(component);
        if (component.Parameters is not null && component is IArchicadQueryPresets query)
        {
            query.RestoreModePresets();
            query.ClearSelectorPresets(); // saved keys must not silently cross projects
        }
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
                if (component is ArchicadChoiceInput or ArchicadStoryInput or ArchicadBakeTerrainMesh)
                    ApplyPresets(component);
                // A successful Refresh may return identical layer/story lists
                // while element property/GDL definitions have changed. Do not
                // keep the previous discovery catalog across that refresh.
                if (component is IArchicadQueryPresets query && !status.Checking)
                    query.ClearSelectorPresets();
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
        ArchicadBakeTerrainMesh => changes.HasFlag(ArchicadProjectOptions.Changed.Layers) ||
            changes.HasFlag(ArchicadProjectOptions.Changed.Stories),
        ArchicadConnectionStatus => true,
        ArchicadElement or ArchicadElementHeaders => true,
        ArchicadGetContours or ArchicadGetRelationships or ArchicadGetProperties or
            ArchicadGetGeometry or ArchicadGdlParameters => true,
        _ => false
    };

    private static void ApplyPresets(Component component)
    {
        int inputIndex = component is ArchicadBakeTerrainMesh ? 2 : 0;
        ArchicadProjectOptions.Snapshot snapshot = ArchicadProjectOptions.Read();
        string[] names = component switch
        {
            ArchicadLayerInput or ArchicadBakeTerrainMesh => snapshot.Layers,
            ArchicadLineTypeInput => snapshot.LineTypes,
            ArchicadStoryInput => snapshot.Stories.Select(story => $"{story.Index}: {story.Name}").ToArray(),
            _ => []
        };
        if (component is ArchicadBakeTerrainMesh)
        {
            var layer = (TextParameter)component.Parameters.Input(inputIndex);
            layer.Presets.Clear();
            layer.Presets.Add("Mesh tool default", "Use Archicad's active Mesh tool layer.", "");
            foreach (string name in names)
                layer.Presets.Add(name, "Archicad project layer", name);
        }
        else
        {
            var choice = (IntegerParameter)component.Parameters.Input(inputIndex);
            choice.Presets.Clear();
            for (int index = 0; index < names.Length; index++)
                choice.Presets.Add(names[index], "Archicad project choice", index);
        }

        RefreshConnectedPickers(component, inputIndex);
    }

    internal static void RefreshConnectedPickers(Component component, int inputIndex)
    {
        Document? document = component.Document;
        if (document is null)
            return;
        var choice = component.Parameters.Input(inputIndex);
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
