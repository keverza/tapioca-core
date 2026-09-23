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
}
