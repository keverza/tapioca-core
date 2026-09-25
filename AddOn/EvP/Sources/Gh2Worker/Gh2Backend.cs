using System.Reflection;
using Grasshopper2.Components;
using Grasshopper2.Doc;
using Grasshopper2.Framework;

namespace Tapioca.Gh2Worker;

/// <summary>
/// One Rhino 9 core and one serialized GH2 document. The .rhp only owns
/// components; this class owns load, solve, input leases and teardown.
/// </summary>
internal sealed class Gh2Backend : IDisposable
{
    private static readonly Guid InputTypeId = new("9efec712-4356-45d9-9777-8aecd2b3e557");
    private static readonly Guid StatusTypeId = new("c512fd81-a693-47c4-95bf-30e3fa60b411");

    private readonly Rhino.Runtime.InProcess.RhinoCore core;
    private readonly MethodInfo installOverride;
    private Document? document;
    private CancellationTokenSource? activeSolve;
    private int solving;

    internal Gh2Backend(string pluginPath)
    {
        core = new Rhino.Runtime.InProcess.RhinoCore(
            ["/nosplash", "/notemplate"], Rhino.Runtime.InProcess.WindowStyle.NoWindow);
        try
        {
            if (typeof(Document).Assembly.GetName().Version != new Version(9, 0, 26258, 12303))
                throw new InvalidOperationException("The installed GH2 build differs from this worker's pinned SDK.");

            // The Player does not open the Rhino GH2 editor: doing so can load a
            // globally registered older .rhp before this worker's versioned one.
            PluginServer.ScopeStandardPlugins();
            var loaded = PluginServer.LoadAllScopedPlugins();
            if (loaded.failed != 0)
                throw new InvalidOperationException(
                    $"GH2 standard plugins: {loaded.loaded} loaded, {loaded.failed} failed.");

            pluginPath = Path.GetFullPath(pluginPath);
            PluginServer.ScopePlugin(pluginPath);
            if (ObjectProxies.FindById(InputTypeId) is null &&
                !PluginServer.LoadPlugin(pluginPath, out var failure))
                throw new InvalidOperationException(
                    $"GH2 refused TapiocaGH2.rhp: {failure.Kind}: {failure.Reason} {failure.Exception}");

            Type type = ObjectProxies.FindById(InputTypeId)?.Type
                ?? throw new InvalidOperationException("GH2 did not harvest Tapioca Number Input.");
            if (!string.Equals(Path.GetFullPath(type.Assembly.Location), pluginPath,
                    StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException(
                    $"GH2 loaded TapiocaGH2.rhp from '{type.Assembly.Location}', not '{pluginPath}'.");
            if (ObjectProxies.FindById(StatusTypeId)?.Type.Assembly != type.Assembly)
                throw new InvalidOperationException("This TapiocaGH2.rhp does not provide Archicad Connection Status.");

            installOverride = type.Assembly.GetType("TapiocaGH2.PlayerOverride")?
                .GetMethod("Install", BindingFlags.Public | BindingFlags.Static)
                ?? throw new InvalidOperationException("TapiocaGH2.rhp has no Player override entry point.");
            Console.WriteLine($"Rhino {Rhino.RhinoApp.Version}; GH2 {typeof(Document).Assembly.GetName().Version}");
            Console.WriteLine($"GH2 sha256 {HashFile(typeof(Document).Assembly.Location)}; " +
                              $"TapiocaGH2 sha256 {HashFile(pluginPath)}");
        }
        catch
        {
            core.Dispose();
            throw;
        }
    }

    internal Guid ExposureId => ReadExposureId(Input());

    internal void CreateDefinition()
    {
        CloseDefinition();
        Document candidate = Document.NewInertDocument();
        try
        {
            IDocumentObject input = ObjectProxies.TryEmit<IDocumentObject>(InputTypeId)
                ?? throw new InvalidOperationException("The input component could not be constructed.");
            if (!candidate.Objects.Add(input))
                throw new InvalidOperationException("The input component could not be added.");
            candidate.State = DocumentState.Active;
            document = candidate;
        }
        catch
        {
            candidate.Close();
            throw;
        }
    }

    internal void OpenDefinition(string path)
    {
        CloseDefinition();
        var io = new DocumentIO(trackFiles: false, reportErrors: false, resolvePlugins: false);
        if (!io.Open(path) || io.Document is null)
            throw new InvalidOperationException($"GH2 could not open '{path}'.");
        Document candidate = io.Document;
        try
        {
            // Opening a .ghz produces an inert document which will not compute.
            // The caller still decides when to expire and explicitly solve it.
            candidate.State = DocumentState.Active;
            var input = candidate.Objects.ActiveObjects.OfType<Component>().SingleOrDefault(
                item => item.GetType() == ObjectProxies.FindById(InputTypeId)?.Type);
            if (input is null)
                throw new InvalidOperationException("The definition has no restorable Tapioca Number Input.");
            if (candidate.Objects.ActiveObjects.Any(item => item.GetType().Name.StartsWith("Placeholder", StringComparison.Ordinal)))
                throw new InvalidOperationException("The definition has an unresolved GH2 component dependency.");
            document = candidate;
        }
        catch
        {
            candidate.Close();
            throw;
        }
    }

    internal void SaveCopy(string path)
    {
        if (Volatile.Read(ref solving) != 0)
            throw new InvalidOperationException("A definition cannot be saved during a Player solve.");
        if (!new DocumentIO(RequiredDocument(), trackFiles: false, reportErrors: false, resolvePlugins: false)
                .SaveCopy(path))
            throw new InvalidOperationException($"GH2 could not save '{path}'.");
    }

    internal void RemapDocumentIds()
    {
        RequiredDocument().Objects.ChangeAllIds();
    }

    internal double Solve(double? value)
    {
        if (Interlocked.CompareExchange(ref solving, 1, 0) != 0)
            throw new InvalidOperationException("GH2 already has an active solve.");

        var source = new CancellationTokenSource();
        Volatile.Write(ref activeSolve, source);
        try
        {
            Document current = RequiredDocument();
            Component input = Input();
            if (input.Parameters.Input(0).PersistentDataWeak.ItemCount != 1)
                throw new InvalidOperationException("The authored default was not restored from the .ghz.");
            using IDisposable? lease = value.HasValue
                ? (IDisposable)installOverride.Invoke(null, [current, ExposureId.ToString("D"), value.Value])!
                : null;

            // Staging the override does not expire anything. This is the
            // explicit Solve boundary; the solver itself runs off the UI thread.
            input.Expire();
            var result = Task.Run(() => current.Solution.StartWait(source, SolutionMode.Headless))
                .GetAwaiter().GetResult();
            if (result.Phase != SolutionPhase.Completed)
                throw new InvalidOperationException($"GH2 solution ended in {result.Phase}.");
            var tree = input.Parameters.Output(0).State.Data.Tree();
            if (tree?.ItemCount != 1)
                throw new InvalidOperationException($"GH2 produced {tree?.ItemCount ?? 0} number items.");
            return Convert.ToDouble(tree.AllItems.Single(), System.Globalization.CultureInfo.InvariantCulture);
        }
        finally
        {
            Volatile.Write(ref activeSolve, null);
            source.Dispose();
            Volatile.Write(ref solving, 0);
        }
    }

    // Cooperative tier. The supervising Archicad process owns the separate
    // hard-stop tier if GH2 or a third-party component ignores cancellation.
    internal void Cancel()
    {
        try { Volatile.Read(ref activeSolve)?.Cancel(); }
        catch (ObjectDisposedException) { }
        document?.Solution.Stop();
    }

    internal void CloseDefinition()
    {
        if (Volatile.Read(ref solving) != 0)
            throw new InvalidOperationException("The active solve must finish or the worker must stop.");
        Document? closing = document;
        document = null;
        closing?.Close();
    }

    public void Dispose()
    {
        try
        {
            CloseDefinition();
        }
        finally
        {
            core.Dispose();
        }
    }

    private Document RequiredDocument() => document
        ?? throw new InvalidOperationException("No GH2 definition is loaded.");

    private Component Input() => RequiredDocument().Objects.ActiveObjects.OfType<Component>().Single(
        item => item.GetType() == ObjectProxies.FindById(InputTypeId)?.Type);

    private static Guid ReadExposureId(IDocumentObject component)
    {
        PropertyInfo property = component.GetType().GetProperty("ExposureId")
            ?? throw new InvalidOperationException("This component is not a Tapioca Player input.");
        return (Guid)property.GetValue(component)!;
    }

    private static string HashFile(string path)
    {
        using var file = File.OpenRead(path);
        return Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(file));
    }
}
