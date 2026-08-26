using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;

namespace Tapioca.GhWorker
{
    /// <summary>
    /// Makes Grasshopper load Tapioca's own component package.
    /// </summary>
    /// <remarks>
    /// <para>
    /// The package is BUILT beside the add-on and INSTALLED into Grasshopper's
    /// Libraries folder on every start. Building it beside the add-on is what
    /// keeps it versioned with the ABI it calls through; copying it every start
    /// is what stops an old copy in the Libraries folder from outliving the
    /// bridge it was built against. See <see cref="Prepare"/> for why the copy
    /// is necessary at all.
    /// </para>
    /// <para>
    /// ⚠️ THE INSTALL MUST HAPPEN BEFORE THE EDITOR LOADS. Grasshopper scans for
    /// packages exactly once, while the editor comes up. A file copied in
    /// afterwards is a file nothing reads until the next Archicad.
    /// </para>
    /// <para>
    /// Like <see cref="RhinoBoot"/> and <see cref="TapirPackage"/>, every method
    /// that names a Grasshopper type is non-inlinable and is only ever reached
    /// after the Rhino.Inside resolver is installed.
    /// </para>
    /// </remarks>
    internal static class TapiocaPackage
    {
        private const string PackageFileName = "Tapioca.Grasshopper.gha";

        /// <summary>Where the build puts the package, beside the add-on.</summary>
        private const string FolderName = "GrasshopperLibraries";

        /// <summary>
        /// Installs the package into Grasshopper's own Libraries folder, so that
        /// Grasshopper finds it by the route it already uses. Returns a line
        /// worth logging; never throws.
        /// </summary>
        /// <remarks>
        /// <para>
        /// ⚠️ ADDING TO <c>Grasshopper.Folders.AssemblyFolders</c> DOES NOT WORK,
        /// AND THAT WAS TRIED FIRST. The folder was added before the editor
        /// loaded — the one moment Grasshopper reads that list — and the package
        /// still did not appear; it only showed up once the <c>.gha</c> was
        /// copied into the Libraries folder by hand. Whatever that list is, it is
        /// not a live input to the scan. Do not reinstate that approach without
        /// evidence that it loads something.
        /// </para>
        /// <para>
        /// So the package is COPIED into <c>Folders.DefaultAssemblyFolder</c>
        /// instead, on every start, whenever the shipped file differs from the
        /// installed one. Copying every start is what keeps the version lock that
        /// shipping beside the .apx was meant to give: the package calls through
        /// a versioned ABI, and an old copy left in the Libraries folder after an
        /// upgrade would be a stale package talking to a newer bridge. Overwriting
        /// from the shipped file makes that impossible.
        /// </para>
        /// <para>
        /// The copy is left behind if Tapioca is uninstalled, and it will load
        /// into a standalone Rhino too. Neither is harmful: with no add-on in the
        /// process the components report that the bridge is unavailable, which is
        /// the message they exist to give.
        /// </para>
        /// </remarks>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static string Prepare()
        {
            try
            {
                string shipped = ResolveShippedPackage();
                if (shipped == null)
                {
                    return "Tapioca package: " + PackageFileName + " was not found beside the add-on, so "
                           + "Tapioca's own Grasshopper components will not be available. Rebuild the add-on.";
                }

                string libraries = Grasshopper.Folders.DefaultAssemblyFolder;
                if (string.IsNullOrWhiteSpace(libraries))
                {
                    return "Tapioca package: Grasshopper reported no Libraries folder, so " + PackageFileName
                           + " could not be installed.";
                }

                Directory.CreateDirectory(libraries);
                string installed = Path.Combine(libraries, PackageFileName);

                if (IsSameFile(shipped, installed))
                {
                    return "Tapioca package: " + installed + " is already current.";
                }

                File.Copy(shipped, installed, true);
                return "Tapioca package: installed " + installed + " from " + shipped + ".";
            }
            catch (IOException exception)
            {
                // Almost always the file being locked by a Grasshopper that
                // already loaded it. Worth saying plainly, because the fix is a
                // restart and nothing else.
                return "Tapioca package: could not update " + PackageFileName
                       + " (" + exception.Message + "). If Grasshopper has already loaded it in this Archicad "
                       + "session, restart Archicad to pick up the new one.";
            }
            catch (Exception exception)
            {
                return "Tapioca package: preparation failed: " + exception.GetType().Name + ": " + exception.Message;
            }
        }

        /// <summary>
        /// Same length and same write time. Enough: the shipped file is only ever
        /// replaced by a build, which moves both.
        /// </summary>
        private static bool IsSameFile(string shipped, string installed)
        {
            if (!File.Exists(installed))
            {
                return false;
            }

            FileInfo source = new FileInfo(shipped);
            FileInfo target = new FileInfo(installed);
            return source.Length == target.Length
                && source.LastWriteTimeUtc == target.LastWriteTimeUtc;
        }

        /// <summary>
        /// Confirms after the editor loaded that Grasshopper really took the
        /// package. Returns a line worth logging; never throws.
        /// </summary>
        /// <remarks>
        /// Asked rather than assumed, for the same reason the Tapir check exists:
        /// "we added the folder" and "the components are on the ribbon" are
        /// different claims, and only the second one is the user's experience.
        /// </remarks>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static string Verify()
        {
            try
            {
                if (!Grasshopper.Instances.IsComponentServer || Grasshopper.Instances.ComponentServer == null)
                {
                    return "Tapioca package: Grasshopper's component server is not up, so it could not be checked.";
                }

                foreach (Grasshopper.Kernel.GH_AssemblyInfo library in Grasshopper.Instances.ComponentServer.Libraries)
                {
                    if (library == null)
                    {
                        continue;
                    }

                    string location = library.Location ?? string.Empty;
                    if (string.Equals(
                            Path.GetFileName(location),
                            PackageFileName,
                            StringComparison.OrdinalIgnoreCase))
                    {
                        return "Tapioca package loaded from " + location + ".";
                    }
                }

                return "Tapioca package: Grasshopper did not load " + PackageFileName
                       + "; its components will be missing from the ribbon.";
            }
            catch (Exception exception)
            {
                return "Tapioca package: verification failed: " + exception.GetType().Name + ": "
                       + exception.Message;
            }
        }

        /// <summary>
        /// The package as built and shipped beside the add-on, or null when it
        /// is not there.
        /// </summary>
        private static string ResolveShippedPackage()
        {
            string host = typeof(TapiocaPackage).Assembly.Location;
            if (string.IsNullOrEmpty(host))
            {
                return null;
            }

            string beside = Path.GetDirectoryName(host);
            if (string.IsNullOrEmpty(beside))
            {
                return null;
            }

            string shipped = Path.Combine(beside, FolderName, PackageFileName);
            return File.Exists(shipped) ? shipped : null;
        }
    }
}
