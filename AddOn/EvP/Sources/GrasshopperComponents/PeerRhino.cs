using System;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// The three things the peer asks of the Rhino it is running inside.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Kept together so that what a peer touches of its host is one short file
    /// and can be read in one sitting: the UI thread, the version, and bringing
    /// the canvas to the front. Nothing else.
    /// </para>
    /// <para>
    /// ⚠️ WHAT IS ABSENT IS THE CONTRACT. There is no start, no exit, no
    /// RhinoCore and no document mutation here. A peer's Rhino was running before
    /// Archicad asked for it and must be running after Archicad is finished with
    /// it; every operation that could break that promise is deliberately not
    /// available to this code.
    /// </para>
    /// </remarks>
    internal static class PeerRhino
    {
        /// <summary>
        /// Runs an action on the thread that owns Rhino's documents.
        /// </summary>
        /// <remarks>
        /// Rhino executes it directly when already on that thread, so a session
        /// request that arrives during a solve is not deadlocked by this.
        /// </remarks>
        internal static void InvokeOnUiThread(Action action)
        {
            Rhino.RhinoApp.InvokeOnUiThread(action);
        }

        internal static string Version()
        {
            return Rhino.RhinoApp.Version.ToString();
        }

        /// <summary>
        /// Brings the canvas the user already has to the front. Never loads one.
        /// </summary>
        /// <remarks>
        /// ⚠️ NOT <c>LoadEditor</c>. In the worker, "show the editor" may have to
        /// create the window; here a Grasshopper is running because a person
        /// opened it, and loading an editor into their Rhino on a request from
        /// another application's panel is a window they did not ask for. If no
        /// canvas is open the request is a no-op, which is the honest answer.
        /// </remarks>
        internal static void FocusEditor()
        {
            System.Windows.Forms.Form editor = global::Grasshopper.Instances.DocumentEditor;
            if (editor == null || editor.IsDisposed)
            {
                return;
            }

            if (!editor.Visible)
            {
                editor.Show();
            }

            editor.BringToFront();
        }
    }
}
