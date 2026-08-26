using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text;

namespace Tapioca.GrasshopperHost
{
    /// <summary>
    /// Slice 1's Tapir arm: find the pinned Tapir package, confirm Grasshopper
    /// actually loaded it, and hand it THIS Archicad's JSON port.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ TAPIR IS LOADED UNCHANGED, AND THAT IS A RULE, NOT A SHORTCUT. The
    /// handoff's first integration rule is that P1 loads the pinned existing
    /// <c>.gha</c> as published and does not fork its component library or
    /// rebuild its API surface. So nothing here compiles, patches or copies
    /// Tapir. It looks for the package, reports what it found, and touches
    /// exactly one public static property on it.
    /// </para>
    /// <para>
    /// That one property is the point. Tapir's <c>ConnectionSettings.Port</c>
    /// defaults to 19723 and the plugin has no Archicad-instance discovery at
    /// all, so with two Archicads open a definition silently drives whichever
    /// one happens to hold the default port — which may not be the model the
    /// user is looking at. Only code inside this process can answer "which
    /// Archicad am I". The native side already asked
    /// <c>ACAPI_Command_GetHttpConnectionPort</c> and passed the answer across;
    /// this is where that answer lands.
    /// </para>
    /// <para>
    /// Reflection rather than a compile-time reference, deliberately: a
    /// reference would pin Tapir into this build and make the add-on fail to
    /// load without it. Every failure here is a diagnostic line, never a failed
    /// start — Grasshopper without Tapir is still a working editor.
    /// </para>
    /// <para>
    /// Like <see cref="RhinoBoot"/>, every method that names a Grasshopper type
    /// is <see cref="MethodImplOptions.NoInlining"/> and is only ever reached
    /// after the Rhino.Inside resolver is installed.
    /// </para>
    /// </remarks>
    internal static class TapirPackage
    {
        /// <summary>
        /// The Tapir release this slice was verified against. A different
        /// version is reported, not refused: pinning here means "say which one
        /// you got", because the compatibility gate is evidence, not a version
        /// string.
        /// </summary>
        internal const string PinnedVersion = "1.5.8";

        private const string PackageName = "Tapir";
        private const string AssemblyFileName = "TapirGrasshopperPlugin.gha";
        private const string ConnectionSettingsTypeName = "TapirGrasshopperPlugin.Types.ArchiCad.ConnectionSettings";
        private const string PortPropertyName = "Port";

        /// <summary>
        /// Points the host at a Tapir <c>.gha</c> outside Rhino's package
        /// manager. Only needed on a machine where Tapir was not installed with
        /// Yak.
        /// </summary>
        private const string OverrideVariable = "TAPIOCA_TAPIR_DIR";

        /// <summary>
        /// Runs BEFORE Grasshopper loads its external files, because adding a
        /// folder afterwards would be too late for this session. Returns a line
        /// worth logging; never throws.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static string Prepare()
        {
            try
            {
                string overrideFolder = Environment.GetEnvironmentVariable(OverrideVariable);
                if (!string.IsNullOrWhiteSpace(overrideFolder))
                {
                    overrideFolder = overrideFolder.Trim();
                    if (!Directory.Exists(overrideFolder))
                    {
                        return "Tapir: " + OverrideVariable + " points at a folder that does not exist ("
                               + overrideFolder + "); ignoring it.";
                    }

                    return AddAssemblyFolder(overrideFolder);
                }

                string packageFolder = FindInstalledPackage();
                if (packageFolder == null)
                {
                    return "Tapir: no installed package found under Rhino's package folders. Install it with "
                           + "Rhino's package manager (\"Tapir\"), or set " + OverrideVariable
                           + " to a folder holding " + AssemblyFileName + ".";
                }

                // Nothing to do: Rhino's package manager loads its own packages,
                // and adding this folder to Grasshopper's assembly folders as
                // well would offer the same .gha twice under one component GUID
                // set. Report it and let the post-load check confirm.
                return "Tapir: package found at " + packageFolder + "; Rhino's package manager loads it.";
            }
            catch (Exception exception)
            {
                return "Tapir: preparation failed: " + Describe(exception);
            }
        }

