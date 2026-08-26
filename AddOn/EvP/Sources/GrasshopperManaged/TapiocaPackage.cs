using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;

namespace Tapioca.GrasshopperHost
{
    /// <summary>
    /// Makes Grasshopper load Tapioca's own component package.
    /// </summary>
    /// <remarks>
    /// <para>
    /// The package ships beside the add-on rather than being installed into the
    /// user's Grasshopper Libraries folder, and that is deliberate: it is built
    /// from this repository and versioned with the ABI it calls through, so a
    /// copy left behind in a user folder after an upgrade would be a stale
    /// package talking to a newer bridge. Shipping it beside the <c>.apx</c>
    /// makes the two impossible to separate.
    /// </para>
    /// <para>
    /// ⚠️ THE FOLDER MUST BE ADDED BEFORE THE EDITOR LOADS. Grasshopper scans its
    /// assembly folders exactly once, while the editor comes up. A folder added
    /// afterwards is a folder nothing reads until the next Archicad.
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

        /// <summary>Beside the add-on, in its own folder so the scan is cheap.</summary>
        private const string FolderName = "GrasshopperLibraries";

        /// <summary>
        /// Adds the package folder to Grasshopper's assembly folders. Returns a
        /// line worth logging; never throws.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static string Prepare()
        {
            try
            {
                string folder = ResolveFolder();
                if (folder == null)
                {
                    return "Tapioca package: " + PackageFileName + " was not found beside the add-on, so "
                           + "Tapioca's own Grasshopper components will not be available. Rebuild the add-on.";
                }

                List<Grasshopper.Kernel.GH_AssemblyFolderInfo> folders = Grasshopper.Folders.AssemblyFolders;
                if (folders == null)
                {
                    return "Tapioca package: Grasshopper exposed no assembly-folder list, so " + folder
                           + " was not added.";
                }

                string wanted = Path.GetFullPath(folder).TrimEnd('\\');
                foreach (Grasshopper.Kernel.GH_AssemblyFolderInfo existing in folders)
                {
                    if (!string.IsNullOrEmpty(existing.Folder)
                        && string.Equals(
                            Path.GetFullPath(existing.Folder).TrimEnd('\\'),
                            wanted,
                            StringComparison.OrdinalIgnoreCase))
                    {
                        return "Tapioca package: " + folder + " is already one of Grasshopper's assembly folders.";
                    }
                }

                folders.Add(
                    new Grasshopper.Kernel.GH_AssemblyFolderInfo(
                        folder,
                        Grasshopper.Kernel.GH_PluginFolderType.UserFolder));
                return "Tapioca package: added " + folder + " to Grasshopper's assembly folders.";
            }
            catch (Exception exception)
            {
                return "Tapioca package: preparation failed: " + exception.GetType().Name + ": " + exception.Message;
            }
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
        /// The package folder beside this assembly, or null when the package is
        /// not there.
        /// </summary>
        private static string ResolveFolder()
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

            string folder = Path.Combine(beside, FolderName);
            return File.Exists(Path.Combine(folder, PackageFileName)) ? folder : null;
        }
    }
}
