using System;
using System.Drawing;

using Grasshopper.Kernel;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// Runs any Tapioca command by name and returns its JSON.
    /// </summary>
    /// <remarks>
    /// <para>
    /// The first component of the package, and deliberately the general one: the
    /// add-on already registers a catalogue of native commands, so one component
    /// that can call any of them makes the whole existing surface reachable from
    /// the canvas today, without a component per command. Typed components are
    /// worth adding for the calls people reach for daily; they are not worth
    /// waiting for before anything works at all.
    /// </para>
    /// <para>
    /// ⚠️ THE CALL RUNS INLINE ON ARCHICAD'S MAIN THREAD, which is where
    /// SolveInstance already is, and that is the whole reason this package
    /// exists. No socket, no scheduling, no timeout. Do not move this work to a
    /// task: the add-on checks the thread and will refuse it.
    /// </para>
    /// </remarks>
    public class TapiocaCallComponent : GH_Component
    {
        public TapiocaCallComponent()
            : base(
                "Tapioca Call",
                "Tapioca",
                "Runs a Tapioca command inside Archicad and returns its JSON response.",
                "Tapioca",
                "Archicad")
        {
        }

        protected override void RegisterInputParams(GH_InputParamManager pManager)
        {
            pManager.AddTextParameter(
                "Command",
                "C",
                "Command name, for example Tapioca.GetStatus or GetStatus.",
                GH_ParamAccess.item,
                "Tapioca.GetStatus");
            pManager.AddTextParameter(
                "Parameters",
                "P",
                "Parameters as a JSON object. Empty means none.",
                GH_ParamAccess.item,
                string.Empty);
            pManager.AddBooleanParameter(
                "Run",
                "R",
                "Set to true to call Archicad. Left false, the component does nothing.",
                GH_ParamAccess.item,
                false);
            pManager[1].Optional = true;
        }

        protected override void RegisterOutputParams(GH_OutputParamManager pManager)
        {
            pManager.AddBooleanParameter("Ok", "Ok", "Whether the command succeeded.", GH_ParamAccess.item);
            pManager.AddTextParameter("Data", "D", "The response payload, as JSON.", GH_ParamAccess.item);
            pManager.AddTextParameter("Error", "E", "The failure message, when there is one.", GH_ParamAccess.item);
        }

        protected override void SolveInstance(IGH_DataAccess DA)
        {
            string command = null;
            string parameters = null;
            bool run = false;

            if (!DA.GetData(0, ref command))
            {
                return;
            }

            DA.GetData(1, ref parameters);
            if (!DA.GetData(2, ref run))
            {
                return;
            }

            if (!run)
            {
                // A remark, not a warning: not having pressed Run yet is a normal
                // state for a component that talks to a live model, and a canvas
                // full of orange balloons trains people to ignore them.
                AddRuntimeMessage(GH_RuntimeMessageLevel.Remark, "Set Run to true to call Archicad.");
                return;
            }

            string response = TapiocaBridge.Call(command, parameters);
            bool ok = Envelope.IsOk(response);

            DA.SetData(0, ok);
            DA.SetData(1, ok ? Envelope.DataOf(response) : "{}");
            DA.SetData(2, ok ? string.Empty : Envelope.ErrorOf(response));

            if (!ok)
            {
                AddRuntimeMessage(GH_RuntimeMessageLevel.Error, Envelope.ErrorOf(response));
            }
        }

        protected override Bitmap Icon
        {
            get { return null; }
        }

        public override Guid ComponentGuid
        {
            get { return new Guid("2f7a1c64-9b3d-4f0e-8a51-6c2d9e4b7a10"); }
        }
    }
}
