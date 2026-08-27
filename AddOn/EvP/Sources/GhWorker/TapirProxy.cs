using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Threading;

namespace Tapioca.GhWorker
{
    /// <summary>
    /// A transparent loopback proxy in front of Archicad's JSON port, so that
    /// every request Tapir makes can be counted.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ THIS IS AN INSTRUMENT, NOT A POLICY. It forwards every request and
    /// every response byte-for-byte and changes nothing. It does not defer, batch,
    /// reorder or refuse anything. Deferring writes in order to replay them in one
    /// undo scope is a real possibility and is discussed in the plan, but it hits
    /// read-after-write — a Tapir component consumes the GUID its own write
    /// returns — so it is a decision to take deliberately, not a behaviour to
    /// acquire by extending this class.
    /// </para>
    /// <para>
    /// ⚠️ WHY COUNTING HERE ANSWERS THE QUESTION AT ALL. Archicad exposes no
    /// undo-stack count to an add-on: ACAPI_CallUndoableCommand is the only undo
    /// primitive in the AC29 DevKit, with no begin/end pair and no way to ask how
    /// deep the stack is. But every Tapir write command opens exactly one
    /// undoable scope, so one write REQUEST is one undo step — and requests are
    /// something a proxy can count. That is what makes the loop measurable
    /// without Archicad's cooperation.
    /// </para>
    /// <para>
    /// Tapioca already injects the port into Tapir's process-global
    /// <c>ConnectionSettings.Port</c>, so pointing it here needs no fork and no
    /// cooperation from Tapir. Its <c>ArchicadConnection</c> builds
    /// <c>http://127.0.0.1:{port}</c> — host fixed, port configurable — which is
    /// exactly the seam this uses.
    /// </para>
    /// <para>
    /// ⚠️ OFF BY DEFAULT. A proxy in front of every Tapir call is a latency cost
    /// and one more thing to fail, and it earns that only while someone is
    /// measuring. TAPIOCA_GH_TAPIR_PROXY=1 turns it on; without it Tapir talks
    /// straight to Archicad as before.
    /// </para>
    /// </remarks>
    internal sealed class TapirProxy : IDisposable
    {
        private const string TapirNamespace = "TapirCommand";

        /// <summary>
        /// ⚠️ ONE CLIENT FOR THE PROCESS, NOT ONE PER REQUEST. Tapir creates a
        /// new HttpClient per call — a real defect, noted in the handoff, that
        /// exhausts sockets under load — and a proxy that copied it would put a
        /// second instance of the same bug in front of the first. Timeout is
        /// infinite because this sits in front of calls that legitimately take a
        /// long time, and a proxy that gave up where a direct call would have
        /// succeeded would turn a slow definition into a broken one.
        /// </summary>
        private static readonly HttpClient Forwarder =
            new HttpClient { Timeout = Timeout.InfiniteTimeSpan };

        private readonly object _sync = new object();
        private readonly Dictionary<string, uint> _ledger = new Dictionary<string, uint>();
        private readonly int _archicadPort;

        private HttpListener _listener;
        private Thread _thread;
        private volatile bool _stopping;

        internal TapirProxy(int archicadPort)
        {
            _archicadPort = archicadPort;
        }

        internal int Port { get; private set; }

        internal bool IsRunning
        {
            get { return _listener != null && !_stopping; }
        }

        /// <summary>
        /// Binds a free loopback port and starts forwarding. Returns false with a
        /// reason rather than throwing: a proxy that will not start must degrade
        /// to "no measurement", never to "no Tapir".
        /// </summary>
        internal bool Start(out string error)
        {
            error = string.Empty;

            // Ports are tried rather than requested: HttpListener has no
            // "bind anything free" mode, and a fixed port would collide with the
            // next Archicad. Starting above Archicad's own range keeps the two
            // from being confused in a log.
            for (int port = 19800; port < 19850; port++)
            {
                HttpListener candidate = new HttpListener();
                candidate.Prefixes.Add("http://127.0.0.1:" + port + "/");
                try
                {
                    candidate.Start();
                    _listener = candidate;
                    Port = port;
                    break;
                }
                catch (Exception)
                {
                    try
                    {
                        candidate.Close();
                    }
                    catch (Exception)
                    {
                    }
                }
            }

            if (_listener == null)
            {
                error = "The Tapir proxy could not bind a loopback port in 19800-19849.";
                return false;
            }

            _stopping = false;
            _thread = new Thread(Serve);
            _thread.IsBackground = true;
            _thread.Name = "Tapir proxy";
            _thread.Start();
            return true;
        }

