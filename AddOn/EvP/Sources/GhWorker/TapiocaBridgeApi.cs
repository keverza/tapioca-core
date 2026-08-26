using System;

namespace Tapioca.GhWorker
{
    /// <summary>
    /// The one way a Grasshopper component reaches Archicad from this worker.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ THIS IS THE PUBLIC SURFACE <c>Tapioca.Grasshopper.gha</c> BINDS TO BY
    /// REFLECTION. Changing a signature here is a breaking change for that
    /// package. Everything else in this assembly is internal.
    /// </para>
    /// <para>
    /// ⚠️ THE CALL BLOCKS, AND THAT IS LEGAL NOW. It was not legal in process:
    /// measured 2026-08-26, while a Grasshopper solution held Archicad's main
    /// thread inside an add-on callback, Archicad executed NO JSON command that
    /// needed that thread — not Tapir's, not Tapioca's, not even Graphisoft's own
    /// API.GetSelectedElements. Out here the thread this blocks belongs to the
    /// WORKER; Archicad's main thread is free, answers through MainThreadGate,
    /// and sends the envelope back. The same fact is why the unmodified Tapir
    /// .gha works out of process.
    /// </para>
    /// <para>
    /// ⚠️ CALL IT FROM SolveInstance, NOT FROM A COMPONENT'S OWN WORKER THREAD.
    /// Not for correctness — the add-on marshals to Archicad's main thread
    /// itself — but because a solve that has been cancelled should stop asking,
    /// and only the solve knows.
    /// </para>
    /// </remarks>
    public static class TapiocaBridgeApi
    {
        private static BridgeClient _bridge;

        /// <summary>
        /// True when this worker is connected to an Archicad. A component should
        /// say so on the canvas rather than throw.
        /// </summary>
        public static bool IsAvailable
        {
            get
            {
                BridgeClient bridge = _bridge;
                return bridge != null && bridge.IsConnected;
            }
        }

        /// <summary>Records the live bridge. Called only by <see cref="Program"/>.</summary>
        internal static void Bind(BridgeClient bridge)
        {
            _bridge = bridge;
        }

        internal static void Unbind()
        {
            _bridge = null;
        }

        /// <summary>
        /// Runs one Tapioca command in Archicad and returns its JSON envelope:
        /// <c>{"ok":true,"data":{...}}</c> or <c>{"ok":false,"error":"..."}</c>.
        /// Never throws.
        /// </summary>
        public static string Call(string commandName, string parametersJson)
        {
            BridgeClient bridge = _bridge;
            if (bridge == null)
            {
                return "{\"ok\":false,\"error\":\"This Grasshopper is not connected to Archicad. Open "
                       + "Tapioca > Grasshopper Editor from Archicad's menu.\"}";
            }

            return bridge.Call(commandName, parametersJson);
        }
    }
}
