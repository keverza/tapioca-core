using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Tapioca.GrasshopperHost
{
    /// <summary>
    /// The four entry points EvP.apx resolves through hostfxr. Nothing else in
    /// this assembly is reachable from native code.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Every method here obeys the same three rules, and they are the reason
    /// this class exists separately from <see cref="GrasshopperSession"/>:
    /// </para>
    /// <list type="number">
    /// <item>NO ORDINARY EXCEPTION MAY ESCAPE. An exception crossing an
    /// [UnmanagedCallersOnly] boundary does not unwind into the C++ caller —
    /// it terminates the process, and the process here is Archicad with the
    /// user's unsaved project in it. So every body is wrapped, and the failure
    /// becomes a status code plus a message the add-on can copy out.
    /// <para>
    /// ⚠️ THESE CATCHES DO NOT PROTECT ARCHICAD FROM RHINO. A corrupted-state
    /// exception — an <see cref="AccessViolationException"/> above all — is not
    /// catchable in .NET Core at all: the runtime terminates the process without
    /// running any handler, and <c>HandleProcessCorruptedStateExceptions</c> is a
    /// no-op here. That is not hypothetical; it is how the opennurbs conflict
    /// took Archicad down (RHINO.L1 arm 2). Anything that can access-violate must
    /// therefore be REFUSED BEFORE IT IS CALLED, natively, which is what
    /// GrasshopperHost's preflight checks and crash breadcrumb are for. Do not
    /// weaken those on the strength of the catches in this file.
    /// </para></item>
    /// <item>NO RHINO TYPE IS NAMED IN THIS FILE. Touching one would make the
    /// JIT load RhinoCommon while compiling the method that is supposed to
    /// install the resolver first. That is why the session lives behind
    /// <see cref="MethodImplOptions.NoInlining"/> in its own type: the standard
    /// Rhino.Inside arrangement, and the one thing that reliably breaks when it
    /// is "simplified" into one class.</item>
    /// <item>NOTHING CALLS ARCHICAD. There is no callback into the add-on at
    /// all in this slice — see GrasshopperHostApi.h.</item>
    /// </list>
    /// </remarks>
    public static class Bootstrap
    {
        // Mirrors TapiocaGhStatus in Sources/AddOn/Grasshopper/GrasshopperHostApi.h.
        private const int StatusOk = 0;
        private const int StatusAlreadyRunning = 1;
        private const int StatusTerminal = 3;
        private const int StatusNotRunning = 4;
        private const int StatusAbiMismatch = 5;
        private const int StatusRhinoMissing = 9;
        private const int StatusRhinoInitFailed = 10;
        private const int StatusLicenceUnavailable = 11;
        private const int StatusGrasshopperFailed = 12;
        private const int StatusEditorUnavailable = 14;
        private const int StatusFaulted = 13;

        private const uint AbiVersion = 3;
        private const uint FlagLoadGrasshopper = 0x0001u;
        private const uint FlagShowEditor = 0x0002u;

        private static readonly object Sync = new object();
        private static string _lastMessage = string.Empty;

        /// <summary>Mirrors TapiocaGhStartRequest. Blittable on purpose.</summary>
        [StructLayout(LayoutKind.Sequential)]
        private struct StartRequest
        {
            public uint StructSize;
            public uint AbiVersion;
            public uint Flags;
            public uint ArchicadJsonPort;
            public IntPtr RhinoSystemDir;
            public IntPtr LogPath;
        }

        [UnmanagedCallersOnly]
        public static int Start(IntPtr requestPointer)
        {
            try
            {
                if (requestPointer == IntPtr.Zero)
                {
                    SetMessage("The start request was null.");
                    return StatusAbiMismatch;
                }

                StartRequest request = Marshal.PtrToStructure<StartRequest>(requestPointer);
                if (request.AbiVersion != AbiVersion
                    || request.StructSize != (uint)Marshal.SizeOf<StartRequest>())
                {
                    SetMessage(
                        $"ABI mismatch: the add-on sent version {request.AbiVersion} in {request.StructSize} bytes, "
                        + $"this managed host speaks version {AbiVersion} in {Marshal.SizeOf<StartRequest>()} bytes. "
                        + "Rebuild and redeploy both halves together.");
                    return StatusAbiMismatch;
                }

                string rhinoSystemDir = request.RhinoSystemDir == IntPtr.Zero
                    ? null
                    : Marshal.PtrToStringUni(request.RhinoSystemDir);
                string logPath = request.LogPath == IntPtr.Zero
                    ? null
                    : Marshal.PtrToStringUni(request.LogPath);

                lock (Sync)
                {
                    Log.Open(logPath);
                    // Recorded, not re-derived: only native code can answer which
                    // Archicad this process is, and it already has.
                    GrasshopperSession.ArchicadJsonPort = request.ArchicadJsonPort;
                    return StartCore(rhinoSystemDir, request.Flags);
                }
            }
            catch (Exception exception)
            {
                SetMessage(Describe(exception));
                return StatusFaulted;
            }
        }

        [UnmanagedCallersOnly]
        public static int Stop()
        {
            try
            {
                lock (Sync)
                {
                    return StopCore();
                }
            }
            catch (Exception exception)
            {
                SetMessage(Describe(exception));
                return StatusFaulted;
            }
        }

        /// <summary>
        /// Shows the Grasshopper canvas over the running core. Slice 1's whole
        /// user-visible feature.
        /// </summary>
        /// <remarks>
        /// Requires a running host and says so rather than starting one: the
        /// native side owns "start if needed", because it is the side that
        /// holds the lifecycle state machine and the preflight checks. A
        /// managed shortcut here would be a second, weaker start path.
        /// </remarks>
        [UnmanagedCallersOnly]
        public static int ShowEditor()
        {
            return SetEditorVisibility(true);
        }

        [UnmanagedCallersOnly]
        public static int HideEditor()
        {
            return SetEditorVisibility(false);
        }

        /// <summary>
        /// Never throws and never blocks: the add-on calls this while tearing
        /// down, when a lock held by a wedged start would be fatal.
        /// </summary>
        [UnmanagedCallersOnly]
        public static int State()
        {
            try
            {
                return (int)GrasshopperSession.State;
            }
            catch (Exception)
            {
                return (int)HostState.Failed;
            }
        }

        /// <summary>
        /// Copies the last message as UTF-16 into a buffer the CALLER owns.
        /// Returns the character count excluding the terminator; call with a
        /// null buffer to ask how much room is needed.
        /// </summary>
        [UnmanagedCallersOnly]
        public static int CopyLastMessage(IntPtr buffer, int capacityChars)
        {
            try
            {
                string message;
                lock (Sync)
                {
                    message = _lastMessage ?? string.Empty;
                }

                if (buffer == IntPtr.Zero || capacityChars <= message.Length)
                {
                    return message.Length;
                }

                if (message.Length > 0)
                {
                    Marshal.Copy(message.ToCharArray(), 0, buffer, message.Length);
                }

                Marshal.WriteInt16(buffer, message.Length * sizeof(char), 0);
                return message.Length;
            }
            catch (Exception)
            {
                return 0;
            }
        }

        // ⚠️ NoInlining, and it is not a micro-optimisation in reverse: it stops
        // the JIT from having to resolve GrasshopperSession (and through it
        // RhinoCommon) while it is compiling Start, which runs BEFORE the
        // resolver is installed. Inlined, this fails as a FileNotFoundException
        // for RhinoCommon on a machine that has Rhino installed and working.
        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int StartCore(string rhinoSystemDir, uint flags)
        {
            switch (GrasshopperSession.State)
            {
                case HostState.Running:
                    SetMessage("A Rhino core is already running in this process.");
                    return StatusAlreadyRunning;
                case HostState.Stopped:
                    SetMessage("Rhino was stopped in this process and cannot be reconstructed. Restart Archicad.");
                    return StatusTerminal;
                default:
                    break;
            }

            StartOutcome outcome = GrasshopperSession.Start(
                rhinoSystemDir,
                (flags & FlagLoadGrasshopper) != 0,
                (flags & FlagShowEditor) != 0);

            SetMessage(outcome.Message);
            switch (outcome.Kind)
            {
                case StartOutcomeKind.Started:
                    return StatusOk;
                case StartOutcomeKind.RhinoMissing:
                    return StatusRhinoMissing;
                case StartOutcomeKind.LicenceUnavailable:
                    return StatusLicenceUnavailable;
                case StartOutcomeKind.GrasshopperFailed:
                    return StatusGrasshopperFailed;
                default:
                    return StatusRhinoInitFailed;
            }
        }

        private static int SetEditorVisibility(bool visible)
        {
            try
            {
                lock (Sync)
                {
                    return SetEditorVisibilityCore(visible);
                }
            }
            catch (Exception exception)
            {
                SetMessage(Describe(exception));
                return StatusFaulted;
            }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int SetEditorVisibilityCore(bool visible)
        {
            if (GrasshopperSession.State != HostState.Running)
            {
                SetMessage("Rhino is not running, so there is no Grasshopper editor to show.");
                return StatusNotRunning;
            }

            string failure;
            if (!RhinoBoot.SetEditorVisible(visible, out failure))
            {
                SetMessage(failure);
                return StatusEditorUnavailable;
            }

            SetMessage(visible ? "Grasshopper editor shown." : "Grasshopper editor hidden.");
            return StatusOk;
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static int StopCore()
        {
            if (GrasshopperSession.State != HostState.Running)
            {
                SetMessage("Nothing to stop.");
                return StatusNotRunning;
            }

            string report = GrasshopperSession.Stop();
            SetMessage(report);
            return StatusOk;
        }

        private static void SetMessage(string message)
        {
            lock (Sync)
            {
                _lastMessage = message ?? string.Empty;
            }

            Log.Write(message);
        }

        // Aggregate and inner exceptions matter more than usual here: a
        // RhinoCore that will not start almost always reports the real reason
        // (a licence, a missing file) one level down.
        private static string Describe(Exception exception)
        {
            string text = exception.GetType().Name + ": " + exception.Message;
            Exception inner = exception.InnerException;
            while (inner != null)
            {
                text += " -> " + inner.GetType().Name + ": " + inner.Message;
                inner = inner.InnerException;
            }

            return text;
        }
    }
}
