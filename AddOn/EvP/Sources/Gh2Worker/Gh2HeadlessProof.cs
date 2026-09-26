using Grasshopper2.Components;
using Grasshopper2.Doc;
using Grasshopper2.Framework;
using Grasshopper2.Parameters;
using Grasshopper2.Parameters.Special;
using System.Reflection;

namespace Tapioca.Gh2Worker;

// A real RhinoCore + GH2 document round-trip, kept separate from the backend
// that a future named-pipe session will call.
internal static class Gh2HeadlessProof
{
    internal static int Run(string pluginPath, string fixturePath)
    {
        using var backend = new Gh2Backend(pluginPath);
        if (!File.Exists(fixturePath))
        {
            backend.CreateDefinition();
            Guid originalExposure = backend.ExposureId;
            backend.RemapDocumentIds();
            if (backend.ExposureId == originalExposure)
                throw new InvalidOperationException("GH2 ID remap did not mint a new ExposureId.");
            Check(backend, 3.5, 3.5);
            backend.SaveCopy(fixturePath);
        }

        backend.OpenDefinition(fixturePath);
        Guid persistedExposure = backend.ExposureId;
        Check(backend, 3.5, 3.5);
        Check(backend, 7.25, 7.25);
        Check(backend, null, 1.0);
        backend.SaveCopy(fixturePath);
        backend.CloseDefinition();

        backend.OpenDefinition(fixturePath);
        if (backend.ExposureId != persistedExposure)
            throw new InvalidOperationException("Saving changed the Player exposure ID.");
        Check(backend, null, 1.0);
        VerifyStatusComponent(fixturePath);
        Console.WriteLine("GH2 headless transient-input/save/reopen gate passed.");
        return 0;
    }

    private static void Check(Gh2Backend backend, double? value, double expected)
    {
        double actual = backend.Solve(value);
        if (actual != expected)
            throw new InvalidOperationException($"Expected {expected}, got {actual}.");
        Console.WriteLine($"Solution completed: {actual}");
    }

