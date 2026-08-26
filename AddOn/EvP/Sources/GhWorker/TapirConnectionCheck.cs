using System;
using System.Diagnostics;
using System.Globalization;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace Tapioca.GhWorker
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

        /// <summary>
        /// Tapioca's own cheapest registered add-on command, used as the
        /// stopwatch for add-on dispatch latency.
        /// </summary>
        /// <remarks>
        /// Ours rather than Tapir's on purpose: the number this measures is a
        /// property of Archicad's add-on dispatch in this process, not of Tapir,
        /// and using Tapir's command would make an Archicad problem look like a
        /// Tapir problem on any machine that has not installed it.
        /// </remarks>
        private const string TapiocaStatusCommand =
            "{\"command\":\"API.ExecuteAddOnCommand\",\"parameters\":{\"addOnCommandId\":{"
            + "\"commandNamespace\":\"Tapioca\",\"commandName\":\"GetStatus\"},"
            + "\"addOnCommandParameters\":{}}}";

        // Enough samples to tell a constant cost from a warm-up, few enough that
        // they all fit inside the startup the user is already waiting through.
        private const int LatencySamples = 4;

        // Long enough to survive a slow first request, short enough that a
        // wedged reply is REPORTED as wedged rather than waited on. Tapir's own
        // HttpClient keeps the 100-second default, which is why a blocked call
        // there reads to a user as a hang rather than as an error.
        private static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(10);

        // The reentrancy check runs on Archicad's main thread, so its budget is
        // what the user pays on first editor open in the bad case. Six seconds
        // is far longer than a healthy loopback call (measured: 21-150 ms) and
        // short enough not to read as a freeze.
        private static readonly TimeSpan ReentrancyTimeout = TimeSpan.FromSeconds(6);

        // Slack for the wait to observe the HttpClient timeout firing, rather
        // than racing it and reporting "did not return" for a request that was
        // about to report its own timeout.
        private static readonly TimeSpan ReentrancyGrace = TimeSpan.FromSeconds(2);

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

        /// <summary>
        /// Settles, by measurement, whether a Tapir component's request can be
        /// answered while Archicad's main thread waits for it. MUST be called ON
        /// the main thread. Returns a report line; never throws.
        /// </summary>
        /// <remarks>
        /// <para>
        /// THE QUESTION. Grasshopper solves on the thread that constructed
        /// RhinoCore, which is Archicad's main thread. Tapir finishes every
        /// request with <c>task.Wait()</c>. Archicad executes a JSON command on
        /// its main thread. If all three hold at once, a component's request
        /// cannot be served until the solve that issued it returns, and Tapir's
        /// 100-second default timeout is what a user experiences as a hang.
        /// The same package driving the same Archicad from a SEPARATE Rhino
        /// process works, which is what makes the shared thread the suspect.
        /// </para>
        /// <para>
        /// THE EXPERIMENT, and why it is decisive. Two identical requests are
        /// issued: a CONTROL on the thread pool whose result is read afterwards,
        /// and a REPRODUCTION shaped exactly like Tapir's — <c>Task.Run</c>
        /// followed by a blocking wait on this thread. Both are in flight while
        /// this thread is blocked, so their timings separate the two worlds:
        /// </para>
        /// <list type="bullet">
        /// <item>both slow or timed out — Archicad cannot answer while its main
        /// thread waits. Confirmed, and no amount of work inside Grasshopper
        /// will fix it.</item>
        /// <item>control fast, reproduction slow — the block is on the
        /// Grasshopper side of the socket, not Archicad's.</item>
        /// <item>both fast — the hypothesis is dead and the hang is elsewhere;
        /// look at the component, not the thread.</item>
        /// </list>
        /// <para>
        /// It costs one bounded wait on first editor open, and only actually
        /// spends it when the answer is bad news. That is a fair price for
        /// turning an argument into a number.
        /// </para>
        /// </remarks>
        internal static string CheckMainThreadReentrancy(uint port)
        {
            if (port == 0)
            {
                return "Main-thread reentrancy check skipped: Archicad's JSON port is unknown.";
            }

            try
            {
                HttpClientHandler handler = new HttpClientHandler();
                handler.UseProxy = false;
                handler.Proxy = null;

                using (HttpClient client = new HttpClient(handler, true))
                {
                    client.BaseAddress = new Uri(
                        "http://127.0.0.1:" + port.ToString(CultureInfo.InvariantCulture));
                    client.Timeout = ReentrancyTimeout;

                    // ⚠️ IT MUST BE AN ADD-ON COMMAND, AND THE FIRST VERSION OF
                    // THIS CHECK GOT THAT WRONG. It used API.GetProductInfo,
                    // which came back in 23 ms with the main thread blocked and
                    // "proved" there was no deadlock — but the measurement was
                    // meaningless, because GetProductInfo is answered without
                    // ever reaching Archicad's main thread. The same run showed
                    // an ExecuteAddOnCommand taking 2.2 SECONDS on the same
                    // connection, back to back with 3-28 ms official commands.
                    // Add-on dispatch is the thing that needs the main thread,
                    // so it is the only thing worth timing here.
                    //
                    // Tapioca's own GetStatus rather than Tapir's, so the
                    // measurement does not depend on another project being
                    // installed and cannot be confused with a Tapir fault.
                    const string Body = TapiocaStatusCommand;

                    // The control goes first so that it is already on the wire
                    // when this thread stops servicing anything.
                    Task<Reply> control = Task.Run(() => Post(client, Body));

                    // The reproduction, in Tapir's exact shape: Task.Run, then a
                    // blocking wait on the thread Archicad needs.
                    Stopwatch blocked = Stopwatch.StartNew();
                    Task<Reply> reproduction = Task.Run(() => Post(client, Body));
                    bool finished = reproduction.Wait(ReentrancyTimeout + ReentrancyGrace);
                    blocked.Stop();

                    string reproductionText = finished
                        ? Outcome(reproduction.Result) + " in "
                          + reproduction.Result.ElapsedMs.ToString(CultureInfo.InvariantCulture) + " ms"
                        : "DID NOT RETURN within "
                          + blocked.ElapsedMilliseconds.ToString(CultureInfo.InvariantCulture) + " ms";

                    // Read the control only now. Its elapsed time was measured
                    // across the window in which this thread was blocked, which
                    // is the whole point of it.
                    bool controlFinished = control.Wait(ReentrancyGrace);
                    string controlText = controlFinished
                        ? Outcome(control.Result) + " in "
                          + control.Result.ElapsedMs.ToString(CultureInfo.InvariantCulture) + " ms"
                        : "DID NOT RETURN";

                    return "Main-thread reentrancy check: blocking call from Archicad's main thread "
                           + reproductionText + "; background control " + controlText + ". "
                           + Interpret(finished, controlFinished, reproduction, control);
                }
            }
            catch (Exception exception)
            {
                return "Main-thread reentrancy check failed: " + exception.GetType().Name + ": "
                       + exception.Message;
            }
        }

        private static string Interpret(
            bool reproductionFinished,
            bool controlFinished,
            Task<Reply> reproduction,
            Task<Reply> control)
        {
            bool reproductionOk = reproductionFinished && !reproduction.Result.Failed;
            bool controlOk = controlFinished && !control.Result.Failed;

            if (reproductionOk && controlOk)
            {
                return "CONCLUSION: Archicad answers a loopback command while its main thread waits, so the "
                       + "shared-thread deadlock is NOT the cause of slow Tapir components. Look elsewhere.";
            }

            if (!reproductionOk && controlOk)
            {
                return "CONCLUSION: Archicad answered the background request but not the blocking one, so the "
                       + "block is on the Grasshopper side of the socket rather than Archicad's.";
            }

            return "CONCLUSION: Archicad could not answer while its main thread waited. Grasshopper solves on "
                   + "that thread and Tapir waits on it, so a Tapir component's request cannot be served until "
                   + "the solve that issued it returns. Nothing inside Grasshopper can fix this; it needs an "
                   + "in-process bridge that does not go through Archicad's HTTP server, or Grasshopper out of "
                   + "process.";
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

                    // THE NUMBER THAT MATTERS. Official commands answer in
                    // single-digit to low-tens milliseconds here; the first
                    // measured add-on command took 2.2 seconds on the same
                    // connection. If that is a constant per-call cost rather
                    // than a one-off warm-up, then a component making tens of
                    // add-on calls is minutes of waiting by arithmetic alone,
                    // which is exactly the reported symptom. Four samples tell
                    // those two apart.
                    StringBuilder samples = new StringBuilder();
                    long total = 0;
                    long worst = 0;
                    for (int index = 0; index < LatencySamples; index++)
                    {
                        Reply sample = await Post(client, TapiocaStatusCommand);
                        if (index > 0)
                        {
                            samples.Append(", ");
                        }

                        samples.Append(sample.ElapsedMs.ToString(CultureInfo.InvariantCulture));
                        if (sample.Failed)
                        {
                            samples.Append(" (failed)");
                        }

                        total += sample.ElapsedMs;
                        if (sample.ElapsedMs > worst)
                        {
                            worst = sample.ElapsedMs;
                        }
                    }

                    long mean = total / LatencySamples;
                    report.Append(" Add-on dispatch latency over ")
                          .Append(LatencySamples.ToString(CultureInfo.InvariantCulture))
                          .Append(" Tapioca.GetStatus calls: ")
                          .Append(samples)
                          .Append(" ms (mean ")
                          .Append(mean.ToString(CultureInfo.InvariantCulture))
                          .Append(" ms).");
                    if (worst > 250)
                    {
                        report.Append(" THIS IS THE BOTTLENECK: every Tapir component call goes through "
                                      + "API.ExecuteAddOnCommand, so a component making tens of them costs "
                                      + "minutes. Official API.* commands on this same connection answer in "
                                      + "milliseconds.");
                    }
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
            WorkerLog.Write(text);
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
