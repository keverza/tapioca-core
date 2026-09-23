using System.Runtime.CompilerServices;
using System.Runtime.Loader;

namespace Tapioca.Gh2Worker;

internal static class Program
{
    // This executable is the headless GH2 runtime gate, not an Archicad bridge
    // endpoint yet. It creates and destroys its own RhinoCore per invocation.
    [STAThread]
    private static int Main(string[] args)
    {
        if (args.Length != 2)
        {
            Console.Error.WriteLine("Usage: Tapioca.Gh2Worker <TapiocaGH2.rhp> <fixture.ghz>");
            return 2;
        }

        string? systemDirectory = Environment.GetEnvironmentVariable("TAPIOCA_RHINO9_SYSTEM");
        systemDirectory ??= @"C:\Program Files\Rhino 9 WIP\System";
        if (!Directory.Exists(systemDirectory) || !File.Exists(args[0]))
        {
            Console.Error.WriteLine("Rhino 9 system directory or GH2 plugin is missing.");
            return 2;
        }

        try
        {
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
            return RunAfterResolver(args[0], args[1]);
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            return 1;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void InitializeResolver(string directory) => RhinoInside.Resolver.Initialize(directory);

    // Rhino types must not JIT until after the installed Rhino 9 resolver is up.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int RunAfterResolver(string pluginPath, string fixturePath) =>
        Gh2HeadlessProof.Run(pluginPath, fixturePath);
}
