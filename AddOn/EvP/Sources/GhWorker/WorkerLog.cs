using System;
using System.Globalization;
using System.IO;

namespace Tapioca.GhWorker
{
    /// <summary>
    /// Where the worker says things, and to whom.
    /// </summary>
    /// <remarks>
    /// <para>
    /// TWO SINKS, AND THE SPLIT IS NOT ARBITRARY. Once the bridge is up every
    /// line goes to the ADD-ON, which appends it to <c>logs\grasshopper.log</c>
    /// stamped with this worker's pid and restart generation. That keeps
    /// grasshopper.log to exactly ONE writer, which matters more than it sounds:
    /// two processes appending to one file with open/append/close per line
    /// interleave half-written lines and throw sharing violations at each other,
    /// and the file whose whole job is explaining a failure is the worst possible
    /// place for a race.
    /// </para>
    /// <para>
    /// Before the handshake there is no bridge, so those lines go to the BOOT LOG
    /// — a separate file the add-on names on the command line and never writes
    /// itself. It exists for exactly one question, and it is the question a
    /// missing worker raises: what did it manage to say before it died?
    /// </para>
    /// <para>Failing to log is never allowed to fail a start.</para>
    /// </remarks>
    internal static class WorkerLog
    {
        private static readonly object Sync = new object();

        private static string _bootLogPath;
        private static BridgeClient _bridge;

        internal static void OpenBootLog(string path)
        {
            lock (Sync)
            {
                _bootLogPath = string.IsNullOrWhiteSpace(path) ? null : path;
            }
        }

        /// <summary>
        /// Switches the sink to the bridge. Called once, after the handshake.
        /// </summary>
        internal static void AttachBridge(BridgeClient bridge)
        {
            lock (Sync)
            {
                _bridge = bridge;
            }
        }

        internal static void Write(string message)
        {
            if (string.IsNullOrEmpty(message))
            {
                return;
            }

            BridgeClient bridge;
            string bootLogPath;
            lock (Sync)
            {
                bridge = _bridge;
                bootLogPath = _bootLogPath;
            }

            if (bridge != null && bridge.IsConnected)
            {
                bridge.Log(message);
                return;
            }

            if (bootLogPath == null)
            {
                return;
            }

            try
            {
                lock (Sync)
                {
                    File.AppendAllText(
                        bootLogPath,
                        string.Format(
                            CultureInfo.InvariantCulture,
                            "{0:yyyy-MM-dd HH:mm:ss} [worker boot] {1}{2}",
                            DateTime.Now,
                            message,
                            Environment.NewLine));
                }
            }
            catch (Exception)
            {
                // See the remarks: a log that cannot be written must never be the
                // reason a worker fails to start.
            }
        }

        internal static string Describe(Exception exception)
        {
            if (exception == null)
            {
                return string.Empty;
            }

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
