using System;
using System.Runtime.CompilerServices;

namespace Tapioca.GrasshopperHost
{
    /// <summary>
    /// The only file in this assembly that names a Rhino or Grasshopper type.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Every method is <see cref="MethodImplOptions.NoInlining"/>. That is the
    /// whole contract of this class: the JIT resolves the types a method uses
    /// when it compiles that method, so a Rhino type mentioned in a method that
    /// runs before <see cref="InitializeResolver"/> would be looked up before
    /// anything knows where Rhino is installed. Keeping the mentions here, one
    /// per non-inlinable method, is what guarantees the resolver is always in
    /// place first.
    /// </para>
    /// <para>
    /// RhinoCommon and Grasshopper are COMPILE-time references only (see the
    /// .csproj). At run time these types come from the installed Rhino 8 that
    /// the Rhino.Inside resolver points the loader at.
    /// </para>
    /// </remarks>
    internal static class RhinoBoot
    {
        // Grasshopper's plug-in id, as registered with Rhino. Used only to name
        // it in a diagnostic; the plug-in itself is reached by name below.
        private const string GrasshopperPlugInName = "Grasshopper";

        /// <summary>
        /// Installs the Rhino.Inside assembly resolver and returns the Rhino
        /// system directory it settled on. Throws when no Rhino 8 is found.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static string InitializeResolver(string rhinoSystemDirectory)
        {
            if (string.IsNullOrWhiteSpace(rhinoSystemDirectory))
            {
                RhinoInside.Resolver.Initialize();
            }
            else
            {
                RhinoInside.Resolver.Initialize(rhinoSystemDirectory);
            }

            return RhinoInside.Resolver.RhinoSystemDirectory;
        }

        /// <summary>
        /// Constructs the one RhinoCore, hidden. Returned as
        /// <see cref="IDisposable"/> so the caller never names the type.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static IDisposable CreateHiddenCore()
        {
            // /nosplash and /notemplate: this is an embedded core with no user
            // in front of it, and either dialog would appear over Archicad and
            // block the main thread waiting for a click that no one expects to
            // have to make.
            string[] arguments = new string[] { "/nosplash", "/notemplate" };
            return new Rhino.Runtime.InProcess.RhinoCore(arguments, Rhino.Runtime.InProcess.WindowStyle.Hidden);
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static string DescribeVersion()
        {
            try
            {
                return Rhino.RhinoApp.Version.ToString();
            }
            catch (Exception)
            {
                return "(version unavailable)";
            }
        }

        /// <summary>
        /// Loads stock Grasshopper. The editor window is loaded but NOT shown
        /// unless <paramref name="showEditor"/> is set, which P0 never does.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static bool LoadGrasshopper(bool showEditor, out string failure)
        {
            failure = string.Empty;
            object plugInObject = Rhino.RhinoApp.GetPlugInObject(GrasshopperPlugInName);
            if (plugInObject == null)
            {
                failure = "Rhino did not return the Grasshopper plug-in object. Grasshopper may not be installed "
                          + "with this Rhino, or it failed to load.";
                return false;
            }

            Grasshopper.Plugin.GH_RhinoScriptInterface grasshopper =
                plugInObject as Grasshopper.Plugin.GH_RhinoScriptInterface;
            if (grasshopper == null)
            {
                failure = "The Grasshopper plug-in object was of an unexpected type ("
                          + plugInObject.GetType().FullName + ").";
                return false;
            }

            // LoadEditor returns nothing, so "did it work" has to be asked
            // separately — and asking afterwards is the honest check anyway.
            if (!grasshopper.IsEditorLoaded())
            {
                grasshopper.LoadEditor();
            }

            if (!grasshopper.IsEditorLoaded())
            {
                failure = "Grasshopper's editor would not load.";
                return false;
            }

            if (showEditor)
            {
                grasshopper.ShowEditor();
            }

            return true;
        }
    }
}