        /// <summary>
        /// Runs AFTER Grasshopper's editor has loaded. Confirms Tapir is in the
        /// component server and writes the live Archicad port into it. Returns
        /// a multi-line report; never throws.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static string BindPort(uint port)
        {
            try
            {
                Assembly tapir;
                string version;
                string location;
                string found = FindLoadedLibrary(out tapir, out version, out location);
                if (tapir == null)
                {
                    return found;
                }

                StringBuilder report = new StringBuilder();
                report.Append(found);

                if (!string.Equals(version, PinnedVersion, StringComparison.OrdinalIgnoreCase))
                {
                    report.Append(" NOTE: slice 1 was verified against Tapir ")
                          .Append(PinnedVersion)
                          .Append("; this is ")
                          .Append(string.IsNullOrEmpty(version) ? "an unknown version" : version)
                          .Append('.');
                }

                report.Append(' ').Append(WritePort(tapir, port));
                return report.ToString();
            }
            catch (Exception exception)
            {
                return "Tapir: port injection failed: " + Describe(exception);
            }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static string AddAssemblyFolder(string folder)
        {
            List<Grasshopper.Kernel.GH_AssemblyFolderInfo> folders = Grasshopper.Folders.AssemblyFolders;
            if (folders == null)
            {
                return "Tapir: Grasshopper exposed no assembly-folder list, so " + folder + " was not added.";
            }

            foreach (Grasshopper.Kernel.GH_AssemblyFolderInfo existing in folders)
            {
                // GH_AssemblyFolderInfo is a value type, so there is no null to
                // guard against here — only an empty Folder.
                if (!string.IsNullOrEmpty(existing.Folder)
                    && string.Equals(
                        Path.GetFullPath(existing.Folder).TrimEnd('\\'),
                        Path.GetFullPath(folder).TrimEnd('\\'),
                        StringComparison.OrdinalIgnoreCase))
                {
                    return "Tapir: " + folder + " is already one of Grasshopper's assembly folders.";
                }
            }

            folders.Add(
                new Grasshopper.Kernel.GH_AssemblyFolderInfo(
                    folder,
                    Grasshopper.Kernel.GH_PluginFolderType.UserFolder));
            return "Tapir: added " + folder + " to Grasshopper's assembly folders (" + OverrideVariable + ").";
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static string FindLoadedLibrary(out Assembly tapir, out string version, out string location)
        {
            tapir = null;
            version = string.Empty;
            location = string.Empty;

            if (!Grasshopper.Instances.IsComponentServer || Grasshopper.Instances.ComponentServer == null)
            {
                return "Tapir: Grasshopper's component server is not up, so no library list could be read.";
            }

            foreach (Grasshopper.Kernel.GH_AssemblyInfo library in Grasshopper.Instances.ComponentServer.Libraries)
            {
                if (library == null || !IsTapir(library))
                {
                    continue;
                }

                tapir = library.Assembly;
                version = library.Version ?? string.Empty;
                location = library.Location ?? string.Empty;
                if (tapir == null)
                {
                    return "Tapir: Grasshopper lists the library but exposed no assembly for it (" + location + ").";
                }

                return "Tapir " + (string.IsNullOrEmpty(version) ? "(no version)" : version)
                       + " loaded from " + (string.IsNullOrEmpty(location) ? "(unknown path)" : location) + ".";
            }

            return "Tapir: Grasshopper loaded " + LibraryCount()
                   + " libraries and none of them is Tapir." + DescribeLoadingFailures();
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static bool IsTapir(Grasshopper.Kernel.GH_AssemblyInfo library)
        {
            string location = library.Location ?? string.Empty;
            if (string.Equals(Path.GetFileName(location), AssemblyFileName, StringComparison.OrdinalIgnoreCase))
            {
                return true;
            }

            string name = library.Name ?? string.Empty;
            string assemblyName = library.AssemblyName ?? string.Empty;
            return string.Equals(name, PackageName, StringComparison.OrdinalIgnoreCase)
                   || assemblyName.StartsWith("TapirGrasshopperPlugin", StringComparison.OrdinalIgnoreCase);
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static string LibraryCount()
        {
            return Grasshopper.Instances.ComponentServer.Libraries.Count.ToString(CultureInfo.InvariantCulture);
        }

        /// <summary>
        /// Grasshopper records why a <c>.gha</c> would not load, and that record
        /// is the only actionable thing there is when the package is installed
        /// but absent from the ribbon.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining)]
        private static string DescribeLoadingFailures()
        {
            try
            {
                List<Grasshopper.Kernel.GH_LoadingException> failures =
                    Grasshopper.Instances.ComponentServer.LoadingExceptions;
                if (failures == null || failures.Count == 0)
                {
                    return " Grasshopper reported no loading errors, so it never saw the file.";
                }

                StringBuilder text = new StringBuilder(" Grasshopper reported ");
                text.Append(failures.Count.ToString(CultureInfo.InvariantCulture)).Append(" loading error(s):");
                foreach (Grasshopper.Kernel.GH_LoadingException failure in failures)
                {
                    if (failure == null)
                    {
                        continue;
                    }

                    text.Append(" [").Append(failure.Name).Append(": ").Append(failure.Message).Append(']');
                }

                return text.ToString();
            }
            catch (Exception exception)
            {
                return " Grasshopper's loading errors could not be read: " + Describe(exception);
            }
        }

        private static string WritePort(Assembly tapir, uint port)
        {
            if (port == 0)
            {
                return "Archicad's JSON port is unknown, so Tapir keeps its own default (19723). "
                       + "Set the port by hand on ConnectArchicad if more than one Archicad is running.";
            }

            Type settings = tapir.GetType(ConnectionSettingsTypeName, false, false);
            if (settings == null)
            {
                return "Tapir does not expose " + ConnectionSettingsTypeName
                       + " in this version, so the port could not be injected; set it by hand on ConnectArchicad ("
                       + port.ToString(CultureInfo.InvariantCulture) + ").";
            }

            PropertyInfo property = settings.GetProperty(
                PortPropertyName,
                BindingFlags.Public | BindingFlags.Static);
            if (property == null || !property.CanWrite || property.PropertyType != typeof(int))
            {
                return "Tapir's " + PortPropertyName
                       + " is not a writable static int in this version, so the port could not be injected; "
                       + "set it by hand on ConnectArchicad (" + port.ToString(CultureInfo.InvariantCulture) + ").";
            }

            property.SetValue(null, (int)port, null);

            // Read back rather than assume: this is another project's internals,
            // and "we set it" is worth nothing next to "it holds that value".
            object readBack = property.GetValue(null, null);
            int actual = readBack is int ? (int)readBack : -1;
            if (actual != (int)port)
            {
                return "Tapir's connection port did not take the injected value (asked for "
                       + port.ToString(CultureInfo.InvariantCulture) + ", reads "
                       + actual.ToString(CultureInfo.InvariantCulture) + ").";
            }

            return "Tapir's default connection port is now " + port.ToString(CultureInfo.InvariantCulture)
                   + ", this Archicad's own.";
        }

        /// <summary>
        /// Looks for a Yak-installed Tapir, preferring the pinned version and
        /// otherwise taking the highest one present.
        /// </summary>
        private static string FindInstalledPackage()
        {
            string best = null;
            Version bestVersion = null;

            foreach (string root in PackageRoots())
            {
                string packageRoot = Path.Combine(root, PackageName);
                if (!Directory.Exists(packageRoot))
                {
                    continue;
                }

                string pinned = Path.Combine(packageRoot, PinnedVersion);
                if (File.Exists(Path.Combine(pinned, AssemblyFileName)))
                {
                    return pinned;
                }

                foreach (string candidate in Directory.GetDirectories(packageRoot))
                {
                    if (!File.Exists(Path.Combine(candidate, AssemblyFileName)))
                    {
                        continue;
                    }

                    Version version;
                    if (!Version.TryParse(Path.GetFileName(candidate), out version))
                    {
                        version = new Version(0, 0);
                    }

                    if (bestVersion == null || version > bestVersion)
                    {
                        bestVersion = version;
                        best = candidate;
                    }
                }
            }

            return best;
        }

        private static IEnumerable<string> PackageRoots()
        {
            // Rhino 8's package folders, per-user first: that is where the
            // package manager installs by default and where a machine-wide copy
            // would be shadowed from.
            string appData = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
            if (!string.IsNullOrEmpty(appData))
            {
                yield return Path.Combine(appData, "McNeel", "Rhinoceros", "packages", "8.0");
            }

            string programData = Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData);
            if (!string.IsNullOrEmpty(programData))
            {
                yield return Path.Combine(programData, "McNeel", "Rhinoceros", "packages", "8.0");
            }
        }

        private static string Describe(Exception exception)
        {
            string text = exception.GetType().Name + ": " + exception.Message;
            Exception inner = exception.InnerException;
            while (inner != null)
            {
                text += " -> " + inner.GetType().Name + ": " + inner.Message;
                inner = inner.InnerException;
            }

            return text;
        }
    }
}
