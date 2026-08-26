using System;
using System.Diagnostics;
using System.Globalization;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace Tapioca.GrasshopperHost
{
    /// <summary>
    /// Asks Archicad the three questions that decide whether a Tapir component
    /// can work, and answers them with measurements instead of assumptions.
    /// </summary>
    /// <remarks>
    /// <para>
    /// TAPIR IS TWO HALVES, AND ONLY ONE OF THEM IS THE GRASSHOPPER PACKAGE.
    /// The .gha speaks two different protocols over the same socket:
    /// <c>ToArchicad</c> sends Graphisoft's own <c>API.*</c> commands, which any
    /// Archicad answers, while <c>ToAddOn</c> sends
    /// <c>API.ExecuteAddOnCommand</c> into the <c>TapirCommand</c> namespace,
    /// which exists only if the separate TAPIR ARCHICAD ADD-ON is installed and
    /// only carries the commands THAT add-on's version knows. Most Tapir
    /// components use the second one. So "ConnectArchicad says true but
    /// everything else fails" is the expected symptom of a missing or
    /// version-mismatched Archicad add-on: ConnectArchicad pings
    /// <c>API.IsAlive</c> and never touches the add-on at all.
    /// </para>
    /// <para>
    /// THIS NEVER RUNS ON ARCHICAD'S MAIN THREAD, AND THAT IS THE POINT. It is
    /// the same loopback HTTP call a Tapir component makes, so issuing it from
    /// the thread Archicad services its JSON queue on would mean waiting for a
    /// reply that cannot be produced until the wait ends. Running it on a
    /// thread-pool thread and never awaiting it keeps the measurement honest and
    /// keeps a slow or absent server from delaying Archicad's startup at all.
    /// </para>
    /// <para>
    /// Nothing here calls ACAPI. This is loopback HTTP to Archicad's own JSON
    /// port, which is the architecture the unchanged Tapir package already uses.
    /// It is a diagnostic, not a second bridge, and must not grow into one.
    /// </para>
    /// </remarks>
    internal static class TapirConnectionCheck
    {
        private const string AddOnVersionCommand = "GetAddOnVersion";
        private const string TapirNamespace = "TapirCommand";

        // Long enough to survive a slow first request, short enough that a
        // wedged reply is REPORTED as wedged rather than waited on. Tapir's own
        // HttpClient keeps the 100-second default, which is why a blocked call
        // there reads to a user as a hang rather than as an error.
        private static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(10);

        private static string _report = "The Tapir connection check has not run.";
        private static int _started;

        /// <summary>
        /// The last completed check, as one line. Safe to read from any thread.
        /// </summary>
        internal static string Report
        {
            get { return Volatile.Read(ref _report); }
        }

        /// <summary>
        /// Starts the check and returns immediately. Runs at most once per
        /// session; never throws.
        /// </summary>
        internal static void Begin(uint port)
        {
            if (Interlocked.Exchange(ref _started, 1) != 0)
            {
                return;
            }

            if (port == 0)
            {
                Volatile.Write(ref _report, "Tapir connection check skipped: Archicad's JSON port is unknown.");
                return;
            }

            try
            {
                Task.Run(() => Run(port));
            }
            catch (Exception exception)
            {
                Volatile.Write(ref _report, "Tapir connection check could not start: " + exception.Message);
            }
        }

        private static async Task Run(uint port)
        {
            StringBuilder report = new StringBuilder();
            report.Append("Tapir connection check on port ")
                  .Append(port.ToString(CultureInfo.InvariantCulture))
                  .Append(':');

            try
            {
                // Proxy explicitly OFF. A system proxy or a WPAD lookup applied
                // to a 127.0.0.1 request is a well-known way to turn a
                // sub-millisecond loopback call into a multi-second one, and
                // Tapir builds a fresh proxy-aware HttpClient for every call.
                // Ruling it out here is what lets a slow Tapir component point
                // at Archicad rather than at the network stack.
                HttpClientHandler handler = new HttpClientHandler();
                handler.UseProxy = false;
                handler.Proxy = null;

                using (HttpClient client = new HttpClient(handler, true))
                {
                    client.BaseAddress = new Uri(
                        "http://127.0.0.1:" + port.ToString(CultureInfo.InvariantCulture));
                    client.Timeout = RequestTimeout;

                    Reply alive = await Post(client, "{\"command\":\"API.IsAlive\"}");
                    Append(report, "API.IsAlive", alive, string.Empty);

                    Reply product = await Post(client, "{\"command\":\"API.GetProductInfo\"}");
                    Append(report, "API.GetProductInfo", product, DescribeProduct(product));

                    Reply addOn = await Post(
                        client,
                        "{\"command\":\"API.ExecuteAddOnCommand\",\"parameters\":{\"addOnCommandId\":{"
                        + "\"commandNamespace\":\"" + TapirNamespace + "\","
                        + "\"commandName\":\"" + AddOnVersionCommand + "\"},"
                        + "\"addOnCommandParameters\":{}}}");
                    Append(report, "Tapir Archicad add-on", addOn, DescribeAddOn(addOn));
                }
            }
            catch (Exception exception)
            {
                report.Append(" check failed: ")
                      .Append(exception.GetType().Name)
                      .Append(": ")
                      .Append(exception.Message);
            }

            string text = report.ToString();
            Volatile.Write(ref _report, text);
            Log.Write(text);
        }

        private static void Append(StringBuilder report, string label, Reply reply, string detail)
        {
            report.Append(' ')
                  .Append(label)
                  .Append(' ')
                  .Append(Outcome(reply))
                  .Append(" in ")
                  .Append(reply.ElapsedMs.ToString(CultureInfo.InvariantCulture))
                  .Append(" ms.");
            if (!string.IsNullOrEmpty(detail))
            {
                report.Append(' ').Append(detail);
            }
        }

        /// <summary>
        /// One reply, how long it took, and whether it is a reply at all.
        /// </summary>
        /// <remarks>
        /// The timing travels WITH the body rather than in a field: an await can
        /// resume on a different thread-pool thread, so a shared stopwatch
        /// reading would be some other request's, or nobody's. The failure flag
        /// is a flag for the same reason a sentinel character would be a bad
        /// idea: a transport error message is arbitrary text and must not have
        /// to be distinguished from a real reply by inspecting it.
        /// </remarks>
        private readonly struct Reply
        {
            internal Reply(string body, long elapsedMs, bool failed)
            {
                Body = body;
                ElapsedMs = elapsedMs;
                Failed = failed;
            }

            internal string Body { get; }

            internal long ElapsedMs { get; }

            internal bool Failed { get; }
        }

        private static async Task<Reply> Post(HttpClient client, string body)
        {
            Stopwatch watch = Stopwatch.StartNew();
            try
            {
                using (StringContent content = new StringContent(body, Encoding.UTF8, "application/json"))
                using (HttpResponseMessage response = await client.PostAsync(string.Empty, content))
                {
                    string text = await response.Content.ReadAsStringAsync();
                    watch.Stop();
                    return new Reply(text, watch.ElapsedMilliseconds, false);
                }
            }
            catch (Exception exception)
            {
                watch.Stop();
                return new Reply(
                    exception.GetType().Name + ": " + exception.Message,
                    watch.ElapsedMilliseconds,
                    true);
            }
        }

        private static string Outcome(Reply reply)
        {
            if (reply.Failed)
            {
                return "FAILED (" + reply.Body + ")";
            }

            try
            {
                using (JsonDocument document = JsonDocument.Parse(reply.Body))
                {
                    JsonElement succeeded;
                    if (document.RootElement.TryGetProperty("succeeded", out succeeded)
                        && succeeded.ValueKind == JsonValueKind.True)
                    {
                        return "ok";
                    }

                    JsonElement error;
                    JsonElement message;
                    if (document.RootElement.TryGetProperty("error", out error)
                        && error.TryGetProperty("message", out message))
                    {
                        return "refused (" + message.GetString() + ")";
                    }

                    return "refused";
                }
            }
            catch (Exception)
            {
                return "unreadable reply";
            }
        }

        /// <summary>
        /// Names the Archicad that actually answered, because with two Archicads
        /// open "the port replied" is not the same as "the right model replied".
        /// </summary>
        private static string DescribeProduct(Reply reply)
        {
            if (reply.Failed)
            {
                return string.Empty;
            }

            try
            {
                using (JsonDocument document = JsonDocument.Parse(reply.Body))
                {
                    JsonElement result;
                    JsonElement version;
                    JsonElement build;
                    if (document.RootElement.TryGetProperty("result", out result)
                        && result.TryGetProperty("version", out version)
                        && result.TryGetProperty("buildNumber", out build))
                    {
                        return "Answered by Archicad " + version.GetRawText() + " build " + build.GetRawText() + ".";
                    }
                }
            }
            catch (Exception)
            {
                // An unreadable reply is already reported by Outcome.
            }

            return string.Empty;
        }

        /// <summary>
        /// The whole reason this check exists: the Archicad add-on's version,
        /// and whether it matches the Grasshopper package's.
        /// </summary>
        private static string DescribeAddOn(Reply reply)
        {
            if (!reply.Failed)
            {
                try
                {
                    using (JsonDocument document = JsonDocument.Parse(reply.Body))
                    {
                        JsonElement result;
                        JsonElement inner;
                        JsonElement version;
                        if (document.RootElement.TryGetProperty("result", out result)
                            && result.TryGetProperty("addOnCommandResponse", out inner)
                            && inner.TryGetProperty("version", out version))
                        {
                            string text = version.GetString() ?? string.Empty;
                            if (string.Equals(text, TapirPackage.PinnedVersion, StringComparison.OrdinalIgnoreCase))
                            {
                                return "Add-on version " + text + " matches the Grasshopper package.";
                            }

                            return "VERSION MISMATCH: the Archicad add-on is " + text
                                   + " but the Grasshopper package is " + TapirPackage.PinnedVersion
                                   + ". Components calling commands the older add-on does not have will fail. "
                                   + "Install the matching TapirAddOn_AC29_Win.apx through "
                                   + "Options > Add-On Manager.";
                        }
                    }
                }
                catch (Exception)
                {
                    // Handled by Outcome.
                }
            }

            return "The TapirCommand namespace did not answer, so the Tapir ARCHICAD ADD-ON is probably not "
                   + "installed. The Grasshopper package alone is not enough: most Tapir components go through "
                   + "API.ExecuteAddOnCommand into that namespace. Install TapirAddOn_AC29_Win.apx through "
                   + "Options > Add-On Manager.";
        }
    }
}
