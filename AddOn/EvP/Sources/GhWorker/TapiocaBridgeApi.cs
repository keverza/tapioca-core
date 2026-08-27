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

        private static uint _epoch;

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

        /// <summary>
        /// Records the live bridge and this worker generation. Called only by
        /// <see cref="Program"/>.
        /// </summary>
        internal static void Bind(BridgeClient bridge, uint epoch)
        {
            _epoch = epoch;
            _bridge = bridge;
        }

        internal static void Unbind()
        {
            _bridge = null;
        }

        /// <summary>
        /// True when Archicad GRANTED preview at the handshake. A capture
        /// component that reads false must not convert geometry at all.
        /// </summary>
        /// <remarks>
        /// ⚠️ OFF HAS TO COST NOTHING, AND THAT IS A RULE ABOUT WHAT IS NEVER
        /// DONE, NOT ABOUT WHAT IS DROPPED. Converting a definition's meshes and
        /// then discarding the batch would put a tessellation on every solve of
        /// every definition for a user who turned preview off.
        /// </remarks>
        public static bool PreviewAvailable
        {
            get
            {
                BridgeClient bridge = _bridge;
                return bridge != null && bridge.IsConnected && bridge.PreviewGranted;
            }
        }

        /// <summary>
        /// This worker generation. It changes on every restart, and the host
        /// drops preview from any other one without complaint -- after a kill and
        /// restart it holds geometry from a process that no longer exists.
        /// </summary>
        public static uint PreviewEpoch
        {
            get { return _epoch; }
        }

        /// <summary>
        /// Sends one framed preview batch to Archicad. Returns an empty string on
        /// success and a reason otherwise; never throws.
        /// </summary>
        /// <remarks>
        /// <para>
        /// ⚠️ THE FRAMES ARE BYTES HERE, AND THAT IS THE POINT OF THE SIGNATURE.
        /// This is the surface <c>Tapioca.Grasshopper.gha</c> binds to BY
        /// REFLECTION, so nothing in it may name a type that lives in either
        /// assembly -- the .gha frames the wire format it also converts to
        /// (PreviewChannel.cs), and this worker owns the transport. One place
        /// knows the layout, and it is the one beside the conversion.
        /// </para>
        /// <para>
        /// ⚠️ A NON-EMPTY RETURN MEANS THE CALLER'S MIRROR IS NOW WRONG. The diff
        /// was computed against it and the batch did not arrive, so the two sides
        /// disagree; the caller must drop its mirror and send a full batch next
        /// rather than another delta.
        /// </para>
        /// </remarks>
        public static string SubmitPreviewBatch(
            uint revision, uint[] messageTypes, byte[][] payloads, byte[] segment, string segmentName)
        {
            BridgeClient bridge = _bridge;
            if (bridge == null)
            {
                return "This Grasshopper is not connected to Archicad.";
            }

            return bridge.SendPreviewBatch(_epoch, revision, messageTypes, payloads, segment, segmentName);
        }

        /// <summary>
        /// Tells Archicad to forget everything this worker previewed, and
        /// releases every segment it has not acknowledged.
        /// </summary>
        public static void SubmitPreviewDropAll(byte[] payload)
        {
            BridgeClient bridge = _bridge;
            if (bridge == null)
            {
                return;
            }

            bridge.SendPreviewDropAll(_epoch, payload);
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
