using System;
using System.Collections.Generic;
using System.Drawing;

using Grasshopper.Kernel;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// Captures geometry for Archicad's viewport. Mechanism 1 of the three the
    /// handoff describes, and deliberately the first one built.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ EXPLICIT AND OPT-IN, WHICH IS THE POINT. What Archicad previews is
    /// exactly what was wired into this component — not everything the canvas
    /// happens to be drawing. That is what a Player definition wants: an author
    /// decides what the building model shows, rather than every intermediate
    /// result leaking into someone's viewport. The document-wide sweep that gives
    /// canvas parity is mechanism 2 and comes later, off by default.
    /// </para>
    /// <para>
    /// It also depends on no Grasshopper preview internals — only on the parameter
    /// data it is handed — which makes it the mechanism least likely to break
    /// across a Rhino service release, and the one buildable before the Grasshopper
    /// SDK reference lands.
    /// </para>
    /// <para>
    /// ⚠️ WHAT THIS DOES NOT DO YET. Nothing here reaches Archicad. Capture,
    /// conversion and the delta are complete and observable on the canvas; the
    /// transport and the Diligent layers are P1 and P2 of the preview work. The
    /// outputs below report what WOULD be sent, so this is verifiable now instead
    /// of after the whole pipeline exists. Do not read the delta counts as
    /// evidence that anything was drawn.
    /// </para>
    /// <para>
    /// ⚠️ CAPTURE MUST NEVER CAUSE A SOLVE. This reads the data it is given inside
    /// its own SolveInstance and expires nothing. A capture path that marks
    /// something dirty in order to draw it is a feedback loop that presents as
    /// "Archicad makes Grasshopper run continuously", and the cause is very hard to
    /// see from that symptom.
    /// </para>
    /// </remarks>
    public class TapiocaPreviewComponent : GH_Component
    {
        /// <summary>
        /// The mirror is per COMPONENT INSTANCE, so two preview components on one
        /// canvas keep separate diffs and neither reports the other's geometry as
        /// removed. Identity already includes the component guid, so the host's
        /// cache stays coherent across both.
        /// </summary>
        private readonly PreviewMirror _mirror = new PreviewMirror();

        public TapiocaPreviewComponent()
            : base(
                "Tapioca Preview",
                "TapiocaPrev",
                "Captures geometry for Archicad's viewport. What is wired here is what Archicad shows.",
                "Tapioca",
                "Archicad")
        {
        }

        protected override void RegisterInputParams(GH_InputParamManager pManager)
        {
            pManager.AddGenericParameter(
                "Geometry",
                "G",
                "Geometry to preview in Archicad. Meshes, breps, surfaces, curves, points, planes and "
                + "vectors are understood; anything else is counted as unsupported.",
                GH_ParamAccess.list);
            // ⚠️ HIDES GRASSHOPPER'S OWN PREVIEW OF THIS INPUT, NOT TAPIOCA'S.
            // HideParameter sets IGH_PreviewObject.Hidden on the input parameter
            // (Grasshopper/Kernel/GH_Component.cs), which is exactly what stock
            // Custom Preview does with its geometry input. Without it the wired
            // geometry is drawn TWICE — once by the parameter on Grasshopper's
            // canvas preview and once by Archicad — and the duplicate reads as a
            // z-fighting artefact rather than as two previews of one thing.
            pManager.HideParameter(0);
            pManager.AddBooleanParameter(
                "Visible",
                "V",
                "Clear to hide this component's preview in Archicad without removing it.",
                GH_ParamAccess.item,
                true);
            pManager[0].Optional = true;
        }

        protected override void RegisterOutputParams(GH_OutputParamManager pManager)
        {
            pManager.AddIntegerParameter(
                "Primitives", "P", "How many preview primitives this capture produced.", GH_ParamAccess.item);
            pManager.AddTextParameter(
                "Delta", "D", "What changed since the last solve.", GH_ParamAccess.item);
        }

        protected override void SolveInstance(IGH_DataAccess DA)
        {
            List<object> items = new List<object>();
            DA.GetDataList(0, items);

            bool visible = true;
            DA.GetData(1, ref visible);

            Guid parameterGuid = Params.Input.Count > 0 ? Params.Input[0].InstanceGuid : InstanceGuid;

            // ⚠️ THE BRANCH COMES FROM THE DATA ACCESS, NOT FROM A COUNTER. Identity
            // has to survive a re-solve that reorders nothing but arrives on a
            // different path, or the host sees a Remove plus an Add for geometry
            // that did not move.
            uint branchHash = PreviewHash.Branch(DA.ParameterTargetPath(0).Indices);

            List<PreviewPrimitive> captured = new List<PreviewPrimitive>();
            int unsupported = 0;
            for (int index = 0; index < items.Count; index++)
            {
                PreviewPrimitive primitive = PreviewConvert.Convert(
                    items[index], InstanceGuid, parameterGuid, branchHash, (uint)index);
                if (primitive == null)
                {
                    unsupported++;
                    continue;
                }

                if (!visible)
                {
                    primitive.Flags &= ~PreviewFlags.Visible;
                    // The content hash covers geometry, not flags, so it is left
                    // alone here on purpose: hiding must diff as Visibility, never
                    // as Changed.
                }

                captured.Add(primitive);
            }

            PreviewBatch batch = _mirror.Diff(captured);

            DA.SetData(0, captured.Count);
            DA.SetData(1, batch.Describe());

            if (unsupported > 0)
            {
                AddRuntimeMessage(
                    GH_RuntimeMessageLevel.Remark,
                    unsupported + " item(s) were not geometry Tapioca previews, and were skipped.");
            }

            // Said once, on the component, rather than in a log nobody opens: a
            // user wiring this up today should not be left wondering why Archicad
            // shows nothing.
            AddRuntimeMessage(
                GH_RuntimeMessageLevel.Remark,
                "Capture only. Preview does not reach Archicad's viewport yet.");
        }

        /// <summary>
        /// Forgets the mirror when the component leaves the document, so that
        /// re-adding it does not diff against a host cache that was dropped with it.
        /// </summary>
        public override void RemovedFromDocument(GH_Document document)
        {
            _mirror.DropAll();
            base.RemovedFromDocument(document);
        }

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
            get { return new Guid("7C4E1B92-3A6D-4F58-9E21-5D8B0A7C36F4"); }
        }
    }
}