        /// <summary>
        /// Empties the ledger. Called at the start of a Run so the count belongs
        /// to that run and not to everything since the worker started.
        /// </summary>
        internal void ResetLedger()
        {
            lock (_sync)
            {
                _ledger.Clear();
            }
        }

        /// <summary>
        /// What the run called, and how often. The host classifies and judges —
        /// see Sources/AddOn/Grasshopper/GhUndoBudget.hpp — because that is where
        /// the offline tests are, and a rule with no test is a rule that drifts.
        /// </summary>
        internal List<KeyValuePair<string, uint>> Ledger()
        {
            lock (_sync)
            {
                return new List<KeyValuePair<string, uint>>(_ledger);
            }
        }

        public void Dispose()
        {
            _stopping = true;
            try
            {
                if (_listener != null)
                {
                    _listener.Close();
                }
            }
            catch (Exception)
            {
            }

            _listener = null;
        }

        private void Serve()
        {
            while (!_stopping)
            {
                HttpListenerContext context;
                try
                {
                    context = _listener.GetContext();
                }
                catch (Exception)
                {
                    // Close() during GetContext is the ordinary way this ends.
                    return;
                }

                try
                {
                    Forward(context);
                }
                catch (Exception exception)
                {
                    WorkerLog.Write("the Tapir proxy failed a request: " + WorkerLog.Describe(exception));
                    try
                    {
                        context.Response.StatusCode = 502;
                        context.Response.Close();
                    }
                    catch (Exception)
                    {
                    }
                }
            }
        }

        private void Forward(HttpListenerContext context)
        {
            string body;
            using (StreamReader reader = new StreamReader(context.Request.InputStream, Encoding.UTF8))
            {
                body = reader.ReadToEnd();
            }

            Record(body);

            using (HttpContent content = new StringContent(body, Encoding.UTF8, "application/json"))
            using (HttpResponseMessage response =
                Forwarder.PostAsync("http://127.0.0.1:" + _archicadPort, content).GetAwaiter().GetResult())
            {
                byte[] answer = response.Content.ReadAsByteArrayAsync().GetAwaiter().GetResult();
                context.Response.StatusCode = (int)response.StatusCode;
                context.Response.ContentType = "application/json";
                context.Response.ContentLength64 = answer.Length;
                context.Response.OutputStream.Write(answer, 0, answer.Length);
            }

            context.Response.Close();
        }

        /// <summary>
        /// Records the EFFECTIVE command name.
        /// </summary>
        /// <remarks>
        /// ⚠️ API.ExecuteAddOnCommand IS UNWRAPPED, AND THAT IS THE WHOLE POINT.
        /// Every Tapir call arrives as ExecuteAddOnCommand carrying the real
        /// command inside; counting the wrapper would report a run as "40 calls to
        /// ExecuteAddOnCommand" and hide which modification cost the undo steps.
        /// Read with a deliberate reader rather than a JSON parser: this is on the
        /// path of every Tapir call, the shape is fixed and produced by Tapir
        /// itself, and taking a JSON dependency into the worker to read two string
        /// fields would be a poor trade.
        /// </remarks>
        private void Record(string body)
        {
            string command = ReadString(body, "\"command\"");
            if (command == null)
            {
                command = "(unparsed)";
            }
            else if (command == "API.ExecuteAddOnCommand")
            {
                string ns = ReadString(body, "\"commandNamespace\"");
                string name = ReadString(body, "\"commandName\"");
                if (name != null)
                {
                    command = (ns ?? TapirNamespace) + "." + name;
                }
            }

            lock (_sync)
            {
                uint count;
                _ledger.TryGetValue(command, out count);
                _ledger[command] = count + 1;
            }
        }

        /// <summary>The string value following a key, or null.</summary>
        private static string ReadString(string body, string key)
        {
            int at = body.IndexOf(key, StringComparison.Ordinal);
            if (at < 0)
            {
                return null;
            }

            int colon = body.IndexOf(':', at + key.Length);
            if (colon < 0)
            {
                return null;
            }

            int open = body.IndexOf('"', colon + 1);
            if (open < 0)
            {
                return null;
            }

            int close = body.IndexOf('"', open + 1);
            return close < 0 ? null : body.Substring(open + 1, close - open - 1);
        }
    }
}
