using System;
using System.Collections.Generic;
using System.Globalization;
using System.Runtime.CompilerServices;
using System.Security.Cryptography;
using System.Text;

namespace Tapioca.GhWorker
{
    /// <summary>
    /// One workflow session: a mode, a document, an input snapshot, and the two
    /// revision counters that decide what may be published.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ TWO COUNTERS, NOT ONE, AND THEY ADVANCE AT DIFFERENT RATES.
    /// HANDOFF-GHHost.md §7: <c>requestRevision</c> counts what the host ASKED
    /// for and is the host's; <c>solutionRevision</c> counts what this session
    /// actually PRODUCED and is the worker's. A slider drag that requests forty
    /// revisions and completes three solutions is the ordinary case, and it is
    /// unreadable if one number is made to serve both.
    /// </para>
    /// <para>
    /// ⚠️ ONE PENDING SOLVE, AND OLDER PENDING WORK IS REPLACED RATHER THAN
    /// QUEUED. §6: "A second solve request while one runs replaces the single
    /// pending request. It does not create an unbounded queue." A drag that
    /// queued one solve per pixel would still be solving minutes after the user
    /// let go, on values nobody is looking at any more.
    /// </para>
    /// <para>
    /// ⚠️ A RUNNING SOLVE IS NOT CANCELLED MERELY BECAUSE A NEWER REQUEST
    /// ARRIVED. §7 again: cooperative abort is only requested when the component
    /// set is known to tolerate it, and it cannot stop a blocked component
    /// anyway. The default is to finish, discard, and immediately run the one
    /// newest pending revision.
    /// </para>
    /// <para>
    /// State that the reader thread touches is under <c>_sync</c>; everything
    /// that names a Grasshopper type runs on the engine thread. The split is why
    /// a cancel can arrive while a solve holds the engine.
    /// </para>
    /// </remarks>
    internal sealed class WorkflowSession
    {
        private readonly object _sync = new object();

        /// <summary>The input snapshot, in the order the host sent it.</summary>
        /// <remarks>
        /// A list rather than a dictionary: the ORDER is part of what is hashed,
        /// and two snapshots that differ only in the order the panel walked its
        /// controls are the same snapshot to the definition but would hash
        /// differently if the order were allowed to vary. It is rebuilt whole on
        /// every SetInputs, so there is nothing to merge and nothing to key.
        /// </remarks>
        private readonly List<SessionProtocol.InputValue> _inputs = new List<SessionProtocol.InputValue>();

        private readonly DefinitionHost _definition = new DefinitionHost();

        private SessionProtocol.SessionState _state = SessionProtocol.SessionState.Empty;
        private SessionProtocol.SessionMode _mode = SessionProtocol.SessionMode.Headless;
        private uint _newestRequestedRevision;
        private uint _pendingWants;
        private uint _solutionRevision;
        private bool _solvePosted;
        private string _inputsApplied = string.Empty;

        internal WorkflowSession(uint sessionId, SessionProtocol.SessionMode mode)
        {
            SessionId = sessionId;
            _mode = mode;
        }

        internal uint SessionId { get; private set; }

        internal SessionProtocol.SessionState State
        {
            get { lock (_sync) { return _state; } }
        }

        internal SessionProtocol.SessionMode Mode
        {
            get { lock (_sync) { return _mode; } }
        }

        internal DefinitionHost Definition
        {
            get { return _definition; }
        }

        internal void SetState(SessionProtocol.SessionState state)
        {
            lock (_sync)
            {
                _state = state;
            }
        }

        internal void SetMode(SessionProtocol.SessionMode mode)
        {
            lock (_sync)
            {
                _mode = mode;
            }
        }

        /// <summary>
        /// Records a solve request and says whether the caller must post the
        /// engine work for it.
        /// </summary>
        /// <remarks>
        /// Called on the reader thread. False means a pending solve is already
        /// posted and will pick this revision up when it runs — which is what
        /// makes the queue one deep no matter how fast the requests arrive.
        /// </remarks>
        internal bool RequestSolve(uint requestRevision, uint wants)
        {
            lock (_sync)
            {
                // ⚠️ AN OLDER REVISION NEVER OVERWRITES A NEWER ONE, EVEN THOUGH
                // THE PIPE DELIVERS IN ORDER. The host is entitled to abandon a
                // revision without sending it; what it is not entitled to do is
                // move the newest backwards, and defending that here means the
                // ordering rule holds even if a future transport reorders.
                if (requestRevision > _newestRequestedRevision)
                {
                    _newestRequestedRevision = requestRevision;
                    _pendingWants = wants;
                }
                else if (requestRevision == _newestRequestedRevision)
                {
                    // The same revision asking for more than last time. Merged
                    // rather than replaced: a preview request followed by a data
                    // request for one revision wants both collected, and taking
                    // the later word alone would drop the preview.
                    _pendingWants |= wants;
                }
                else
                {
                    return false;
                }

                if (_solvePosted)
                {
                    return false;
                }

                _solvePosted = true;
                return true;
            }
        }

