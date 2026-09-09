using System;
using System.Collections.Generic;
using System.Threading;
using System.Windows.Forms;

namespace Tapioca.GhWorker
{
    /// <summary>Owns every Rhino and Grasshopper operation on one STA thread.</summary>
    internal sealed class GhEngineThread : IDisposable
    {
        private readonly object _sync = new object();
        private readonly Queue<Action> _pending = new Queue<Action>();
        private readonly ManualResetEventSlim _started = new ManualResetEventSlim(false);
        private readonly ManualResetEventSlim _released = new ManualResetEventSlim(false);
        private readonly Thread _thread;
        private Func<StartOutcome> _bootstrap;
        private Control _marshaller;
        private StartOutcome _outcome;
        private bool _stopping;
        private bool _releasedForWork;

        internal GhEngineThread()
        {
            _thread = new Thread(Run);
            _thread.IsBackground = false;
            _thread.Name = "Tapioca Grasshopper engine";
            _thread.SetApartmentState(ApartmentState.STA);
        }

        internal StartOutcome Start(Func<StartOutcome> bootstrap)
        {
            if (bootstrap == null)
            {
                throw new ArgumentNullException(nameof(bootstrap));
            }

            _bootstrap = bootstrap;
            _thread.Start();
            _started.Wait();
            return _outcome;
        }

        internal bool Post(Action action)
        {
            if (action == null)
            {
                return false;
            }

            lock (_sync)
            {
                if (_stopping)
                {
                    return false;
                }

                if (_marshaller == null || !_releasedForWork)
                {
                    _pending.Enqueue(action);
                    return true;
                }

                return BeginInvokeLocked(action);
            }
        }

        // Program sends the startup acknowledgement before releasing queued
        // editor/run work, so that acknowledgement is always the host's runtime
        // readiness signal rather than whichever queued operation runs first.
        internal void Release()
        {
            lock (_sync)
            {
                _releasedForWork = true;
                _released.Set();
            }
        }

        internal void RequestStop()
        {
            lock (_sync)
            {
                if (_stopping)
                {
                    return;
                }

                _stopping = true;
                _released.Set();
                if (_marshaller == null)
                {
                    return;
                }

                BeginInvokeLocked(Application.ExitThread);
            }
        }

        internal void Join()
        {
            if (_thread.IsAlive)
            {
                _thread.Join();
            }
        }

        public void Dispose()
        {
            RequestStop();
            Join();
            _started.Dispose();
            _released.Dispose();
        }

        private void Run()
        {
            try
            {
                Control marshaller = new Control();
                marshaller.CreateControl();
                lock (_sync)
                {
                    _marshaller = marshaller;
                }

                _outcome = _bootstrap();
                _started.Set();
                if (_outcome.Kind != StartOutcomeKind.Started)
                {
                    return;
                }

                _released.Wait();
                lock (_sync)
                {
                    while (_pending.Count > 0)
                    {
                        BeginInvokeLocked(_pending.Dequeue());
                    }

                    if (_stopping)
                    {
                        BeginInvokeLocked(Application.ExitThread);
                    }
                }

                Application.Run();
            }
            catch (Exception exception)
            {
                _outcome = new StartOutcome(StartOutcomeKind.RhinoInitFailed, WorkerLog.Describe(exception));
                _started.Set();
                WorkerLog.Write("Grasshopper engine thread faulted: " + WorkerLog.Describe(exception));
            }
            finally
            {
                try
                {
                    WorkerLog.Write(WorkerSession.Stop());
                }
                catch (Exception exception)
                {
                    WorkerLog.Write("the session did not stop cleanly: " + WorkerLog.Describe(exception));
                }

                lock (_sync)
                {
                    _stopping = true;
                    _pending.Clear();
                    if (_marshaller != null)
                    {
                        _marshaller.Dispose();
                        _marshaller = null;
                    }
                }
                _started.Set();
            }
        }

        private bool BeginInvokeLocked(Action action)
        {
            try
            {
                _marshaller.BeginInvoke(new Action(() => Execute(action)));
                return true;
            }
            catch (Exception exception)
            {
                WorkerLog.Write("could not marshal to the Grasshopper engine: " + WorkerLog.Describe(exception));
                return false;
            }
        }

        private static void Execute(Action action)
        {
            try
            {
                action();
            }
            catch (Exception exception)
            {
                WorkerLog.Write("a Grasshopper engine operation failed: " + WorkerLog.Describe(exception));
            }
        }
    }
}
