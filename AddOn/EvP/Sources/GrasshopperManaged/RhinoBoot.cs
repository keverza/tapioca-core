using System;
using System.Runtime.CompilerServices;

namespace Tapioca.GrasshopperHost
{
    /// <summary>
    /// Where Rhino and Grasshopper types are named, and when.
    /// </summary>
    /// <remarks>
    /// <para>
    /// This file and <see cref="TapirPackage"/> are the only two that name a
    /// Rhino or Grasshopper type, and both obey the same rule for the same
    /// reason. Nothing else in the assembly may name one.
    /// </para>
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
        /// Loads stock Grasshopper, then the pinned Tapir package and this
        /// Archicad's JSON port. The editor window is loaded but NOT shown
        /// unless <paramref name="showEditor"/> is set, which P0 never does.
        /// </summary>
        /// <remarks>
        /// The Tapir steps bracket the editor load rather than following it,
        /// and the order is not arbitrary: Grasshopper scans its assembly
        /// folders exactly once, while the editor loads, so a folder added
        /// afterwards is a folder that will not be read until the next
        /// Archicad. Preparation therefore goes first and verification second,
        /// with the load between them.
        /// </remarks>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static bool LoadGrasshopper(
            bool showEditor,
            uint archicadJsonPort,
            out string tapirReport,
            out string failure)
        {
            failure = string.Empty;
            tapirReport = string.Empty;
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

            string prepared = TapirPackage.Prepare();

            // LoadEditor returns nothing, so "did it work" has to be asked
            // separately — and asking afterwards is the honest check anyway.
            if (!grasshopper.IsEditorLoaded())
            {
                grasshopper.LoadEditor();
            }

            if (!grasshopper.IsEditorLoaded())
            {
                tapirReport = prepared;
                failure = "Grasshopper's editor would not load.";
                return false;
            }

            tapirReport = prepared + " " + TapirPackage.BindPort(archicadJsonPort);

            // Fire and forget, off the main thread, for the reason spelled out
            // in TapirConnectionCheck: it makes the same loopback call a Tapir
            // component makes, and making it from HERE would be waiting on a
            // reply this thread has to be free to produce.
            TapirConnectionCheck.Begin(archicadJsonPort);

            if (showEditor)
            {
                ShowAndGate(grasshopper);
            }

            return true;
        }

        /// <summary>
        /// Shows the Grasshopper canvas. Idempotent; loads the editor first if
        /// something has not already.
        /// </summary>
        /// <remarks>
        /// ⚠️ THIS MUST NEVER TOUCH THE CORE. The editor is a window over a
        /// runtime that outlives it: Editor and Player share one RhinoCore, so
        /// showing a canvas cannot construct anything and hiding one cannot
        /// dispose anything. If this method ever grows a RhinoCore reference,
        /// the shared-runtime guarantee is gone and closing the canvas starts
        /// taking the Player down with it.
        /// </remarks>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static bool SetEditorVisible(bool visible, out string failure)
        {
            failure = string.Empty;
            Grasshopper.Plugin.GH_RhinoScriptInterface grasshopper =
                Rhino.RhinoApp.GetPlugInObject(GrasshopperPlugInName)
                    as Grasshopper.Plugin.GH_RhinoScriptInterface;
            if (grasshopper == null)
            {
                failure = "Grasshopper is not available in this Rhino core.";
                return false;
            }

            if (visible)
            {
                if (!grasshopper.IsEditorLoaded())
                {
                    grasshopper.LoadEditor();
                }

                if (!grasshopper.IsEditorLoaded())
                {
                    failure = "Grasshopper's editor would not load.";
                    return false;
                }

                ShowAndGate(grasshopper);
            }
            else
            {
                // Hiding an editor that was never loaded is a no-op, not a
                // failure: the caller's intent (no canvas on screen) holds.
                if (grasshopper.IsEditorLoaded())
                {
                    grasshopper.HideEditor();
                }

                // The gate goes with the canvas. While no Grasshopper window is
                // on screen there is nothing to protect, and a hook that is not
                // installed cannot get anything wrong.
                Log.Write(EditorInput.Uninstall());
            }

            return true;
        }

        /// <summary>
        /// Shows the canvas and arms the keyboard gate around it.
        /// </summary>
        /// <remarks>
        /// The two belong together. A Grasshopper canvas that is visible but
        /// ungated is the state the user actually met: the editor opens, the
        /// canvas takes focus, and Delete, Escape and the rest still go to
        /// Archicad because Archicad's message loop translates its own
        /// accelerators before anything else sees the keystroke. See
        /// <see cref="EditorInput"/>.
        /// </remarks>
        [MethodImpl(MethodImplOptions.NoInlining)]
        private static void ShowAndGate(Grasshopper.Plugin.GH_RhinoScriptInterface grasshopper)
        {
            grasshopper.ShowEditor();

            // AFTER the show, not before: the editor form's handle does not
            // exist until it has been created, and gating a handle of zero
            // gates nothing.
            EditorInput.SetGatedRoots(GatedWindowHandles());
            Log.Write(EditorInput.Install());
        }

        /// <summary>
        /// The top-level windows whose keystrokes belong to Grasshopper rather
        /// than to Archicad.
        /// </summary>
        /// <remarks>
        /// Rhino's main window is in the list even though it is hidden: it owns
        /// Grasshopper's dialogs, so it is the window the owner chain of a
        /// Grasshopper modal ends at.
        /// </remarks>
        [MethodImpl(MethodImplOptions.NoInlining)]
        private static IntPtr[] GatedWindowHandles()
        {
            System.Collections.Generic.List<IntPtr> handles = new System.Collections.Generic.List<IntPtr>();
            try
            {
                if (Grasshopper.Instances.DocumentEditor != null)
                {
                    handles.Add(Grasshopper.Instances.DocumentEditor.Handle);
                }
            }
            catch (Exception exception)
            {
                Log.Write("Grasshopper's editor window handle was unavailable: " + exception.Message);
            }

            try
            {
                handles.Add(Rhino.RhinoApp.MainWindowHandle());
            }
            catch (Exception exception)
            {
                Log.Write("Rhino's main window handle was unavailable: " + exception.Message);
            }

            return handles.ToArray();
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static bool IsEditorVisible()
        {
            try
            {
                Grasshopper.Plugin.GH_RhinoScriptInterface grasshopper =
                    Rhino.RhinoApp.GetPlugInObject(GrasshopperPlugInName)
                        as Grasshopper.Plugin.GH_RhinoScriptInterface;
                return grasshopper != null && grasshopper.IsEditorLoaded() && grasshopper.IsEditorVisible();
            }
            catch (Exception)
            {
                return false;
            }
        }
    }
}
