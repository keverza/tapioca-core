using System;

namespace Tapioca.GhWorker
{
    /// <summary>
    /// The one thread Grasshopper may be touched from, whoever owns it.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ THE SESSION HALF DOES NOT OWN A THREAD, AND UNTIL THIS INTERFACE IT
    /// BELIEVED IT DID. <see cref="SessionRouter"/> took a
    /// <see cref="GhEngineThread"/>, which creates an STA thread and runs a
    /// WinForms loop on it — correct in Tapioca.GhWorker.exe, where that thread
    /// is the whole reason the process exists, and impossible in a Grasshopper
    /// that is ALREADY RUNNING inside somebody's Rhino. There the UI thread
    /// predates us, belongs to Rhino, and is the only thread that may touch a
    /// document; all a peer may do is marshal onto it.
    /// </para>
    /// <para>
    /// So the router asks for the capability it actually uses — "run this where
    /// Grasshopper lives" — and the two hosts answer differently: the worker
    /// with a thread it made and will end, the attached peer with
    /// <c>RhinoApp.InvokeOnUiThread</c> onto a thread it must never end.
    /// </para>
    /// <para>
    /// Deliberately ONE method. Starting, stopping and joining a thread are the
    /// worker's business and are exactly the operations that have no meaning in
    /// a peer; leaving them off the interface is what keeps a peer from being
    /// asked to shut down its host's Rhino.
    /// </para>
    /// </remarks>
    internal interface IEngineDispatcher
    {
        /// <summary>
        /// Queues work for the Grasshopper thread. False when it will not run —
        /// the engine is stopping, or the marshalling target has gone — which the
        /// caller reports rather than throwing, because the caller is a bridge
        /// reader thread serving a request from Archicad.
        /// </summary>
        bool Post(Action action);
    }
}
