using System.Runtime.Versioning;
using System.Text.RegularExpressions;
using DynamoShapeManager;

/// <summary>
/// Finds the Autodesk Shape Manager (ASM) binaries Dynamo needs before ANY geometry
/// node can evaluate.
///
/// WHY THIS EXISTS. Dynamo does not ship ASM: it locates an installed Autodesk
/// product and preloads ASM from there, then binds the matching LibG interop in
/// <c>libg_&lt;major&gt;_&lt;minor&gt;_&lt;build&gt;</c> beside DynamoCore. When that
/// search fails, <c>StartupUtils.PreloadASM</c> swallows the failure and leaves the
/// preloader location EMPTY, so the geometry factory path collapses to the runtime
/// root and the first geometry node dies with
///
///     Could not load file or assembly '...\LibG.ProtoInterface.dll'
///
/// which names a file that was never supposed to be there and says nothing about the
/// missing dependency. Probing first turns that into one sentence the user can act on.
///
/// WHAT COUNTS AS COMPATIBLE. The supported majors are not hardcoded — they are the
/// <c>libg_*</c> folders the pinned runtime actually ships, newest first. Dynamo
/// matches ASM to LibG on the MAJOR version (see
/// <c>DynamoShapeManager.Utilities.GetInstalledAsmVersion2</c>), and
/// <c>GetLibGPreloaderLocation</c> will only fall back to a LOWER libg folder — so an
/// ASM older than every shipped libg cannot be used at all, however close it looks.
/// </summary>
internal static class GeometryLibrary
{
    /// <summary>An explicit ASM directory, for a product this probe cannot find.</summary>
    internal const string OverrideVariable = "TAPIOCA_DYNAMO_ASM_DIR";

    private static readonly Regex LibGFolder = new(@"^libg_(\d+)_(\d+)_(\d+)$", RegexOptions.IgnoreCase);

    /// <param name="Path">ASM directory to hand Dynamo, or empty when none was found.</param>
    /// <param name="Loaded">Whether geometry nodes can be expected to work at all.</param>
    /// <param name="Message">One sentence for the palette's status line.</param>
    internal readonly record struct Probe(string Path, bool Loaded, string Message);

    /// <summary>
    /// The pinned Dynamo runtime directory. The host starts this process THERE (see
    /// DynamoHost.cpp), which is also what makes the assembly resolver below work, so
    /// the current directory is the runtime root rather than the runner's own folder.
    /// </summary>
    internal static string RuntimeRoot => Environment.CurrentDirectory;

    // Windows-only, and honestly so: the whole runner exists to be launched by a
    // Windows Archicad add-on, and DynamoShapeManager's ASM search is itself
    // [SupportedOSPlatform("windows")].
    [SupportedOSPlatform("windows")]
    internal static Probe Find()
    {
        var root = RuntimeRoot;

        // The override wins and is never second-guessed: it exists precisely for a
        // product arrangement this probe does not know about. It is still checked for
        // existence, because a typo here would otherwise surface as the same
        // unreadable LibG failure it was set to avoid.
        var supported = SupportedVersions(root);

        var configured = Environment.GetEnvironmentVariable(OverrideVariable);
        if (!string.IsNullOrWhiteSpace(configured))
        {
            return CheckOverride(Environment.ExpandEnvironmentVariables(configured.Trim()), supported);
        }

        if (supported.Count == 0)
        {
            return new Probe(string.Empty, false,
                "No libg_* folder in the Dynamo runtime; geometry nodes will fail.");
        }

        try
        {
            var location = string.Empty;
            var version = Utilities.GetInstalledAsmVersion2(supported, ref location, root);
            if (version is not null && !string.IsNullOrEmpty(location) && Directory.Exists(location))
            {
                return new Probe(location, true, $"Geometry library ASM {version} loaded.");
            }
        }
        catch (Exception exception)
        {
            return new Probe(string.Empty, false, $"Geometry library search failed: {exception.Message}");
        }

        // Named majors rather than "not found": the user has to install a product that
        // carries one of THESE, and no other number will do.
        var majors = string.Join(" or ", supported.Select(version => version.Major));
        return new Probe(string.Empty, false,
            $"No Autodesk Shape Manager {majors} found - geometry nodes will fail. " +
            $"Install a product that ships one (Revit, Civil 3D or FormIt of a matching year), " +
            $"or set {OverrideVariable} to a directory holding ASMAHL*.dll.");
    }

    /// <summary>
    /// An explicitly configured ASM directory, checked rather than trusted.
    ///
    /// ⚠️ EXISTING IS NOT ENOUGH. Dynamo binds ASM to LibG on the MAJOR version, and
    /// <c>GetLibGPreloaderLocation</c> only ever falls back to a LOWER libg folder — so
    /// an ASM whose major no libg here matches cannot load, and reporting the override
    /// as "loaded" because the folder exists would put the original misleading LibG
    /// error back, one indirection further away. Saying which version was found against
    /// which are supported is the whole value of checking.
    /// </summary>
    [SupportedOSPlatform("windows")]
    private static Probe CheckOverride(string directory, List<Version> supported)
    {
        if (!Directory.Exists(directory))
        {
            return new Probe(string.Empty, false,
                $"{OverrideVariable} points at '{directory}', which does not exist. Geometry nodes will fail.");
        }

        Version? found;
        try
        {
            found = Utilities.GetVersionFromPath(directory, "*ASMAHL*.dll");
        }
        catch (Exception exception)
        {
            return new Probe(string.Empty, false,
                $"{OverrideVariable} ('{directory}') could not be read: {exception.Message}");
        }

        if (found is null)
        {
            return new Probe(string.Empty, false,
                $"{OverrideVariable} ('{directory}') holds no ASMAHL*.dll. Geometry nodes will fail.");
        }

        if (!supported.Any(version => version.Major == found.Major))
        {
            var majors = string.Join(" or ", supported.Select(version => version.Major));
            return new Probe(string.Empty, false,
                $"{OverrideVariable} ('{directory}') holds ASM {found}, but this Dynamo runtime can only bind " +
                $"{majors}. Geometry nodes will fail.");
        }

        return new Probe(directory, true, $"Geometry library ASM {found} from {OverrideVariable}.");
    }

    /// <summary>
    /// The ASM majors this runtime can bind, newest first — one per shipped libg folder.
    /// </summary>
    private static List<Version> SupportedVersions(string root)
    {
        var versions = new List<Version>();
        if (!Directory.Exists(root))
        {
            return versions;
        }

        foreach (var directory in Directory.EnumerateDirectories(root, "libg_*"))
        {
            var match = LibGFolder.Match(Path.GetFileName(directory));
            if (match.Success &&
                int.TryParse(match.Groups[1].Value, out var major) &&
                int.TryParse(match.Groups[2].Value, out var minor) &&
                int.TryParse(match.Groups[3].Value, out var build))
            {
                versions.Add(new Version(major, minor, build));
            }
        }

        versions.Sort();
        versions.Reverse();
        return versions;
    }
}
