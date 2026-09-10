using System;
using System.Collections.Generic;
using System.Drawing;

using Grasshopper.Kernel;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// Connects this Grasshopper to an Archicad that is waiting for one.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ THE ANSWER TO "WHY MUST TAPIOCA START ITS OWN RHINO". It need not. Drop
    /// this on the canvas of a Rhino that is already open, and Archicad's panel
    /// drives THAT Grasshopper: no second Rhino, no second licence seat in play,
    /// and the user keeps the canvas, the plug-ins and the undo history they were
    /// already working in.
    /// </para>
    /// <para>
    /// ⚠️ A COMPONENT RATHER THAN AN AUTOMATIC CONNECTION, and the choice is
    /// about consent. The pipe only exists while somebody has pressed Connect in
    /// Archicad, so connecting on its own would not be dangerous — but it would
    /// mean a Grasshopper that quietly joins another application the moment this
    /// package loads, which is not a thing to do to a user's session without them
    /// putting it there. Placing the component IS the consent, and removing it
    /// (or setting Connect false) is how they withdraw it.
    /// </para>
    /// <para>
    /// It reports rather than throws, and it never fails the solve: a definition
    /// that cannot reach Archicad has to say so on the canvas.
    /// </para>
    /// </remarks>
    public class TapiocaLinkComponent : GH_Component
    {
        public TapiocaLinkComponent()
            : base(
                "Tapioca Link",
                "TapiocaLink",
                "Connects this Grasshopper to an Archicad that is waiting for one, so Tapioca's panel drives this "
                + "Rhino instead of starting its own.",
                "Tapioca",
                "Archicad")
        {
        }

        protected override void RegisterInputParams(GH_InputParamManager pManager)
        {
            // ⚠️ DEFAULTS TO FALSE. The moment this turns true this Rhino starts
            // answering another application's requests, including ones that load
            // documents into its Grasshopper. That is a decision, and a component
            // that arrived already making it would be making it for the user.
            pManager.AddBooleanParameter(
                "Connect",
                "C",
                "True to connect to a waiting Archicad; false to disconnect. This Rhino keeps running either way.",
                GH_ParamAccess.item,
                false);

            // Only needed with two Archicads open, and then it is needed rather
            // than convenient: see the refusal to guess in TapiocaPeer.Connect.
            pManager.AddTextParameter(
                "Pipe",
                "P",
                "Which waiting Archicad to connect to, by pipe name. Leave empty when only one is waiting.",
                GH_ParamAccess.item,
                string.Empty);
            pManager[1].Optional = true;
        }

        protected override void RegisterOutputParams(GH_OutputParamManager pManager)
        {
            pManager.AddBooleanParameter("Connected", "C", "Whether Archicad is on the line.", GH_ParamAccess.item);
            pManager.AddTextParameter("Status", "S", "What happened, in words.", GH_ParamAccess.item);
            pManager.AddTextParameter(
                "Waiting",
                "W",
                "Every Archicad on this machine currently waiting for a Grasshopper, by pipe name.",
                GH_ParamAccess.list);
        }

        protected override void SolveInstance(IGH_DataAccess DA)
        {
            bool connect = false;
            string pipe = string.Empty;
            DA.GetData(0, ref connect);
            DA.GetData(1, ref pipe);

            string status;
            if (connect)
            {
                TapiocaPeer.Connect(pipe, out status);
            }
            else
            {
                TapiocaPeer.Disconnect("this component was switched off");
                status = TapiocaPeer.Status;
            }

            bool connected = TapiocaPeer.IsConnected;
            DA.SetData(0, connected);
            DA.SetData(1, status);

            List<string> waiting = new List<string>();
            IList<TapiocaPeer.Candidate> candidates = TapiocaPeer.Discover();
            for (int index = 0; index < candidates.Count; index++)
            {
                waiting.Add(candidates[index].PipeName);
            }

            DA.SetDataList(2, waiting);

            // ⚠️ A WARNING, NOT AN ERROR, WHEN ASKED TO CONNECT AND NOT
            // CONNECTED. An error would go red and read as a broken definition;
            // the ordinary case here is that nobody has pressed Connect in
            // Archicad yet, which is not this definition's fault and is fixed
            // somewhere else entirely.
            if (connect && !connected)
            {
                AddRuntimeMessage(GH_RuntimeMessageLevel.Warning, status);
            }
        }

        /// <summary>
        /// Follows the connection for as long as this component is on a canvas.
        /// </summary>
        /// <remarks>
        /// ⚠️ SUBSCRIBED PER DOCUMENT, NOT ONCE PER PROCESS. Grasshopper adds
        /// and removes a component as documents open, close and are copied, and a
        /// handler left behind would hold a component whose document has gone.
        /// </remarks>
        public override void AddedToDocument(GH_Document document)
        {
            base.AddedToDocument(document);
            TapiocaPeer.StateChanged += OnPeerStateChanged;
        }

        public override void RemovedFromDocument(GH_Document document)
        {
            TapiocaPeer.StateChanged -= OnPeerStateChanged;
            base.RemovedFromDocument(document);
        }

        /// <remarks>
        /// <para>
        /// ⚠️ ARRIVES ON THE BRIDGE READER THREAD. A detach is Archicad's
        /// decision and reaches this peer between solves, so this is the one
        /// place a Grasshopper object is touched from a thread that is not
        /// Rhino's. Everything below therefore happens in two hops: onto Rhino's
        /// UI thread, then onto Grasshopper's own solution schedule.
        /// </para>
        /// <para>
        /// ⚠️ ScheduleSolution RATHER THAN ExpireSolution DIRECTLY. Expiring
        /// a component from outside a solution is the documented way to corrupt
        /// one -- Grasshopper may be mid-solve when the pipe closes, and the
        /// canvas would be re-entered underneath itself. ScheduleSolution hands
        /// the expiry back to Grasshopper to perform when it is between
        /// solutions, which is the only moment it is safe.
        /// </para>
        /// </remarks>
        private void OnPeerStateChanged()
        {
            try
            {
                PeerRhino.InvokeOnUiThread(ScheduleRefresh);
            }
            catch (Exception)
            {
                // A status that could not refresh itself is a stale reading, not
                // a reason to break the transport that reported the change.
            }
        }

        private void ScheduleRefresh()
        {
            GH_Document document = OnPingDocument();
            if (document == null)
            {
                return;
            }

            document.ScheduleSolution(RefreshDelayMs, ExpireForRefresh);
        }

        private void ExpireForRefresh(GH_Document document)
        {
            // false: the scheduled solution is already coming, and asking for a
            // second one here is how a schedule callback turns into a loop.
            ExpireSolution(false);
        }

        /// <summary>
        /// Long enough to coalesce a connect that changes the status twice,
        /// short enough that a detach is visible before anyone looks away.
        /// </summary>
        private const int RefreshDelayMs = 20;

        /// <remarks>
        /// ⚠️ NEVER PARALLEL AND NEVER ON A SECOND THREAD. Connecting binds
        /// process-wide statics -- the bridge, the session router, the log sink --
        /// and does it on whichever thread solves this component.
        /// </remarks>
        public override GH_Exposure Exposure
        {
            get { return GH_Exposure.primary; }
        }

        protected override Bitmap Icon
        {
            get { return null; }
        }

        public override Guid ComponentGuid
        {
            get { return new Guid("6c1f9d84-3b52-4a17-9c0e-5f8d2a41b7e3"); }
        }
    }
}
