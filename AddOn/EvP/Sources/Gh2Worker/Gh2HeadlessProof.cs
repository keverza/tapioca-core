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
            if (ObjectProxies.FindById(new Guid("4aaab802-849a-443c-854a-83631e85968f")) is not null)
                throw new InvalidOperationException("AC Deconstruct is still registered.");
            Type referenceType = selection.GetType().Assembly.GetType("TapiocaGH2.AcElementRef")!;
            if (Grasshopper2.Types.Assistant.TypeAssistantServer.FindByType(referenceType)?.Type != referenceType)
                throw new InvalidOperationException("GH2 did not harvest the Archicad reference assistant.");
            var headerId = new Guid("5b531b0f-7b19-4cb1-8b05-4bdc349be451");
            Component headers = (Component)(ObjectProxies.TryEmit<IDocumentObject>(headerId)
                ?? throw new InvalidOperationException("GH2 did not register AC Element Headers."));
            if (!document.Objects.Add(headers) ||
                !Connections.Connect(((Component)selection).Parameters.Output(1), headers.Parameters.Input(0)))
                throw new InvalidOperationException("The typed reference could not wire to the lazy header query.");
            if (headers.Parameters.Inputs.Count() != 1 || headers.Parameters.Outputs.Count() != 4 ||
                headers.Parameters.Output(0).Nomen.Name != "Type" ||
                headers.Parameters.Output(1).Nomen.Name != "Element ID" ||
                headers.Parameters.Output(2).Nomen.Name != "Story" ||
                headers.Parameters.Output(3).Nomen.Name != "Layer")
                throw new InvalidOperationException("AC Element Headers still exposes removed outputs.");
            var queryIds = new[]
            {
                "839335bd-cf50-4598-9765-f158c4b68c59",
                "1aca73bb-0231-4a9c-921e-37aba8546155",
                "72d1643e-0e64-4e37-95c4-5ffbc73cd7a7",
                "da78e1ea-f789-4246-ae25-1a42166ef404",
                "859af4c8-e7dd-484e-a03c-338d7574b50f"
            };
            var presetChecks = new List<(PresetPickerObject Picker, string Value)>();
            foreach (string id in queryIds)
            {
                Component query = (Component)(ObjectProxies.TryEmit<IDocumentObject>(new Guid(id))
                    ?? throw new InvalidOperationException($"GH2 did not register query component {id}."));
                if (!document.Objects.Add(query) ||
                    !Connections.Connect(((Component)selection).Parameters.Output(1), query.Parameters.Input(0)) ||
                     query.Parameters.Inputs.Count() != 4 || query.Parameters.Outputs.Count() != (id == queryIds[3] ? 6 : 5))
                    throw new InvalidOperationException($"The GH2 element query {id} is not wired or aligned.");
                Type outputType = query.Parameters.Output(0).GetType().BaseType!.BaseType!.GetGenericArguments().FirstOrDefault()
                    ?? throw new InvalidOperationException("Query result has no typed parameter.");
                if (Grasshopper2.Types.Assistant.TypeAssistantServer.FindByType(outputType)?.Type != outputType)
                    throw new InvalidOperationException($"GH2 did not harvest the query assistant for {outputType}.");
                if (string.IsNullOrWhiteSpace(query.Parameters.Output(0).Nomen.Name) ||
                    string.IsNullOrWhiteSpace(query.Parameters.Output(0).DisplayName))
                    throw new InvalidOperationException($"The query {id} has an unnamed output port.");
                var outputId = query.Parameters.Output(0).GetType().GetCustomAttributes(false)
                    .OfType<GrasshopperIO.IoIdAttribute>().Single().Id;
                if (ObjectProxies.FindById(outputId)?.Nomen.Rank != Grasshopper2.UI.Rank.Hidden)
                    throw new InvalidOperationException($"The query {id} exposes its internal output parameter in the palette.");
                var modes = new[]
                {
                     new[] { "Definition", "Boundary" },
                    new[] { "Hosted", "Connected", "Inferred" },
                     new[] { "Element settings", "Discover", "All", "User", "Built-in" },
                     new[] { "Bounding box", "Surface mesh", "2D drawing" },
                    new[] { "Instance", "All parameters" }
                }[Array.IndexOf(queryIds, id)];
                var modePicker = new PresetPickerObject();
                if (!document.Objects.Add(modePicker) ||
                    !Connections.Connect(modePicker, query.Parameters.Input(1)) ||
                    !modePicker.AvailablePresets.Presets.Select(preset => preset.Name).SequenceEqual(modes))
                    throw new InvalidOperationException($"The query {id} Representation input rejected its Preset Picker.");
                modePicker.UserNames = [modes[1]];
                presetChecks.Add((modePicker, modes[1]));
                if (id == queryIds[2] || id == queryIds[4])
                {
                    var selectorPicker = new PresetPickerObject();
                    if (!document.Objects.Add(selectorPicker) ||
                        !Connections.Connect(selectorPicker, query.Parameters.Input(2)))
                        throw new InvalidOperationException($"The query {id} Selectors input rejected its Preset Picker.");
                    Type queryBase = query.GetType().BaseType!;
                    MethodInfo parse = queryBase.GetMethod("ParseRows", BindingFlags.NonPublic | BindingFlags.Static)!;
                    Guid sourceId = Guid.NewGuid();
                    string key = id == queryIds[2] ? Guid.NewGuid().ToString("D") : "sash_count";
                    string label = id == queryIds[2] ? "name" : "label";
                    string reply = "{\"ok\":true,\"data\":{\"elements\":[{\"guid\":\"" + sourceId +
                        "\",\"status\":\"Success\",\"diagnostic\":\"\",\"more\":false," +
                        "\"items\":[{\"key\":\"" + key + "\",\"group\":\"Dimensions\",\"" +
                        label + "\":\"Height\"}]}]}}";
                    Array rows = (Array)parse.Invoke(null, [reply, new[] { sourceId }])!;
                    queryBase.GetMethod("ApplySelectorPresets", BindingFlags.NonPublic | BindingFlags.Instance)!
                        .Invoke(query, [rows]);
                    var available = selectorPicker.AvailablePresets.Presets;
                    if (available.Length != 1 || !available[0].Name.Contains("Height", StringComparison.Ordinal))
                        throw new InvalidOperationException($"The query {id} did not refresh its selectable keys.");
                    selectorPicker.UserNames = [available[0].Name];
                    presetChecks.Add((selectorPicker, key));
                    queryBase.GetMethods(BindingFlags.NonPublic | BindingFlags.Instance)
                        .Single(method => method.Name.EndsWith(".ClearSelectorPresets", StringComparison.Ordinal))
                        .Invoke(query, null);
                    if (selectorPicker.AvailablePresets.Presets.Length != 0 ||
                        modePicker.AvailablePresets.Presets.Length != modes.Length)
                        throw new InvalidOperationException($"The query {id} retained stale keys or lost its modes after refresh.");
                    queryBase.GetMethod("ApplySelectorPresets", BindingFlags.NonPublic | BindingFlags.Instance)!
                        .Invoke(query, [rows]);
                    if (selectorPicker.AvailablePresets.Presets.Length != 1)
                        throw new InvalidOperationException($"The query {id} did not restore discoverable keys.");
                }
            }
            var meshAction = (Component)(ObjectProxies.TryEmit<IDocumentObject>(
                new Guid("f35e5e29-e424-40c9-8493-34e682e9a22b"))
                ?? throw new InvalidOperationException("GH2 did not register the outputless Design mesh action."));
            if (ObjectProxies.FindById(new Guid("ee94cef3-7073-4a37-a338-24e3a5e02d80")) is not null ||
                !document.Objects.Add(meshAction) || meshAction.Parameters.InputCount != 3 ||
                meshAction.Parameters.OutputCount != 0 ||
                ObjectProxies.FindById(new Guid("f35e5e29-e424-40c9-8493-34e682e9a22b"))?.Nomen.Section != "Design")
                throw new InvalidOperationException("The Design action must be a zero-output component.");
            var unique = (Component)(ObjectProxies.TryEmit<IDocumentObject>(
                new Guid("8a6fe2d8-5b6b-4824-ae43-1d5506a81b61"))
                ?? throw new InvalidOperationException("GH2 did not register Util / Unique Archicad Elements."));
            var tint = (Component)(ObjectProxies.TryEmit<IDocumentObject>(
                new Guid("35be9ebe-1510-4fe1-a922-fd6e8e863210"))
                ?? throw new InvalidOperationException("GH2 did not register Display / Preview Tint."));
            var container = (Component)(ObjectProxies.TryEmit<IDocumentObject>(
                new Guid("b44115d6-96aa-4692-94f7-8e9304bf4503"))
                ?? throw new InvalidOperationException("GH2 did not register Input / Archicad Element Container."));
            if (!document.Objects.Add(unique) || !document.Objects.Add(tint) ||
                !document.Objects.Add(container) ||
                !Connections.Connect(((Component)selection).Parameters.Output(1), unique.Parameters.Input(0)) ||
                !Connections.Connect(((Component)selection).Parameters.Output(1), container.Parameters.Input(0)) ||
                unique.Parameters.OutputCount != 1 || tint.Parameters.OutputCount != 1 ||
                container.Parameters.OutputCount != 0 ||
                ObjectProxies.FindById(new Guid("f35e5e29-e424-40c9-8493-34e682e9a22b"))?.Nomen.Section != "Design" ||
                ObjectProxies.FindById(headerId)?.Nomen.Section != "Deconstruct")
                throw new InvalidOperationException("The six-category GH2 component interface was not registered.");
            Type typePort = container.GetType().GetNestedType("TypePort", BindingFlags.NonPublic)!;
            Array MakePorts(params (int Id, int Variation, string Name)[] items)
            {
                Array ports = Array.CreateInstance(typePort, items.Length);
                for (int i = 0; i < items.Length; i++)
                    ports.SetValue(Activator.CreateInstance(typePort, items[i].Id, items[i].Variation, items[i].Name), i);
                return ports;
            }
            MethodInfo syncPorts = container.GetType().GetMethod("SyncPorts", BindingFlags.NonPublic | BindingFlags.Instance)!;
            syncPorts.Invoke(container, [MakePorts((1, 0, "Wall"), (2, 0, "Slab"))]);
            var wallPort = container.Parameters.Output(0);
            if (container.Parameters.OutputCount != 2 || wallPort.Nomen.Name != "Wall")
                throw new InvalidOperationException("The element container did not create type-specific ports.");
            syncPorts.Invoke(container, [MakePorts((2, 0, "Slab"), (1, 0, "Wall"))]);
            if (container.Parameters.Output(0) != wallPort)
                throw new InvalidOperationException("Reordering GUIDs replaced a type's output identity.");
            var prepare = meshAction.GetType().GetMethod("Prepare", BindingFlags.NonPublic | BindingFlags.Static)!;
            object preset = prepare.Invoke(null, [null, Array.Empty<Rhino.Geometry.Curve>(), "", "test-project", 1.0])!;
            if (((double[])preset.GetType().GetProperty("Outline")!.GetValue(preset)!).Length != 8 ||
                ((double[])preset.GetType().GetProperty("PolyZ")!.GetValue(preset)!).Length != 4 ||
                !((int[])preset.GetType().GetProperty("RidgeCounts")!.GetValue(preset)!).SequenceEqual([3]))
                throw new InvalidOperationException("The mesh bake test preset needs one three-vertex ridge line.");
            var ridge = new Rhino.Geometry.PolylineCurve(new[] { new Rhino.Geometry.Point3d(1, 1, 0),
                new Rhino.Geometry.Point3d(2, 2, 1), new Rhino.Geometry.Point3d(3, 3, 0) });
            object withRidge = prepare.Invoke(null, [null, new Rhino.Geometry.Curve[] { ridge }, "", "test-project", 1.0])!;
            if (!((int[])withRidge.GetType().GetProperty("RidgeCounts")!.GetValue(withRidge)!).SequenceEqual([3]))
                throw new InvalidOperationException("A GH2 ridge polyline was reduced to one-point level lines.");
            var secondRidge = new Rhino.Geometry.PolylineCurve(new[] {
                new Rhino.Geometry.Point3d(1, 3, 0), new Rhino.Geometry.Point3d(3, 3, 2) });
            object grouped = prepare.Invoke(null, [null, new Rhino.Geometry.Curve[] { ridge, secondRidge },
                "", "test-project", 1.0])!;
            if (!((int[])grouped.GetType().GetProperty("RidgeCounts")!.GetValue(grouped)!).SequenceEqual([3, 2]) ||
                ((double[])grouped.GetType().GetProperty("RidgeCoords")!.GetValue(grouped)!).Length != 15)
                throw new InvalidOperationException("Multiple ridge polylines lost their native line boundaries.");
            var pluginAssembly = meshAction.GetType().Assembly;
            var ring = Activator.CreateInstance(pluginAssembly.GetType("TapiocaGH2.AcContourRing")!,
                "outer", true, new double[] { 0, 0, 2, 0, 2, 2, 0, 2 }, new double[] { 0, 0, 0, 0 })!;
            var curve = (Rhino.Geometry.Curve)pluginAssembly.GetType("TapiocaGH2.ArchicadGetContours")!
                .GetMethod("BuildPlanCurve", BindingFlags.NonPublic | BindingFlags.Static)!
                .Invoke(null, [ring, 1.0])!;
            if (!curve.IsValid || !curve.IsClosed)
                throw new InvalidOperationException("An Archicad contour did not become a Rhino closed curve.");
            var triangles = Activator.CreateInstance(pluginAssembly.GetType("TapiocaGH2.AcSurfaceMesh")!,
                new double[] { 0, 0, 0, 2, 0, 0, 0, 2, 1 }, new int[] { 0, 1, 2 })!;
            var rhinoMesh = (Rhino.Geometry.Mesh)pluginAssembly.GetType("TapiocaGH2.ArchicadGetGeometry")!
                .GetMethod("ToRhinoMesh", BindingFlags.NonPublic | BindingFlags.Static)!
                .Invoke(null, [triangles, 1.0])!;
            if (!rhinoMesh.IsValid || rhinoMesh.Faces.Count != 1 || rhinoMesh.Vertices.Count != 3)
                throw new InvalidOperationException("Archicad tessellation did not become a Rhino mesh.");
            var refType = pluginAssembly.GetType("TapiocaGH2.AcElementRef")!;
            object sourceRef = Activator.CreateInstance(refType, "test-project", Guid.NewGuid())!;
            using var drawingJson = System.Text.Json.JsonDocument.Parse("[{\"xy\":[0,0,1,0,1,1],\"closed\":false}]");
            object drawing = pluginAssembly.GetType("TapiocaGH2.ArchicadGetGeometry")!
                .GetMethod("Convert", BindingFlags.NonPublic | BindingFlags.Instance)!
                .Invoke(Activator.CreateInstance(pluginAssembly.GetType("TapiocaGH2.ArchicadGetGeometry")!)!,
                    [sourceRef, "2D drawing", drawingJson.RootElement.EnumerateArray().ToArray()])!;
            Array paths = (Array)drawing.GetType().GetProperty("Paths")!.GetValue(drawing)!;
            if (paths.Length != 1 || !((double[])paths.GetValue(0)!.GetType().GetProperty("Xy")!
                    .GetValue(paths.GetValue(0))!).SequenceEqual([0, 0, 1, 0, 1, 1]))
                throw new InvalidOperationException("Archicad 2D drawing paths did not survive the managed query.");
            Type queries = selection.GetType().Assembly.GetType("TapiocaGH2.ArchicadElementQuery`1")!
                .MakeGenericType(selection.GetType().Assembly.GetType("TapiocaGH2.AcPropertySet")!);
            MethodInfo parseQueries = queries.GetMethod("ParseRows", BindingFlags.NonPublic | BindingFlags.Static)!;
            Guid missingQueryId = Guid.NewGuid();
            Guid validQueryId = Guid.NewGuid();
            string alignedQuery = "{\"ok\":true,\"data\":{\"elements\":[" +
                "{\"guid\":\"" + missingQueryId + "\",\"status\":\"Unavailable\",\"diagnostic\":\"Missing\",\"items\":[],\"more\":false}," +
                "{\"guid\":\"" + validQueryId + "\",\"status\":\"NotApplicable\",\"diagnostic\":\"No 3D\",\"items\":[],\"more\":false}]}}";
            Array queryRows = (Array)parseQueries.Invoke(null, [alignedQuery, new[] { missingQueryId, validQueryId }])!;
            if (queryRows.Length != 2 || queryRows.GetValue(1)?.GetType().GetProperty("Diagnostic")?.GetValue(queryRows.GetValue(1))?.ToString() != "No 3D")
                throw new InvalidOperationException("A missing query result shifted the next element's diagnostic.");
            try
            {
                parseQueries.Invoke(null, [alignedQuery, new[] { validQueryId, missingQueryId }]);
                throw new InvalidOperationException("GH2 accepted misaddressed query results.");
            }
            catch (TargetInvocationException error) when (error.InnerException is InvalidDataException) { }
            MethodInfo mergeQueries = queries.GetMethod("MergePages", BindingFlags.NonPublic | BindingFlags.Static)!;
            string FirstPage(string key, bool more, string status = "Success") =>
                "{\"ok\":true,\"data\":{\"elements\":[{\"guid\":\"" + validQueryId +
                "\",\"status\":\"" + status + "\",\"diagnostic\":\"\",\"items\":[{\"key\":\"" + key +
                "\"}],\"more\":" + (more ? "true" : "false") + "}]}}";
            Array firstPage = (Array)parseQueries.Invoke(null, [FirstPage("first", true), new[] { validQueryId }])!;
            Array lastPage = (Array)parseQueries.Invoke(null, [FirstPage("last", false), new[] { validQueryId }])!;
            Array mergedPages = (Array)mergeQueries.Invoke(null, [firstPage, lastPage])!;
            var mergedItems = (System.Text.Json.JsonElement[])mergedPages.GetValue(0)!.GetType()
                .GetProperty("Items")!.GetValue(mergedPages.GetValue(0))!;
            if (mergedItems.Length != 2 || mergedItems[0].GetProperty("key").GetString() != "first" ||
                mergedItems[1].GetProperty("key").GetString() != "last")
                throw new InvalidOperationException("GH2 did not merge property discovery pages in order.");
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
            foreach (var (picker, expected) in presetChecks)
                if (picker.State.Data.Tree()?.AllItems.SingleOrDefault()?.ToString() != expected)
                    throw new InvalidOperationException($"GH2 Preset Picker did not emit the selected input value {expected}.");
            string[] diagnostics = component.Parameters.Output(0).State.Data.Tree()?.AllItems
                .Select(item => item.ToString() ?? "").ToArray() ?? [];
            if (component.State.Data.Messages.WarningCount == 0 ||
                component.Parameters.Outputs.Count() != 1 || diagnostics.Length != 10 ||
                diagnostics[0] != "Status: Disconnected" || diagnostics[2] != "Endpoint: (none)" ||
                !diagnostics[4].Contains("127.0.0.1") ||
                diagnostics[9] != "Description: Project description for Tapioca.")
                throw new InvalidOperationException("GH2 status did not show a disconnected local-only bridge.");
            var tintValue = tint.Parameters.Output(0).State.Data.Tree()?.AllItems.SingleOrDefault();
            if (tintValue is not Grasshopper2.Types.Colour.Colour colour || Math.Abs(colour.Alpha - 0.5f) > 0.01f)
                throw new InvalidOperationException("Display / Preview Tint did not emit its alpha colour: " +
                    (tintValue?.ToString() ?? "no output") + "; " +
                    string.Join(" | ", tint.State.Data.Messages.Errors.Concat(tint.State.Data.Messages.Warnings)) +
                    "; input items=" + tint.Parameters.Input(0).PersistentDataWeak.ItemCount + "/" +
                    tint.Parameters.Input(1).PersistentDataWeak.ItemCount + "; input state=" +
                    (tint.Parameters.Input(0).State.Data.Tree()?.AllItems.FirstOrDefault()?.GetType().FullName ?? "none") +
                    "/" + (tint.Parameters.Input(1).State.Data.Tree()?.AllItems.FirstOrDefault()?.GetType().FullName ?? "none"));
            foreach (Component choice in choiceComponents)
            {
                int availableOutput = choice.Parameters.Outputs.Count() - 1;
                int count = choice.Parameters.Output(availableOutput).State.Data.Tree()?.ItemCount ?? 0;
                if (count != 0)
                    throw new InvalidOperationException("A disconnected selector exposed stale Archicad choices.");
            }
            VerifyPopulatedSelectors(document, choiceComponents, meshAction);
            Guid selectedId = Guid.NewGuid();
            VerifyElementBinding(selection.GetType().Assembly, selectedId);
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
            selection.GetType().GetField("projectKey", BindingFlags.Instance | BindingFlags.NonPublic)!
                .SetValue(selection, "proof-project");
            ((Component)selection).Expire();
            var selectedSolution = Task.Run(() => document.Solution.StartWait(null, SolutionMode.Headless))
                .GetAwaiter().GetResult();
            object? typed = ((Component)selection).Parameters.Output(1).State.Data.Tree()?.AllItems.SingleOrDefault();
            var typedMeta = ((Component)selection).Parameters.Output(1).State.Data.Tree()?.Twigs[0].MetaAt(0);
            var guidName = new Grasshopper2.Data.Meta.MetaName("Tapioca", "Source", "Guid");
            if (selectedSolution.Phase != SolutionPhase.Completed ||
                ((Component)selection).Parameters.Output(0).State.Data.Tree()?.AllItems.Single()?.ToString() != selectedId.ToString("D") ||
                typed?.GetType() != referenceType ||
                (Guid)referenceType.GetProperty("ElementId")!.GetValue(typed)! != selectedId ||
                typedMeta is null || !typedMeta.Get(guidName, out Guid metaId) || metaId != selectedId)
                throw new InvalidOperationException("GH2 selection did not emit a typed Archicad reference with provenance.");
            if (unique.Parameters.Output(0).State.Data.Tree()?.AllItems.SingleOrDefault()?.GetType() != referenceType)
                throw new InvalidOperationException("Util / Unique Archicad Elements lost its typed selection.");
            MethodInfo parseHeaders = headers.GetType().GetMethod("ParseHeaders", BindingFlags.NonPublic | BindingFlags.Static)!;
            string response = "{\"ok\":true,\"data\":{\"elements\":[{\"guid\":\"" + selectedId.ToString("D") +
                "\",\"found\":true,\"type\":\"Wall\",\"elementId\":\"W-1\"," +
                "\"story\":0,\"layer\":\"Walls\"}]}}";
            Array parsedHeaders = (Array)parseHeaders.Invoke(null, [response, new[] { selectedId }])!;
            if (parsedHeaders.Length != 1 || parsedHeaders.GetValue(0)?.GetType()
                    .GetProperty("ElementId")?.GetValue(parsedHeaders.GetValue(0))?.ToString() != "W-1")
                throw new InvalidOperationException("GH2 lost a basic Archicad element field.");
            try
            {
                parseHeaders.Invoke(null, [response, new[] { Guid.NewGuid() }]);
                throw new InvalidOperationException("GH2 accepted another element's header as its own.");
            }
            catch (TargetInvocationException error) when (error.InnerException is InvalidDataException) { }
            Guid missingId = Guid.NewGuid();
            string missingFirst = "{\"ok\":true,\"data\":{\"elements\":[{\"guid\":\"" + missingId.ToString("D") +
                "\",\"found\":false,\"type\":\"\",\"elementId\":\"\",\"story\":0,\"layer\":\"\"}," +
                response.Split("\"elements\":[", 2)[1];
            Array aligned = (Array)parseHeaders.Invoke(null, [missingFirst, new[] { missingId, selectedId }])!;
            if (aligned.Length != 2 ||
                (bool)aligned.GetValue(0)!.GetType().GetProperty("Found")!.GetValue(aligned.GetValue(0))! ||
                aligned.GetValue(1)?.GetType().GetProperty("ElementId")?.GetValue(aligned.GetValue(1))?.ToString() != "W-1")
                throw new InvalidOperationException("A missing element shifted the next header record.");
            var slots = Grasshopper2.Data.Garden.TwigFromPears<string>(
                [null!, Grasshopper2.Data.Garden.Pear("Wall")]);
            if (slots.LeafCount != 2 || !slots.NullAt(0) || slots.NullAt(1))
                throw new InvalidOperationException("GH2 did not keep a null slot for an unresolved element.");
            Type parameterType = selection.GetType().Assembly.GetType("TapiocaGH2.AcElementParameter")!;
            var parameterId = new Guid("fb101c48-204a-49da-85d0-22cb6e655331");
            if (ObjectProxies.FindById(parameterId)?.Nomen.Rank != Grasshopper2.UI.Rank.Hidden)
                throw new InvalidOperationException("The typed Archicad parameter is still a visible duplicate of AC Element.");
            var standalone = (IParameter)(Activator.CreateInstance(parameterType)
                ?? throw new InvalidOperationException("GH2 could not construct an Archicad reference parameter."));
            var typedPear = ((Component)selection).Parameters.Output(1).State.Data.Tree()!.Twigs[0]
                .ToPearArray(Grasshopper2.Data.ToArrayMethod.Always)[0];
            standalone.PersistentDataWeak = Grasshopper2.Data.Garden.ITreeFromIPears([typedPear]);
            if (!document.Objects.Add((IDocumentObject)standalone))
                throw new InvalidOperationException("GH2 could not add a persistent Archicad reference parameter.");
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
                    Component? restoredContainer = io.Document.Objects.ActiveObjects.OfType<Component>()
                        .FirstOrDefault(component => component.GetType() == container.GetType());
                    if (restoredContainer?.Parameters.OutputCount != 2 ||
                        restoredContainer.Parameters.Output(0).Nomen.Name != "Wall" ||
                        restoredContainer.Parameters.Output(0).InstanceId != wallPort.InstanceId)
                        throw new InvalidOperationException("GH2 did not preserve container type ports on save/reopen.");
                    IParameter? savedParameter = io.Document.Objects.ActiveObjects.OfType<IParameter>()
                        .FirstOrDefault(parameter => parameter.GetType() == parameterType);
                    object? savedReference = savedParameter?.PersistentDataWeak.AllItems.SingleOrDefault();
                    if (savedReference?.GetType() != referenceType ||
                        (Guid)referenceType.GetProperty("ElementId")!.GetValue(savedReference)! != selectedId ||
                        referenceType.GetProperty("ProjectKey")!.GetValue(savedReference)?.ToString() != "proof-project" ||
                        savedParameter?.PersistentDataWeak.Twigs[0].MetaAt(0)
                            .Get(guidName, out Guid savedMetaId) != true || savedMetaId != selectedId)
                        throw new InvalidOperationException("GH2 did not serialize the typed Archicad reference.");
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

    private static void VerifyElementBinding(Assembly plugin, Guid id)
    {
        var idOfComponent = new Guid("5cb5887b-df3e-47e5-bc21-eb922d2d16ca");
        Document document = Document.NewInertDocument();
        Type options = plugin.GetType("TapiocaGH2.ArchicadProjectOptions")!;
        Type snapshotType = options.GetNestedType("Snapshot", BindingFlags.NonPublic)!;
        Type storyType = options.GetNestedType("Story", BindingFlags.NonPublic)!;
        object snapshot = Activator.CreateInstance(snapshotType,
            [Array.Empty<string>(), Array.Empty<string>(), Array.CreateInstance(storyType, 0)])!;
        try
        {
            Component source = (Component)(ObjectProxies.TryEmit<IDocumentObject>(idOfComponent)
                ?? throw new InvalidOperationException("GH2 did not register AC Element."));
            if (!document.Objects.Add(source))
                throw new InvalidOperationException("GH2 could not add AC Element.");
            ((Grasshopper2.Parameters.Standard.GuidParameter)source.Parameters.Input(0)).Set(id);
            options.GetMethod("Set", BindingFlags.NonPublic | BindingFlags.Static)!
                .Invoke(null, [snapshot, "proof-project", null, null]);
            document.State = DocumentState.Active;
            var solution = Task.Run(() => document.Solution.StartWait(null, SolutionMode.Headless))
                .GetAwaiter().GetResult();
            object? reference = source.Parameters.Output(0).State.Data.Tree()?.AllItems.SingleOrDefault();
            if (solution.Phase != SolutionPhase.Completed ||
                (Guid?)reference?.GetType().GetProperty("ElementId")?.GetValue(reference) != id ||
                reference.GetType().GetProperty("ProjectKey")?.GetValue(reference)?.ToString() != "proof-project")
                throw new InvalidOperationException("AC Element did not bind the GUID to the copied project identity.");
        }
        finally
        {
            options.GetMethod("Clear", BindingFlags.NonPublic | BindingFlags.Static)!.Invoke(null, null);
            document.Close();
        }
    }

    private static void VerifyPopulatedSelectors(Document document, List<Component> choices, Component meshAction)
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
            applyPresets.Invoke(null, [meshAction]);
            var layerPicker = new PresetPickerObject();
            if (!document.Objects.Add(layerPicker) || !Connections.Connect(layerPicker, meshAction.Parameters.Input(2)) ||
                !layerPicker.AvailablePresets.Presets.Select(preset => preset.Name)
                    .SequenceEqual(new[] { "Mesh tool default", "Walls" }))
                throw new InvalidOperationException("The terrain mesh Layer input rejected its Preset Picker.");
            foreach (Component choice in choices.Skip(1))
            {
                var choicePicker = new PresetPickerObject();
                if (!document.Objects.Add(choicePicker) ||
                    !Connections.Connect(choicePicker, choice.Parameters.Input(0)) ||
                    choicePicker.AvailablePresets.Presets.Length != 1)
                    throw new InvalidOperationException("An Archicad project choice rejected its Preset Picker.");
            }

            var picker = new PresetPickerObject();
            if (!document.Objects.Add(picker) || !Connections.Connect(picker, choices[0].Parameters.Input(0)))
                throw new InvalidOperationException("GH2 Preset Picker did not connect to the Layer Choice input.");
            if (picker.AvailablePresets.Presets.Length != 1 || picker.AvailablePresets.Presets[0].Name != "Walls")
                throw new InvalidOperationException("GH2 Preset Picker did not harvest the layer presets.");
            object changed = Activator.CreateInstance(snapshotType,
                [new[] { "Walls", "Ceilings" }, new[] { "Solid" }, stories])!;
            set.Invoke(null, [changed, "", null, null]);
            applyPresets.Invoke(null, [choices[0]]);
            applyPresets.Invoke(null, [meshAction]);
            Type changedType = options.GetNestedType("Changed", BindingFlags.NonPublic)!;
            object lineTypesChanged = Enum.Parse(changedType, "LineTypes");
            if ((bool)shouldUpdate.Invoke(null, [choices[0], lineTypesChanged])! ||
                !(bool)shouldUpdate.Invoke(null, [choices[1], lineTypesChanged])! ||
                (bool)shouldUpdate.Invoke(null, [choices[2], lineTypesChanged])!)
                throw new InvalidOperationException("A line-type change expired unrelated GH2 selectors.");
            if (picker.AvailablePresets.Presets.Length != 2 || picker.AvailablePresets.Presets[1].Name != "Ceilings")
                throw new InvalidOperationException("GH2 Preset Picker kept its old cached layer list after refresh.");
            if (layerPicker.AvailablePresets.Presets.Length != 3 ||
                layerPicker.AvailablePresets.Presets[2].Name != "Ceilings")
                throw new InvalidOperationException("The terrain mesh layer Picker kept stale project choices.");
            layerPicker.UserNames = ["Ceilings"];
            layerPicker.Expire();
            picker.UserNames = ["Ceilings"];
            picker.Expire();
            foreach (Component choice in choices)
                choice.Expire();
            var result = Task.Run(() => document.Solution.StartWait(null, SolutionMode.Headless))
                .GetAwaiter().GetResult();
            if (result.Phase != SolutionPhase.Completed ||
                layerPicker.State.Data.Tree()?.AllItems.Single()?.ToString() != "Ceilings" ||
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
