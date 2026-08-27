using System;
using System.Reflection;
using System.Threading;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// Finds the Tapioca worker's preview surface in this process and sends
    /// batches through it.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ REFLECTION, NOT A REFERENCE, FOR EXACTLY THE REASON <see cref="TapiocaBridge"/>
    /// GIVES. This <c>.gha</c> is loaded by Grasshopper from its Libraries
    /// folder; the worker host assembly is the process's own entry assembly. A
    /// compile-time reference would let Grasshopper's loader resolve a SECOND
    /// copy of the host from disk — a fresh set of statics whose bridge was never
    /// bound — and every preview would report "not connected" on a machine where
    /// it plainly is.
    /// </para>
    /// <para>
    /// ⚠️ EVERYTHING THAT CROSSES IS PRIMITIVE. Bytes, arrays of bytes, uints and
    /// strings — no type declared in either assembly. The wire format itself
    /// lives here, in <see cref="PreviewChannel"/>, beside the conversion that
    /// produces it; the worker relays frames and owns the transport, and nothing
    /// on that side has to be recompiled when a primitive kind gains a field.
    /// </para>
    /// <para>
    /// ⚠️ RESOLVED LAZILY. Grasshopper loads this package while the editor is
    /// coming up, which can be before the bridge has finished connecting; a
    /// failure cached at that moment would never recover.
    /// </para>
    /// </remarks>
    internal static class TapiocaPreviewBridge
    {
        private const string HostAssembly = "Tapioca.GhWorker";
        private const string HostType = "Tapioca.GhWorker.TapiocaBridgeApi";

        private static MethodInfo _submitBatch;
        private static MethodInfo _submitDropAll;
        private static PropertyInfo _available;
        private static PropertyInfo _epoch;
        private static bool _resolved;

        /// <summary>
        /// True when Archicad granted preview at the handshake. False when the
        /// bridge is absent, not connected, or preview is switched off in the
        /// add-on.
        /// </summary>
        /// <remarks>
        /// ⚠️ ASK BEFORE CONVERTING, NOT AFTER. A component that converted its
        /// geometry and then found this false would have paid a tessellation per
        /// solve for a user who turned preview off, which is precisely the cost
        /// the capability gate exists to avoid.
        /// </remarks>
        internal static bool Available
        {
            get
            {
                if (!Resolve() || _available == null)
                {
                    return false;
                }

                try
                {
                    return (bool)_available.GetValue(null, null);
                }
                catch (Exception)
                {
                    return false;
                }
            }
        }

        /// <summary>
        /// The worker generation this preview belongs to. Zero when the bridge
        /// could not be reached.
        /// </summary>
        internal static uint Epoch
        {
            get
            {
                if (!Resolve() || _epoch == null)
                {
                    return 0;
                }

                try
                {
                    return (uint)_epoch.GetValue(null, null);
                }
                catch (Exception)
                {
                    return 0;
                }
            }
        }

        /// <summary>
        /// Sends one framed batch. Returns an empty string on success and a
        /// reason otherwise. Never throws: the caller is a solve, and a preview
        /// that could not be sent must show a message on the canvas rather than
        /// abort the definition.
        /// </summary>
        internal static string Send(PreviewChannel.PreviewWireBatch wire)
        {
            if (wire == null || wire.Frames.Count == 0)
            {
                return string.Empty;
            }

            if (!Resolve() || _submitBatch == null)
            {
                return "Tapioca's Archicad bridge was not found in this process. Grasshopper must be opened from "
                       + "Archicad with Tapioca > Grasshopper Editor.";
            }

            uint[] types = new uint[wire.Frames.Count];
            byte[][] payloads = new byte[wire.Frames.Count][];
            for (int index = 0; index < wire.Frames.Count; index++)
            {
                types[index] = (uint)wire.Frames[index].Message;
                payloads[index] = wire.Frames[index].Payload;
            }

            try
            {
                object result = _submitBatch.Invoke(
                    null,
                    new object[] { wire.Revision, types, payloads, wire.Segment, wire.SegmentName });
                return result as string ?? string.Empty;
            }
            catch (TargetInvocationException exception)
            {
                Exception inner = exception.InnerException ?? exception;
                return inner.GetType().Name + ": " + inner.Message;
            }
            catch (Exception exception)
            {
                return exception.GetType().Name + ": " + exception.Message;
            }
        }

        /// <summary>
        /// Tells Archicad to forget everything this worker previewed. Sent when a
        /// component leaves the document and after a failed send, since both
        /// leave the two mirrors disagreeing.
        /// </summary>
        internal static void DropAll(string reason)
        {
            if (!Resolve() || _submitDropAll == null)
            {
                return;
            }

            try
            {
                _submitDropAll.Invoke(null, new object[] { PreviewChannel.EncodeDropAll(Epoch, reason) });
            }
            catch (Exception)
            {
                // A drop that cannot be sent must never be the reason a solve or
                // a document close fails. The host drops everything on
                // disconnect anyway.
            }
        }

        private static bool Resolve()
        {
            if (Volatile.Read(ref _resolved))
            {
                return _submitBatch != null;
            }

            try
            {
                Assembly[] loaded = AppDomain.CurrentDomain.GetAssemblies();
                for (int index = 0; index < loaded.Length; index++)
                {
                    AssemblyName name = loaded[index].GetName();
                    if (!string.Equals(name.Name, HostAssembly, StringComparison.OrdinalIgnoreCase))
                    {
                        continue;
                    }

                    Type type = loaded[index].GetType(HostType, false, false);
                    if (type == null)
                    {
                        continue;
                    }

                    MethodInfo submit = type.GetMethod(
                        "SubmitPreviewBatch",
                        BindingFlags.Public | BindingFlags.Static,
                        null,
                        new Type[] { typeof(uint), typeof(uint[]), typeof(byte[][]), typeof(byte[]), typeof(string) },
                        null);
                    if (submit == null)
                    {
                        continue;
                    }

                    _submitBatch = submit;
                    _submitDropAll = type.GetMethod(
                        "SubmitPreviewDropAll",
                        BindingFlags.Public | BindingFlags.Static,
                        null,
                        new Type[] { typeof(byte[]) },
                        null);
                    _available = type.GetProperty("PreviewAvailable", BindingFlags.Public | BindingFlags.Static);
                    _epoch = type.GetProperty("PreviewEpoch", BindingFlags.Public | BindingFlags.Static);
                    // ⚠️ MARKED RESOLVED ONLY ONCE THE SURFACE WAS ACTUALLY
                    // FOUND. Caching a failure would make a package that loaded
                    // fractionally before the bridge connected stay broken for
                    // the whole session.
                    Volatile.Write(ref _resolved, true);
                    return true;
                }
            }
            catch (Exception)
            {
                // Treated as "not found": a component must degrade to a message
                // on the canvas, never to an exception during a solve.
            }

            return false;
        }
    }
}