        /// <summary>
        /// Takes the newest pending request. Called on the engine thread, once
        /// per posted solve.
        /// </summary>
        internal void TakePendingSolve(out uint requestRevision, out uint wants)
        {
            lock (_sync)
            {
                requestRevision = _newestRequestedRevision;
                wants = _pendingWants;
                // Cleared HERE, at the start of the engine work rather than at
                // its end: a request arriving during the solve must be able to
                // post the next one, and clearing at the end would make it the
                // one that is dropped.
                _solvePosted = false;
            }
        }

        /// <summary>
        /// True when a newer request has arrived than the one about to be
        /// published.
        /// </summary>
        internal bool IsStale(uint requestRevision)
        {
            lock (_sync)
            {
                return requestRevision < _newestRequestedRevision;
            }
        }

        internal uint NextSolutionRevision()
        {
            lock (_sync)
            {
                // From 1, never 0: Apply addresses {generation, session,
                // solutionRevision} and the codec refuses a zero there.
                _solutionRevision++;
                return _solutionRevision;
            }
        }

        /// <summary>
        /// Replaces the whole input snapshot and returns its hash.
        /// </summary>
        internal string SetInputs(IList<SessionProtocol.InputValue> inputs)
        {
            lock (_sync)
            {
                _inputs.Clear();
                if (inputs != null)
                {
                    _inputs.AddRange(inputs);
                }

                _inputsApplied = HashInputs(_inputs);
                return _inputsApplied;
            }
        }

        /// <summary>
        /// The snapshot as two parallel arrays, ready for the package facade.
        /// </summary>
        internal void SnapshotFor(out string[] ids, out string[] values, out string hash)
        {
            lock (_sync)
            {
                ids = new string[_inputs.Count];
                values = new string[_inputs.Count];
                for (int index = 0; index < _inputs.Count; index++)
                {
                    ids[index] = _inputs[index].Id;
                    values[index] = _inputs[index].Value;
                }

                hash = _inputsApplied;
            }
        }

        /// <summary>
        /// Disposes the document this session owns. Engine thread only.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining)]
        internal void Close()
        {
            _definition.Dispose();
            SetState(SessionProtocol.SessionState.Empty);
        }

        /// <summary>
        /// A stable hash of the input snapshot, as short hex.
        /// </summary>
        /// <remarks>
        /// It answers one question: did the solve run on the values the host
        /// thinks it did. Ids and values are length-prefixed before they are
        /// hashed so that {"ab","c"} and {"a","bc"} cannot collide — the classic
        /// concatenation bug, and one that would make two different snapshots
        /// look identical in exactly the diagnostics written to catch a race.
        /// </remarks>
        private static string HashInputs(IList<SessionProtocol.InputValue> inputs)
        {
            try
            {
                using (SHA256 sha = SHA256.Create())
                {
                    StringBuilder joined = new StringBuilder();
                    foreach (SessionProtocol.InputValue input in inputs)
                    {
                        Append(joined, input.Id);
                        Append(joined, input.Value);
                    }

                    byte[] digest = sha.ComputeHash(Encoding.UTF8.GetBytes(joined.ToString()));
                    StringBuilder text = new StringBuilder(32);
                    // Sixteen hex characters is plenty: this is a change
                    // detector read by a person in a diagnostics block, not a
                    // security boundary, and a full digest would crowd the line
                    // it shares with the definition identity.
                    for (int index = 0; index < 8; index++)
                    {
                        text.Append(digest[index].ToString("x2", CultureInfo.InvariantCulture));
                    }

                    return text.ToString();
                }
            }
            catch (Exception exception)
            {
                WorkerLog.Write("an input snapshot could not be hashed: " + WorkerLog.Describe(exception));
                return string.Empty;
            }
        }

        private static void Append(StringBuilder into, string text)
        {
            string safe = text ?? string.Empty;
            into.Append(safe.Length.ToString(CultureInfo.InvariantCulture));
            into.Append(':');
            into.Append(safe);
            into.Append('\n');
        }
    }
}
