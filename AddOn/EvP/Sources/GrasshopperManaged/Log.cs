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
        private static string _path;

        internal static void Open(string path)
        {
            _path = string.IsNullOrWhiteSpace(path) ? null : path;
        }

        internal static void Close()
        {
            _path = null;
        }

        internal static void Write(string message)
        {
            if (_path == null || string.IsNullOrEmpty(message))
            {
                return;
            }

            try
            {
                File.AppendAllText(
                    _path,
                    string.Format(
                        CultureInfo.InvariantCulture,
                        "  [managed] {0}{1}",
                        message,
                        Environment.NewLine));
            }
            catch (Exception)
            {
                // A log that cannot be written must never be the reason a host
                // fails to start.
            }
        }
    }
}
