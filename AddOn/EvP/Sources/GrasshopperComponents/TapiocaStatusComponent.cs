using System;
using System.Drawing;

using Grasshopper.Kernel;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// Answers "is Tapioca there, and is it this Archicad" with no typing.
    /// </summary>
    /// <remarks>
    /// It exists because the first question anyone has is whether the bridge is
    /// alive at all, and making them compose a JSON call to find out is a poor
    /// first minute. It is also the cheapest possible end-to-end proof that a
    /// component solving in Tapioca's worker process reached ACAPI in Archicad
    /// and came back, which is the claim this whole package rests on.
    /// </remarks>
    public class TapiocaStatusComponent : GH_Component
    {
        public TapiocaStatusComponent()
            : base(
                "Tapioca Status",
                "TapiocaOK",
                "Reports whether Tapioca's bridge to Archicad is available, and what it says.",
                "Tapioca",
                "Archicad")
        {
        }

        protected override void RegisterInputParams(GH_InputParamManager pManager)
        {
            pManager.AddBooleanParameter(
                "Refresh",
                "R",
                "Toggle to ask again.",
                GH_ParamAccess.item,
                true);
        }

        protected override void RegisterOutputParams(GH_OutputParamManager pManager)
        {
            pManager.AddBooleanParameter("Connected", "C", "Whether Archicad answered.", GH_ParamAccess.item);
            pManager.AddTextParameter("Status", "S", "Archicad's answer, as JSON.", GH_ParamAccess.item);
        }

        protected override void SolveInstance(IGH_DataAccess DA)
        {
            bool refresh = true;
            DA.GetData(0, ref refresh);

            string response = TapiocaBridge.Call("GetStatus", string.Empty);
            bool ok = Envelope.IsOk(response);

            DA.SetData(0, ok);
            DA.SetData(1, ok ? Envelope.DataOf(response) : Envelope.ErrorOf(response));

            if (!ok)
            {
                AddRuntimeMessage(GH_RuntimeMessageLevel.Warning, Envelope.ErrorOf(response));
            }
        }

        protected override Bitmap Icon
        {
            get { return null; }
        }

        public override Guid ComponentGuid
        {
            get { return new Guid("8d4b03f1-27ae-4c96-b5da-1e83f5c2a047"); }
        }
    }
}
