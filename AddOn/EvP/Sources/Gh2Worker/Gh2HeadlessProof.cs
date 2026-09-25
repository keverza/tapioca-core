using Grasshopper2.Components;
using Grasshopper2.Doc;
using Grasshopper2.Framework;

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
        VerifyStatusComponent();
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

    private static void VerifyStatusComponent()
    {
        var statusId = new Guid("c512fd81-a693-47c4-95bf-30e3fa60b411");
        Document document = Document.NewInertDocument();
        try
        {
            IDocumentObject item = ObjectProxies.TryEmit<IDocumentObject>(statusId)
                ?? throw new InvalidOperationException("GH2 did not register Archicad Connection Status.");
            if (!document.Objects.Add(item))
                throw new InvalidOperationException("The GH2 status component was not added.");
            document.State = DocumentState.Active;
            Component component = (Component)item;
            component.Expire();
            var solution = Task.Run(() => document.Solution.StartWait(null, SolutionMode.Headless))
                .GetAwaiter().GetResult();
            if (solution.Phase != SolutionPhase.Completed)
                throw new InvalidOperationException($"GH2 status solution ended in {solution.Phase}.");
            string status = component.Parameters.Output(0).State.Data.Tree()?.AllItems.Single()?.ToString() ?? "";
            string endpoint = component.Parameters.Output(2).State.Data.Tree()?.AllItems.Single()?.ToString() ?? "";
            string addresses = component.Parameters.Output(4).State.Data.Tree()?.AllItems.Single()?.ToString() ?? "";
            if (status != "Disconnected" || endpoint != "(none)" || !addresses.Contains("127.0.0.1"))
                throw new InvalidOperationException("GH2 status did not show a disconnected local-only bridge.");
            Console.WriteLine("GH2 connection status component: disconnected, local addresses available.");
        }
        finally
        {
            document.Close();
        }
    }
}
