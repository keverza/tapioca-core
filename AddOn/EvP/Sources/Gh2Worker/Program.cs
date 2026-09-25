using System.Runtime.CompilerServices;
using System.Runtime.Loader;

namespace Tapioca.Gh2Worker;

internal static class Program
{
    // The two-argument form is the repeatable headless fixture check. The
    // supervised form connects to the native bridge before starting Rhino.
    [STAThread]
    private static int Main(string[] args)
    {
        if (args.Length == 1 && args[0] == "--wire-check")
            return Gh2WireCheck.Run();
        bool proof = args.Length == 2 && !args[0].StartsWith("--", StringComparison.Ordinal);
        string? pluginPath = proof ? args[0] : null;
        string? fixturePath = proof ? args[1] : null;
        string? pipeName = null;
        string? bootLog = null;
        uint generation = 0;
        if (!proof)
        {
            if (args.Length % 2 != 0)
                return 2;
            for (int i = 0; i < args.Length; i += 2)
            {
                switch (args[i])
                {
                    case "--pipe": pipeName = args[i + 1]; break;
                    case "--plugin": pluginPath = args[i + 1]; break;
                    case "--boot-log": bootLog = args[i + 1]; break;
                    case "--generation" when uint.TryParse(args[i + 1], out generation): break;
                    case "--protocol" when args[i + 1] == Gh2Wire.Version.ToString(): break;
                    default: return 2;
                }
            }
            if (string.IsNullOrWhiteSpace(pipeName) || generation == 0)
                return 2;
        }

        string? systemDirectory = Environment.GetEnvironmentVariable("TAPIOCA_RHINO9_SYSTEM");
        systemDirectory ??= @"C:\Program Files\Rhino 9 WIP\System";
        if (!Directory.Exists(systemDirectory) || !File.Exists(pluginPath))
        {
            Note(bootLog, "Rhino 9 system directory or GH2 plugin is missing.");
            return 2;
        }

        Gh2PipeClient? bridge = null;
        try
        {
            if (pipeName is not null)
                bridge = Gh2PipeClient.Connect(pipeName);
            InitializeResolver(systemDirectory);
            string gh2Directory = Path.GetFullPath(Path.Combine(
                systemDirectory, "..", "Plug-ins", "Grasshopper2", "net8.0"));
            if (!File.Exists(Path.Combine(gh2Directory, "Grasshopper2.dll")))
                throw new FileNotFoundException("The pinned GH2 installation was not found.", gh2Directory);
            AssemblyLoadContext.Default.Resolving += (context, name) =>
            {
                if (name.Name != "Grasshopper2" && name.Name != "GrasshopperIO")
                    return null;
                string location = Path.Combine(gh2Directory, name.Name + ".dll");
                return File.Exists(location) ? context.LoadFromAssemblyPath(location) : null;
            };
            return proof ? RunProofAfterResolver(pluginPath!, fixturePath!)
                         : RunWorkerAfterResolver(pluginPath!, bridge!, generation);
        }
        catch (Exception error)
        {
            Note(bootLog, error.ToString());
            if (bridge is not null)
                Gh2Ping.AcknowledgeStartupFailure(bridge, error);
            return 1;
        }
        finally { bridge?.Dispose(); }
    }

    private static void Note(string? path, string text)
    {
        Console.Error.WriteLine(text);
        if (path is not null)
            File.AppendAllText(path, text + Environment.NewLine);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void InitializeResolver(string directory) => RhinoInside.Resolver.Initialize(directory);

    // Rhino types must not JIT until after the installed Rhino 9 resolver is up.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int RunProofAfterResolver(string pluginPath, string fixturePath) =>
        Gh2HeadlessProof.Run(pluginPath, fixturePath);

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int RunWorkerAfterResolver(string pluginPath, Gh2PipeClient bridge, uint generation)
    {
        using var backend = new Gh2Backend(pluginPath);
        Gh2Ping.ConfirmStartup(bridge, generation, "Rhino 9 / GH2 ready");
        bridge.WaitForShutdown();
        return 0;
    }
}
