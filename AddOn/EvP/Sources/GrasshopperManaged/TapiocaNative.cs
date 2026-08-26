using System;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Threading;

namespace Tapioca.GrasshopperHost
{
    /// <summary>
    /// The one way a Grasshopper component reaches Archicad in this process.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ DO NOT REPLACE THIS WITH AN HTTP CALL. Measured 2026-08-26 from inside
    /// a live Grasshopper solution: while a solve holds Archicad's main thread
    /// inside an add-on callback, Archicad executes NO JSON command that needs
    /// that thread — not Tapir's, not Tapioca's, not even Graphisoft's own
    /// <c>API.GetSelectedElements</c> — and not from a background thread either.
    /// Only <c>API.IsAlive</c> survives, because Archicad answers it without
    /// scheduling anything. Pumping messages does not help: a callback cannot
    /// return to the loop it is standing on. An in-process component therefore
    /// cannot reach Archicad over a socket, at all, ever.
    /// </para>
    /// <para>
    /// The same fact read the other way is why this works. A component solves ON
    /// Archicad's main thread, which is the thread ACAPI demands, so the call
    /// runs inline and returns in the same stack frame — no socket, no queue, no
    /// timeout, and none of the per-call dispatch cost the JSON port charges.
    /// </para>
    /// <para>
    /// This is the PUBLIC surface the Tapioca Grasshopper package binds to.
    /// Everything else in this assembly is internal; changing a signature here
    /// is a breaking change for that package.
    /// </para>
    /// </remarks>
    public static class TapiocaNative
    {
        private delegate int CallNativeDelegate(
            [MarshalAs(UnmanagedType.LPWStr)] string commandName,
            [MarshalAs(UnmanagedType.LPWStr)] string parametersJson,
            IntPtr buffer,
            int capacityChars,
            out int neededChars);

        // Mirrors TapiocaGhStatus in GrasshopperHostApi.h. Only the codes this
        // path can produce are named.
        private const int StatusOk = 0;
        private const int StatusNotRunning = 4;
        private const int StatusWrongThread = 6;
        private const int StatusBufferTooSmall = 15;
        private const int StatusUnknownCommand = 16;
        private const int StatusBadRequest = 17;
        private const int StatusCommandFailed = 18;
        private const int StatusWriteRefused = 19;

        // Most answers are small; this avoids a two-pass call for all of them
        // while still handling a large one correctly.
        private const int InitialBufferChars = 8192;

        private static CallNativeDelegate _call;

        /// <summary>
        /// True when the add-on has handed over its native entry point and has
        /// not revoked it. A component should say so rather than throw.
        /// </summary>
        public static bool IsAvailable
        {
            get { return Volatile.Read(ref _call) != null; }
        }

        /// <summary>Mirrors the native TapiocaGhNativeApi struct.</summary>
        [StructLayout(LayoutKind.Sequential)]
        private struct NativeApi
        {
            public uint StructSize;
            public uint AbiVersion;
            public IntPtr CallNative;
        }

        /// <summary>
        /// Records the native table handed over at start. Called only by
        /// <see cref="Bootstrap"/>.
        /// </summary>
        internal static void Bind(IntPtr nativeApi)
        {
            if (nativeApi == IntPtr.Zero)
            {
                Volatile.Write(ref _call, null);
                return;
            }

            NativeApi api = Marshal.PtrToStructure<NativeApi>(nativeApi);
            if (api.CallNative == IntPtr.Zero)
            {
                Volatile.Write(ref _call, null);
                return;
            }

            Volatile.Write(
                ref _call,
                Marshal.GetDelegateForFunctionPointer<CallNativeDelegate>(api.CallNative));
        }

        /// <summary>Drops the native pointer. Called on the stop path.</summary>
        internal static void Unbind()
        {
            Volatile.Write(ref _call, null);
        }

        /// <summary>
        /// Runs one Tapioca native command and returns its JSON envelope:
        /// <c>{"ok":true,"data":{...}}</c> or <c>{"ok":false,"error":"..."}</c>.
        /// </summary>
        /// <remarks>
        /// ⚠️ MAIN THREAD ONLY. Call it from a component's SolveInstance, which
        /// already runs there. The native side checks and refuses rather than
        /// trusting the caller, because ACAPI off the main thread corrupts
        /// Archicad instead of failing.
        /// </remarks>
        public static string Call(string commandName, string parametersJson)
        {
            CallNativeDelegate call = Volatile.Read(ref _call);
            if (call == null)
            {
                return Error("Tapioca's native bridge is not available. Open Tapioca > Grasshopper Editor "
                             + "from Archicad's menu; a Grasshopper started any other way has no add-on to talk to.");
            }

            if (string.IsNullOrWhiteSpace(commandName))
            {
                return Error("No command name was given.");
            }

            try
            {
                int needed;
                string response = Invoke(call, commandName, parametersJson, InitialBufferChars, out needed);
                if (response != null)
                {
                    return response;
                }

                // Only reached when the first buffer was too small; `needed` is
                // now exact, so the second attempt cannot fail for size.
                response = Invoke(call, commandName, parametersJson, needed + 1, out needed);
                return response ?? Error("The response did not fit a buffer sized to its own reported length.");
            }
            catch (Exception exception)
            {
                return Error(exception.GetType().Name + ": " + exception.Message);
            }
        }

        private static string Invoke(
            CallNativeDelegate call,
            string commandName,
            string parametersJson,
            int capacityChars,
            out int neededChars)
        {
            IntPtr buffer = Marshal.AllocHGlobal(capacityChars * sizeof(char));
            try
            {
                int status = call(commandName, parametersJson ?? string.Empty, buffer, capacityChars, out neededChars);
                switch (status)
                {
                    case StatusOk:
                    case StatusCommandFailed:
                        // Both carry a full envelope; ok/error is inside it.
                        return Marshal.PtrToStringUni(buffer, neededChars);
                    case StatusBufferTooSmall:
                        return null;
                    case StatusNotRunning:
                        return Error("Tapioca's native bridge has been stopped. Restart Archicad.");
                    case StatusWrongThread:
                        return Error("This call did not come from Archicad's main thread and was refused. "
                                     + "Tapioca components must do their work in SolveInstance.");
                    case StatusUnknownCommand:
                        return Error("Archicad has no Tapioca command called '" + commandName + "'.");
                    case StatusBadRequest:
                        return Error("The parameters were not valid JSON.");
                    case StatusWriteRefused:
                        return Error("'" + commandName + "' modifies the project, and the Grasshopper bridge "
                                     + "is read-only in this version.");
                    default:
                        return Error("The native bridge returned status "
                                     + status.ToString(CultureInfo.InvariantCulture) + ".");
                }
            }
            finally
            {
                Marshal.FreeHGlobal(buffer);
            }
        }

        private static string Error(string message)
        {
            return "{\"ok\":false,\"error\":\""
                   + message.Replace("\\", "\\\\").Replace("\"", "\\\"")
                   + "\"}";
        }
    }
}
