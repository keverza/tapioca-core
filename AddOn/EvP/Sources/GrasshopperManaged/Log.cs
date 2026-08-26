using System;
using System.Globalization;
using System.IO;

namespace Tapioca.GrasshopperHost
{
    /// <summary>
    /// Appends to the SAME logs\grasshopper.log the native host writes.
    /// </summary>
    /// <remarks>
    /// One file, two writers, on purpose: the startup order crosses the managed
    /// boundary twice, and two files would have to be interleaved by hand every
    /// time someone asks "how far did it get". Each line is opened, written and
    /// closed, like PathUtils::AppendTextLine on the other side — a start that
    /// takes Archicad down leaves nothing behind except what already reached
    /// the disk. Failing to log is never allowed to fail a start.
    /// </remarks>
    internal static class Log
    {
        // ⚠️ WRITERS ARE NO LONGER ALL ON ONE THREAD. TapirConnectionCheck logs
        // from the thread pool while the main thread is still starting the host,
        // and two unsynchronised File.AppendAllText calls on one path is how a
        // line ends up half-written or lost. The lock costs nothing on a path
        // that already opens and closes the file per line.
        private static readonly object Sync = new object();
        private static string _path;

        internal static void Open(string path)
        {
            lock (Sync)
            {
                _path = string.IsNullOrWhiteSpace(path) ? null : path;
            }
        }

        internal static void Close()
        {
            lock (Sync)
            {
                _path = null;
            }
        }

        internal static void Write(string message)
        {
            if (string.IsNullOrEmpty(message))
            {
                return;
            }

            try
            {
                lock (Sync)
                {
                    if (_path == null)
                    {
                        return;
                    }

                    File.AppendAllText(
                        _path,
                        string.Format(
                            CultureInfo.InvariantCulture,
                            "  [managed] {0}{1}",
                            message,
                            Environment.NewLine));
                }
            }
            catch (Exception)
            {
                // A log that cannot be written must never be the reason a host
                // fails to start.
            }
        }
    }
}