    private static void VerifyStatusComponent(string fixturePath)
    {
        var statusId = new Guid("c512fd81-a693-47c4-95bf-30e3fa60b411");
        Document document = Document.NewInertDocument();
        try
        {
            IDocumentObject item = ObjectProxies.TryEmit<IDocumentObject>(statusId)
                ?? throw new InvalidOperationException("GH2 did not register Archicad Connection Status.");
            if (!document.Objects.Add(item))
                throw new InvalidOperationException("The GH2 status component was not added.");
            Guid[] selectors =
            [
                new("0c611705-95dc-4391-bc24-89ed73fd942f"),
                new("3cd387a2-d10a-4925-a8a4-3ec5ab5b507a"),
                new("ea30714c-a8c9-4db6-85db-70fc83835fe5")
            ];
            var choiceComponents = new List<Component>();
            foreach (Guid selector in selectors)
            {
                IDocumentObject choice = ObjectProxies.TryEmit<IDocumentObject>(selector)
                    ?? throw new InvalidOperationException($"GH2 did not register project selector {selector}.");
                if (!document.Objects.Add(choice))
                    throw new InvalidOperationException("A GH2 project selector could not be added.");
                choiceComponents.Add((Component)choice);
            }
            var selectionId = new Guid("b31976a0-3e53-47f6-8c4e-696696c48972");
            IDocumentObject selection = ObjectProxies.TryEmit<IDocumentObject>(selectionId)
                ?? throw new InvalidOperationException("GH2 did not register Archicad Selection.");
            if (!document.Objects.Add(selection))
                throw new InvalidOperationException("The GH2 selection component was not added.");
            document.State = DocumentState.Active;
            Component component = (Component)item;
            ((Grasshopper2.Parameters.Standard.TextParameter)component.Parameters.Input(1))
                .Set("Project description for Tapioca.");
            component.Expire();
            foreach (Component choice in choiceComponents)
                choice.Expire();
            ((Component)selection).Expire();
            var solution = Task.Run(() => document.Solution.StartWait(null, SolutionMode.Headless))
                .GetAwaiter().GetResult();
            if (solution.Phase != SolutionPhase.Completed)
                throw new InvalidOperationException($"GH2 status solution ended in {solution.Phase}.");
            string[] diagnostics = component.Parameters.Output(0).State.Data.Tree()?.AllItems
                .Select(item => item.ToString() ?? "").ToArray() ?? [];
            if (component.State.Data.Messages.WarningCount == 0 ||
                component.Parameters.Outputs.Count() != 1 || diagnostics.Length != 10 ||
                diagnostics[0] != "Status: Disconnected" || diagnostics[2] != "Endpoint: (none)" ||
                !diagnostics[4].Contains("127.0.0.1") ||
                diagnostics[9] != "Description: Project description for Tapioca.")
                throw new InvalidOperationException("GH2 status did not show a disconnected local-only bridge.");
            foreach (Component choice in choiceComponents)
            {
                int availableOutput = choice.Parameters.Outputs.Count() - 1;
                int count = choice.Parameters.Output(availableOutput).State.Data.Tree()?.ItemCount ?? 0;
                if (count != 0)
                    throw new InvalidOperationException("A disconnected selector exposed stale Archicad choices.");
            }
            VerifyPopulatedSelectors(document, choiceComponents);
            Guid selectedId = Guid.NewGuid();
            MethodInfo parseSelection = selection.GetType().GetMethod("ParseSelection",
                BindingFlags.NonPublic | BindingFlags.Static)!;
            Guid[] parsed = (Guid[])parseSelection.Invoke(null, ["{\"ok\":true,\"data\":{\"elements\":[{" +
                "\"elementId\":{\"guid\":\"" + selectedId + "\"}}]}}"] )!;
            if (!parsed.SequenceEqual([selectedId]))
                throw new InvalidOperationException("GH2 lost a selected Archicad GUID.");
            try
            {
                parseSelection.Invoke(null, ["{\"ok\":false,\"error\":\"The GH2 request is unavailable.\"}"]);
                throw new InvalidOperationException("GH2 accepted a refused selection request.");
            }
            catch (TargetInvocationException error) when (error.InnerException is InvalidDataException native &&
                native.Message.Contains("The GH2 request is unavailable", StringComparison.Ordinal)) { }
            try
            {
                parseSelection.Invoke(null, ["{\"ok\":false,\"error\":{\"code\":\"SchemaValidationFailed\"," +
                    "\"message\":\"Invalid selected GUID\"}}"]);
                throw new InvalidOperationException("GH2 accepted a native selection error.");
            }
            catch (TargetInvocationException error) when (error.InnerException is InvalidDataException native &&
                native.Message.Contains("SchemaValidationFailed", StringComparison.Ordinal) &&
                native.Message.Contains("Invalid selected GUID", StringComparison.Ordinal)) { }
            selection.GetType().GetField("elements", BindingFlags.Instance | BindingFlags.NonPublic)!
                .SetValue(selection, parsed);
            ((Component)selection).Expire();
            var selectedSolution = Task.Run(() => document.Solution.StartWait(null, SolutionMode.Headless))
                .GetAwaiter().GetResult();
            if (selectedSolution.Phase != SolutionPhase.Completed ||
                ((Component)selection).Parameters.Output(0).State.Data.Tree()?.AllItems.Single()?.ToString() != selectedId.ToString("D"))
                throw new InvalidOperationException("GH2 selection did not emit its saved element GUID.");
            Guid[] before = choiceComponents.Select(ExposureId).ToArray();
            document.Objects.ChangeAllIds();
            Guid[] reminted = choiceComponents.Select(ExposureId).ToArray();
            if (reminted.Zip(before).Any(pair => pair.First == pair.Second) || reminted.Distinct().Count() != 3)
                throw new InvalidOperationException("Duplicating GH2 selectors did not mint unique ExposureIds.");

            string copy = Path.Combine(Path.GetDirectoryName(fixturePath)!,
                "gh2-selector-proof-" + Guid.NewGuid().ToString("N") + ".ghz");
            try
            {
                if (!new DocumentIO(document, false, false, false).SaveCopy(copy))
                    throw new InvalidOperationException("GH2 could not save a selector definition.");
                var io = new DocumentIO(trackFiles: false, reportErrors: false, resolvePlugins: false);
                if (!io.Open(copy) || io.Document is null)
                    throw new InvalidOperationException("GH2 could not reopen a selector definition.");
                try
                {
                    Guid[] restored = io.Document.Objects.ActiveObjects.OfType<Component>()
                        .Where(component => selectors.Contains(component.GetType().GetCustomAttributes(false)
                            .OfType<GrasshopperIO.IoIdAttribute>().FirstOrDefault()?.Id ?? Guid.Empty))
                        .Select(ExposureId).ToArray();
                    if (restored.Length != 3 || !restored.Order().SequenceEqual(reminted.Order()))
                        throw new InvalidOperationException("GH2 changed selector ExposureIds on save/reopen.");
                    Component? savedSelection = io.Document.Objects.ActiveObjects.OfType<Component>()
                        .FirstOrDefault(component => component.GetType().GetCustomAttributes(false)
                            .OfType<GrasshopperIO.IoIdAttribute>().FirstOrDefault()?.Id == selectionId);
                    Guid[]? restoredIds = (Guid[]?)savedSelection?.GetType()
                        .GetField("elements", BindingFlags.Instance | BindingFlags.NonPublic)?.GetValue(savedSelection);
                    if (restoredIds is null || !restoredIds.SequenceEqual([selectedId]))
                        throw new InvalidOperationException("GH2 did not preserve selected GUIDs on save/reopen.");
                }
                finally { io.Document.Close(); }
            }
            finally { if (File.Exists(copy)) File.Delete(copy); }
            Console.WriteLine("GH2 connection status component: disconnected, local addresses available.");
        }
        finally
        {
            document.Close();
        }
    }

    private static void VerifyPopulatedSelectors(Document document, List<Component> choices)
    {
        // The worker's linked wire-check type is NOT the .rhp's process-local
        // store. Inject only into the plugin for this headless solve fixture.
        Type options = choices[0].GetType().Assembly.GetType("TapiocaGH2.ArchicadProjectOptions")!;
        Type storyType = options.GetNestedType("Story", BindingFlags.NonPublic)!;
        Type snapshotType = options.GetNestedType("Snapshot", BindingFlags.NonPublic)!;
        Array stories = Array.CreateInstance(storyType, 1);
        stories.SetValue(Activator.CreateInstance(storyType, -1, "Basement", -3.2), 0);
        object snapshot = Activator.CreateInstance(snapshotType, [new[] { "Walls" }, new[] { "Solid" }, stories])!;
        MethodInfo set = options.GetMethod("Set", BindingFlags.NonPublic | BindingFlags.Static)!;
        MethodInfo clear = options.GetMethod("Clear", BindingFlags.NonPublic | BindingFlags.Static)!;
        Type bindings = choices[0].GetType().Assembly.GetType("TapiocaGH2.ArchicadPresetBindings")!;
        MethodInfo applyPresets = bindings.GetMethod("ApplyPresets", BindingFlags.NonPublic | BindingFlags.Static)!;
        MethodInfo shouldUpdate = bindings.GetMethod("ShouldUpdate", BindingFlags.NonPublic | BindingFlags.Static)!;
        try
        {
            set.Invoke(null, [snapshot, "", null, null]);
            foreach (Component choice in choices)
                applyPresets.Invoke(null, [choice]); // no editor UI loop in this headless fixture

            var picker = new PresetPickerObject();
            if (!document.Objects.Add(picker) || !Connections.Connect(picker, choices[0].Parameters.Input(0)))
                throw new InvalidOperationException("GH2 Preset Picker did not connect to the Layer Choice input.");
            if (picker.AvailablePresets.Presets.Length != 1 || picker.AvailablePresets.Presets[0].Name != "Walls")
                throw new InvalidOperationException("GH2 Preset Picker did not harvest the layer presets.");
            object changed = Activator.CreateInstance(snapshotType,
                [new[] { "Walls", "Ceilings" }, new[] { "Solid" }, stories])!;
            set.Invoke(null, [changed, "", null, null]);
            applyPresets.Invoke(null, [choices[0]]);
            Type changedType = options.GetNestedType("Changed", BindingFlags.NonPublic)!;
            object lineTypesChanged = Enum.Parse(changedType, "LineTypes");
            if ((bool)shouldUpdate.Invoke(null, [choices[0], lineTypesChanged])! ||
                !(bool)shouldUpdate.Invoke(null, [choices[1], lineTypesChanged])! ||
                (bool)shouldUpdate.Invoke(null, [choices[2], lineTypesChanged])!)
                throw new InvalidOperationException("A line-type change expired unrelated GH2 selectors.");
            if (picker.AvailablePresets.Presets.Length != 2 || picker.AvailablePresets.Presets[1].Name != "Ceilings")
                throw new InvalidOperationException("GH2 Preset Picker kept its old cached layer list after refresh.");
            picker.UserNames = ["Ceilings"];
            picker.Expire();
            foreach (Component choice in choices)
                choice.Expire();
            var result = Task.Run(() => document.Solution.StartWait(null, SolutionMode.Headless))
                .GetAwaiter().GetResult();
            if (result.Phase != SolutionPhase.Completed ||
                choices[0].Parameters.Output(0).State.Data.Tree()?.AllItems.Single()?.ToString() != "Ceilings" ||
                choices[1].Parameters.Output(0).State.Data.Tree()?.AllItems.Single()?.ToString() != "Solid" ||
                choices[2].Parameters.Output(0).State.Data.Tree()?.AllItems.Single()?.ToString() != "-1" ||
                choices[2].Parameters.Output(3).State.Data.Tree()?.ItemCount != 1)
                throw new InvalidOperationException("GH2 selectors did not solve from a copied project snapshot.");
        }
        finally { clear.Invoke(null, null); }
    }

    private static Guid ExposureId(Component component) => (Guid)(component.GetType()
        .GetProperty("ExposureId")?.GetValue(component)
        ?? throw new InvalidOperationException("A selector has no ExposureId."));
}
